#include "platform/clock.h"
#include "core/session.h"
#include "platform/journal_io.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <exception>
#include <fstream>
#include <sstream>
#ifndef _WIN32
#include <thread>
#include <unistd.h>
#endif

namespace spark {
namespace {

void RecordTurnDuration(SessionState& state, const nlohmann::json& payload) {
    if (!state.metadata.is_object()) state.metadata = nlohmann::json::object();
    if (!state.metadata.contains("turn_durations_ms") || !state.metadata["turn_durations_ms"].is_array()) {
        state.metadata["turn_durations_ms"] = nlohmann::json::array();
    }

    const int64_t explicit_index = payload.value("turn_index", int64_t{-1});
    size_t turn_index = 0;
    if (explicit_index >= 0) {
        turn_index = static_cast<size_t>(explicit_index);
    } else {
        const size_t user_turns = static_cast<size_t>(
            std::count_if(state.messages.begin(), state.messages.end(), [](const ChatMessage& message) {
                return message.role == MessageRole::User;
            }));
        turn_index = user_turns == 0 ? 0 : user_turns - 1;
    }

    nlohmann::json& durations = state.metadata["turn_durations_ms"];
    while (durations.size() <= turn_index) durations.push_back(uint64_t{0});
    durations[turn_index] = payload.value("elapsed_ms", uint64_t{0});
}

void RecordTurnUsage(SessionState& state, const nlohmann::json& payload) {
    if (!state.metadata.is_object()) state.metadata = nlohmann::json::object();
    if (!state.metadata.contains("turn_usages") || !state.metadata["turn_usages"].is_array()) {
        state.metadata["turn_usages"] = nlohmann::json::array();
    }

    const int64_t explicit_index = payload.value("turn_index", int64_t{-1});
    size_t turn_index = 0;
    if (explicit_index >= 0) {
        turn_index = static_cast<size_t>(explicit_index);
    } else {
        const size_t user_turns = static_cast<size_t>(
            std::count_if(state.messages.begin(), state.messages.end(), [](const ChatMessage& message) {
                return message.role == MessageRole::User;
            }));
        turn_index = user_turns == 0 ? 0 : user_turns - 1;
    }

    nlohmann::json usage = {
        {"input_tokens", payload.value("input_tokens", uint64_t{0})},
        {"output_tokens", payload.value("output_tokens", uint64_t{0})},
        {"cached_input_tokens", payload.value("cached_input_tokens", uint64_t{0})},
        {"model_calls", payload.value("model_calls", uint64_t{0})},
        {"context_tokens", payload.value("context_tokens", uint64_t{0})},
        {"context_limit_tokens", payload.value("context_limit_tokens", uint64_t{0})},
        {"elapsed_ms", payload.value("elapsed_ms", uint64_t{0})},
    };
    nlohmann::json& usages = state.metadata["turn_usages"];
    while (usages.size() <= turn_index) usages.push_back(nlohmann::json::object());
    usages[turn_index] = std::move(usage);
}

void RecordCompaction(SessionState& state, const nlohmann::json& payload) {
    if (!state.metadata.is_object()) state.metadata = nlohmann::json::object();
    if (!state.metadata.contains("context_compactions") || !state.metadata["context_compactions"].is_array()) {
        state.metadata["context_compactions"] = nlohmann::json::array();
    }
    state.metadata["context_compactions"].push_back({
        {"trigger", payload.value("trigger", std::string("auto"))},
        {"message_count", payload.value("message_count", uint64_t{0})},
        {"before_tokens", payload.value("before_tokens", uint64_t{0})},
        {"after_tokens", payload.value("after_tokens", uint64_t{0})},
    });
}

void RecordPermission(SessionState& state, const nlohmann::json& payload) {
    if (!state.metadata.is_object()) state.metadata = nlohmann::json::object();
    const std::string mode = LowerAscii(Trim(payload.value("mode", std::string("ask"))));
    state.metadata["permission_mode"] =
        (mode == "full_access" || mode == "approve_for_me" || mode == "read_only") ? mode : "ask";
}

void RecordToolExecution(SessionState& state, const nlohmann::json& payload) {
    const std::string call_id = Trim(payload.value("tool_call_id", std::string{}));
    if (call_id.empty()) return;
    if (!state.metadata.is_object()) state.metadata = nlohmann::json::object();
    if (!state.metadata.contains("tool_durations_ms") || !state.metadata["tool_durations_ms"].is_object()) {
        state.metadata["tool_durations_ms"] = nlohmann::json::object();
    }
    state.metadata["tool_durations_ms"][call_id] = payload.value("elapsed_ms", uint64_t{0});
}

void RecordTurnError(SessionState& state, const nlohmann::json& payload, uint64_t event_timestamp) {
    if (!state.metadata.is_object()) state.metadata = nlohmann::json::object();
    if (!state.metadata.contains("turn_errors") || !state.metadata["turn_errors"].is_array()) {
        state.metadata["turn_errors"] = nlohmann::json::array();
    }

    const int64_t explicit_index = payload.value("turn_index", int64_t{-1});
    size_t turn_index = 0;
    if (explicit_index >= 0) {
        turn_index = static_cast<size_t>(explicit_index);
    } else {
        const size_t user_turns = static_cast<size_t>(
            std::count_if(state.messages.begin(), state.messages.end(), [](const ChatMessage& message) {
                return message.role == MessageRole::User;
            }));
        turn_index = user_turns == 0 ? 0 : user_turns - 1;
    }

    nlohmann::json record = payload;
    record["turn_index"] = turn_index;
    if (!record.contains("timestamp")) record["timestamp"] = event_timestamp;
    auto& errors = state.metadata["turn_errors"];
    errors.erase(std::remove_if(errors.begin(), errors.end(), [turn_index](const nlohmann::json& error) {
                     return error.is_object() &&
                            error.value("turn_index", int64_t{-1}) == static_cast<int64_t>(turn_index);
                 }),
                 errors.end());
    errors.push_back(std::move(record));
}

void EnsureLiveTurn(SessionState& state) {
    if (!state.metadata.is_object()) state.metadata = nlohmann::json::object();
    if (!state.metadata.contains("live_turn") || !state.metadata["live_turn"].is_object()) {
        state.metadata["live_turn"] = {
            {"active", true},
            {"started_tick", uint64_t{0}},
            {"turn_index", uint64_t{0}},
            {"phase", "waiting_model"},
            {"status", ""},
            {"text", ""},
            {"reasoning", ""},
            {"tools", nlohmann::json::array()},
        };
    }
}

void RecordTurnStarted(SessionState& state, const nlohmann::json& payload) {
    EnsureLiveTurn(state);
    nlohmann::json& live = state.metadata["live_turn"];
    live["active"] = true;
    live["started_tick"] = payload.value("started_tick", uint64_t{0});
    live["elapsed_ms"] = payload.value("elapsed_ms", uint64_t{0});
    live["turn_index"] = payload.value("turn_index", uint64_t{0});
    live["phase"] = payload.value("phase", std::string("waiting_model"));
    live["status"] = payload.value("status", std::string{});
    live["text"] = "";
    live["reasoning"] = "";
    live["tools"] = nlohmann::json::array();
}

void RecordLiveDelta(SessionState& state, const std::string& field, const nlohmann::json& payload) {
    EnsureLiveTurn(state);
    nlohmann::json& live = state.metadata["live_turn"];
    if (!live[field].is_string()) live[field] = "";
    std::string combined = live[field].get<std::string>() + payload.value("delta", std::string{});
    // Live previews are bounded; the completed message remains authoritative.
    const size_t limit = field == "reasoning" ? 16384 : 1024 * 1024;
    if (field == "reasoning" && combined.size() > limit) {
        size_t start = combined.size() - limit;
        while (start < combined.size() && (static_cast<unsigned char>(combined[start]) & 0xc0) == 0x80) ++start;
        combined.erase(0, start);
    }
    live[field] = TruncateUtf8(std::move(combined), limit);
}

void RecordLivePhase(SessionState& state, const nlohmann::json& payload) {
    EnsureLiveTurn(state);
    nlohmann::json& live = state.metadata["live_turn"];
    live["phase"] = payload.value("phase", nlohmann::json("waiting_model"));
    live["status"] = payload.value("detail", std::string{});
}

void RecordLiveToolStart(SessionState& state, const nlohmann::json& payload) {
    EnsureLiveTurn(state);
    nlohmann::json& tools = state.metadata["live_turn"]["tools"];
    if (!tools.is_array()) tools = nlohmann::json::array();
    const std::string call_id = payload.value("tool_call_id", std::string{});
    for (const auto& item : tools) {
        if (item.is_object() && item.value("tool_call_id", std::string{}) == call_id) return;
    }
    tools.push_back({
        {"tool_call_id", call_id},
        {"tool_name", payload.value("tool_name", std::string("tool"))},
        {"arguments", payload.value("arguments", std::string{})},
        {"started_tick", payload.value("started_tick", uint64_t{0})},
        {"finished", false},
        {"success", false},
        {"output", ""},
        {"elapsed_ms", uint64_t{0}},
    });
}

void RecordLiveToolFinish(SessionState& state, const nlohmann::json& payload) {
    EnsureLiveTurn(state);
    nlohmann::json& tools = state.metadata["live_turn"]["tools"];
    if (!tools.is_array()) return;
    const std::string call_id = payload.value("tool_call_id", std::string{});
    for (auto& item : tools) {
        if (!item.is_object() || item.value("tool_call_id", std::string{}) != call_id) continue;
        item["finished"] = true;
        item["success"] = payload.value("success", false);
        item["output"] = payload.value("output", std::string{});
        item["elapsed_ms"] = payload.value("elapsed_ms", uint64_t{0});
        return;
    }
}

void ClearLiveTurn(SessionState& state) {
    if (!state.metadata.is_object()) return;
    state.metadata.erase("live_turn");
}

bool ApplyLiveEvent(SessionState& state, const std::string& type, const nlohmann::json& payload) {
    if (type == "turn.started") RecordTurnStarted(state, payload);
    else if (type == "turn.text_delta") RecordLiveDelta(state, "text", payload);
    else if (type == "turn.reasoning_delta") RecordLiveDelta(state, "reasoning", payload);
    else if (type == "turn.phase") RecordLivePhase(state, payload);
    else if (type == "turn.tool_start") RecordLiveToolStart(state, payload);
    else if (type == "turn.tool_finish") RecordLiveToolFinish(state, payload);
    else if (type == "turn.ended") ClearLiveTurn(state);
    else if (type == "turn.live_reset") {
        EnsureLiveTurn(state);
        state.metadata["live_turn"]["text"] = "";
        state.metadata["live_turn"]["reasoning"] = "";
    } else return false;
    return true;
}

void RemoveLiveTool(SessionState& state, const std::string& call_id) {
    if (!state.metadata.is_object() || !state.metadata.contains("live_turn") ||
        !state.metadata["live_turn"].is_object()) {
        return;
    }
    nlohmann::json& tools = state.metadata["live_turn"]["tools"];
    if (!tools.is_array()) return;
    for (auto iterator = tools.begin(); iterator != tools.end(); ++iterator) {
        if (iterator->is_object() && iterator->value("tool_call_id", std::string{}) == call_id) {
            tools.erase(iterator);
            break;
        }
    }
}

nlohmann::json StateToJson(const SessionState& state) {
    return {
        {"id", state.id},
        {"title", state.title},
        {"workspace", state.workspace},
        {"created_at", state.created_at},
        {"updated_at", state.updated_at},
        {"messages", state.messages},
        {"context_messages", state.context_messages},
        {"metadata", state.metadata},
    };
}

SessionState StateFromJson(const nlohmann::json& input) {
    SessionState state;
    state.id = input.value("id", std::string{});
    state.title = input.value("title", std::string{});
    state.workspace = input.value("workspace", std::string{});
    state.created_at = input.value("created_at", uint64_t{0});
    state.updated_at = input.value("updated_at", uint64_t{0});
    if (input.contains("messages") && input["messages"].is_array()) {
        state.messages = input["messages"].get<std::vector<ChatMessage>>();
    }
    if (input.contains("context_messages") && input["context_messages"].is_array()) {
        state.context_messages = input["context_messages"].get<std::vector<ChatMessage>>();
    }
    state.metadata = input.value("metadata", nlohmann::json::object());
    return state;
}

std::string LocalDateStamp() {
#ifdef _WIN32
    SYSTEMTIME local{};
    GetLocalTime(&local);
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u", local.wYear, local.wMonth, local.wDay);
#else
    const auto now = std::time(nullptr);
    std::tm local{};
    if (!localtime_r(&now, &local)) return {};
    char buffer[16]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local);
#endif
    return buffer;
}

std::string ConversationFolderName() {
#ifdef _WIN32
    SYSTEMTIME local{};
    GetLocalTime(&local);
    char buffer[80]{};
    const unsigned long counter = static_cast<unsigned long>(spark::MonotonicMillis() & 0xffffffffUL);
    std::snprintf(buffer, sizeof(buffer), "chat-%04u%02u%02u-%02u%02u%02u-%08lx", local.wYear, local.wMonth,
                  local.wDay, local.wHour, local.wMinute, local.wSecond, counter);
    return buffer;
#else
    return "chat-" + NewSessionId();
#endif
}

}  // namespace

const std::vector<ChatMessage>& ModelContextMessages(const SessionState& state) {
    return state.context_messages.empty() ? state.messages : state.context_messages;
}

SessionJournal::~SessionJournal() = default;

Result<void> SessionJournal::Open(const std::string& data_directory, const std::string& workspace) {
    state_ = {};
    state_.workspace = AbsolutePath(workspace);
    state_.created_at = UnixTimeSeconds();
    state_.updated_at = state_.created_at;
    state_.title = "新对话";
    data_directory_ = AbsolutePath(data_directory);
    std::error_code error;
    spark::fs::create_directories(NativePath(JoinPath(data_directory_, "sessions")), error);
    if (error) {
        return Result<void>::Failure("could not create session directory: " + error.message());
    }
    do {
        state_.id = NewSessionId();
        path_ = JoinPath(JoinPath(data_directory_, "sessions"), state_.id + ".jsonl");
    } while (PathExists(path_));
    const nlohmann::json payload = {
        {"session_id", state_.id},
        {"workspace", state_.workspace},
        {"created_at", state_.created_at},
    };
    return Append("session.created", payload);
}

Result<void> SessionJournal::Resume(const std::string& journal_path, const CancellationToken* cancellation) {
    if (cancellation != nullptr && cancellation->IsCancelled()) return Result<void>::Failure("cancelled");
    path_ = AbsolutePath(journal_path);
    // A resumed journal must keep its storage root, otherwise ListRecent() silently searches an
    // empty directory and every conversation disappears from the sidebar after a restart.
    const std::string directory = ParentPath(path_);
    const std::string storage_root = ParentPath(directory);
    data_directory_ = IsDirectory(JoinPath(storage_root, "sessions")) ? storage_root : directory;
    try {
        // Stream the journal instead of holding the file and a second copy of every line.
        spark::fs_ifstream stream(NativePath(path_), std::ios::binary);
        if (!stream) return Result<void>::Failure("could not open session journal");
        return LoadEvents(stream, cancellation);
    } catch (const std::exception& exception) {
        return Result<void>::Failure("could not parse session: " + std::string(exception.what()));
    } catch (...) {
        return Result<void>::Failure("could not parse session: unknown error");
    }
}

void SessionJournal::SetStorageRoot(const std::string& data_directory) {
    if (data_directory.empty()) return;
    data_directory_ = AbsolutePath(data_directory);
}

Result<void> SessionJournal::LoadEvents(std::istream& stream, const CancellationToken* cancellation) {
    state_ = {};
    bool context_override = false;
    nlohmann::json replayed_durations = nlohmann::json::array();
    nlohmann::json replayed_usages = nlohmann::json::array();
    nlohmann::json replayed_tool_durations = nlohmann::json::object();
    nlohmann::json replayed_turn_errors = nlohmann::json::array();
    std::string scope;
    std::string line;
    bool first_line = true;
    for (;;) {
        if (cancellation != nullptr && cancellation->IsCancelled()) return Result<void>::Failure("cancelled");
        if (!std::getline(stream, line)) break;
        if (first_line && line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
        first_line = false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        try {
            const nlohmann::json event = nlohmann::json::parse(line);
            const std::string type = event.value("type", std::string{});
            const nlohmann::json payload = event.value("payload", nlohmann::json::object());
            const uint64_t event_timestamp = event.value("timestamp", uint64_t{0});
            if (ApplyLiveEvent(state_, type, payload)) {
            } else if (type == "session.created") {
                state_.id = payload.value("session_id", std::string{});
                state_.workspace = payload.value("workspace", std::string{});
                state_.created_at = payload.value("created_at", uint64_t{0});
                state_.title = payload.value("title", std::string("新对话"));
            } else if (type == "session.title") {
                state_.title = payload.value("title", state_.title);
            } else if (type == "session.permission") {
                RecordPermission(state_, payload);
            } else if (type == "session.scope") {
                scope = payload.value("scope", std::string{});
            } else if (type == "message") {
                ChatMessage message = payload.get<ChatMessage>();
                if (message.timestamp == 0) message.timestamp = event_timestamp;
                state_.messages.push_back(message);
                if (context_override) state_.context_messages.push_back(std::move(message));
            } else if (type == "context.compacted") {
                RecordCompaction(state_, payload);
                // The following legacy snapshot stores only the compacted model context. Keep
                // every earlier message event as the visible transcript.
                context_override = true;
                state_.context_messages.clear();
                ChatMessage handoff;
                handoff.role = MessageRole::System;
                handoff.content = "Conversation history handoff summary:\n" +
                                  payload.value("summary", std::string{});
                state_.context_messages.push_back(std::move(handoff));
            } else if (type == "turn.completed" || type == "turn.duration") {
                RecordTurnDuration(state_, payload);
                replayed_durations = state_.metadata["turn_durations_ms"];
            } else if (type == "turn.usage") {
                RecordTurnUsage(state_, payload);
                replayed_usages = state_.metadata["turn_usages"];
            } else if (type == "tool.execution") {
                RecordToolExecution(state_, payload);
                replayed_tool_durations = state_.metadata["tool_durations_ms"];
            } else if (type == "turn.error" || type == "turn.dsml.error") {
                RecordTurnError(state_, payload, event_timestamp);
                replayed_turn_errors = state_.metadata["turn_errors"];
            } else if (type == "snapshot") {
                SessionState snapshot = StateFromJson(payload);
                const bool snapshot_has_context =
                    payload.contains("context_messages") && payload["context_messages"].is_array();
                if (context_override && !snapshot_has_context) {
                    snapshot.context_messages = std::move(snapshot.messages);
                    snapshot.messages = std::move(state_.messages);
                } else {
                    // Sessions written before message timestamps were added still
                    // carry the authoritative time on each JSONL event. Preserve
                    // those values when a later snapshot replaces the transcript.
                    for (size_t message_index = 0; message_index < snapshot.messages.size(); ++message_index) {
                        ChatMessage& message = snapshot.messages[message_index];
                        if (message.timestamp != 0) continue;
                        if (message_index < state_.messages.size() && state_.messages[message_index].role == message.role &&
                            state_.messages[message_index].content == message.content) {
                            message.timestamp = state_.messages[message_index].timestamp;
                        } else {
                            message.timestamp = event_timestamp;
                        }
                    }
                }
                context_override = !snapshot.context_messages.empty();
                state_ = std::move(snapshot);
                if (!state_.metadata.is_object()) state_.metadata = nlohmann::json::object();
                // Missing keys belong to legacy snapshots. Present keys, including
                // empty arrays, authoritatively replace the previous branch.
                const auto restore_metadata = [&](const char* key, nlohmann::json& replayed) {
                    if (!state_.metadata.contains(key)) state_.metadata[key] = replayed;
                    replayed = state_.metadata[key];
                };
                restore_metadata("turn_durations_ms", replayed_durations);
                restore_metadata("turn_usages", replayed_usages);
                restore_metadata("tool_durations_ms", replayed_tool_durations);
                restore_metadata("turn_errors", replayed_turn_errors);
                if (replayed_turn_errors.is_array()) {
                    state_.metadata["turn_errors"] = nlohmann::json::array();
                    for (const auto& error : replayed_turn_errors) {
                        if (error.is_object()) RecordTurnError(state_, error, event_timestamp);
                    }
                    replayed_turn_errors = state_.metadata["turn_errors"];
                }
            }
            state_.updated_at = event_timestamp == 0 ? state_.updated_at : event_timestamp;
        } catch (const std::exception&) {
            // A damaged tail must not prevent opening earlier, valid events.
        }
    }
    if (stream.bad()) return Result<void>::Failure("could not read session journal");
    if (!scope.empty()) state_.metadata["scope"] = scope;
    // Older builds could clear paused_turn during a failed resume attempt while leaving the
    // interrupted-turn marker as the final transcript message. That made the play button disappear
    // and turned an empty-composer resume click into a no-op. Recover that state on load so a stuck
    // conversation can still be resumed.
    if (!state_.messages.empty() && state_.messages.back().role == MessageRole::System &&
        state_.messages.back().content == kInterruptedTurnMarker &&
        (!state_.metadata.is_object() || !state_.metadata.contains("paused_turn"))) {
        if (!state_.metadata.is_object()) state_.metadata = nlohmann::json::object();
        uint64_t elapsed_ms = 0;
        if (state_.metadata.contains("turn_durations_ms") &&
            state_.metadata["turn_durations_ms"].is_array() &&
            !state_.metadata["turn_durations_ms"].empty() &&
            state_.metadata["turn_durations_ms"].back().is_number_unsigned()) {
            elapsed_ms = state_.metadata["turn_durations_ms"].back().get<uint64_t>();
        }
        state_.metadata["paused_turn"] = {
            {"elapsed_ms", elapsed_ms},
            {"interrupted_at", state_.updated_at},
        };
    }
    if (state_.id.empty()) {
        return Result<void>::Failure("session journal has no readable session header");
    }
    if (state_.workspace.empty()) {
        state_.workspace = CurrentDirectoryUtf8();
    }
    return Result<void>::Success();
}

Result<void> SessionJournal::Append(const std::string& event_type, const nlohmann::json& payload) {
    if (path_.empty()) {
        return Result<void>::Failure("session journal is not open");
    }
    const nlohmann::json event = {
        {"timestamp", UnixTimeSeconds()},
        {"type", event_type},
        {"payload", payload},
    };
    std::string error;
    if (!AppendJournalRecord(path_, event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n", &error)) {
        return Result<void>::Failure("could not append session event: " + error);
    }
    state_.updated_at = event["timestamp"].get<uint64_t>();
    if (ApplyLiveEvent(state_, event_type, payload)) {
    } else if (event_type == "turn.completed" || event_type == "turn.duration") {
        RecordTurnDuration(state_, payload);
    } else if (event_type == "turn.usage") {
        RecordTurnUsage(state_, payload);
    } else if (event_type == "context.compacted") {
        RecordCompaction(state_, payload);
    } else if (event_type == "session.permission") {
        RecordPermission(state_, payload);
    } else if (event_type == "tool.execution") {
        RecordToolExecution(state_, payload);
    } else if (event_type == "turn.error" || event_type == "turn.dsml.error") {
        RecordTurnError(state_, payload, event["timestamp"].get<uint64_t>());
    }
    return Result<void>::Success();
}

Result<void> SessionJournal::SaveSnapshot() {
    if (path_.empty()) {
        return Result<void>::Failure("session journal is not open");
    }
    return Append("snapshot", StateToJson(state_));
}

Result<std::vector<std::string>> SessionJournal::ListRecent(int limit) const {
    return RecentJournalFiles(JoinPath(data_directory_, "sessions"), limit);
}

std::string NewSessionId() {
    static std::atomic<uint64_t> sequence{0};
    const uint64_t now = UnixTimeSeconds();
    const uint64_t suffix = sequence.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream stream;
    stream << "session-" << now << "-" << std::hex;
#ifdef _WIN32
    stream << GetCurrentProcessId() << "-" << GetCurrentThreadId() << "-" << spark::MonotonicMillis();
#else
    stream << getpid() << "-" << std::hash<std::thread::id>{}(std::this_thread::get_id()) << "-"
           << std::chrono::steady_clock::now().time_since_epoch().count();
#endif
    stream << "-" << suffix;
    return stream.str();
}

std::string CreateNoProjectConversationWorkspace(const std::string& base_directory, std::string* error) {
    const std::string base = AbsolutePath(base_directory);
    const std::string conversation = JoinPath(JoinPath(base, LocalDateStamp()), ConversationFolderName());
    std::error_code filesystem_error;
    spark::fs::create_directories(NativePath(JoinPath(conversation, "work")), filesystem_error);
    if (filesystem_error) {
        if (error != nullptr) *error = filesystem_error.message();
        return {};
    }
    spark::fs::create_directories(NativePath(JoinPath(conversation, "outputs")), filesystem_error);
    if (filesystem_error) {
        if (error != nullptr) *error = filesystem_error.message();
        return {};
    }
    return conversation;
}

}  // namespace spark
