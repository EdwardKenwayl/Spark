#include "gui/transcript.h"

#include <algorithm>
#include <fstream>
#include <map>

namespace spark {
namespace {
enum class Activity { Command, Read, Search, Image, Write, Other };
Activity Category(const std::string& name) {
    if (name == "run_command") return Activity::Command;
    const auto lower = LowerAscii(name);
    for (const char* word : {"image", "vision", "screenshot", "photo", "picture", "ocr"}) {
        if (lower.find(word) != std::string::npos) return Activity::Image;
    }
    if (name == "read_file" || name == "list_files" || name == "read_skill") return Activity::Read;
    if (name == "search_text") return Activity::Search;
    if (name == "write_file" || name == "apply_patch") return Activity::Write;
    return Activity::Other;
}
bool CanGroup(const ChatItem& left, const ChatItem& right) {
    const auto a = Category(left.tool_name), b = Category(right.tool_name);
    if (a <= Activity::Search && b <= Activity::Search) return true;
    return a == b && (a != Activity::Other || LowerAscii(left.tool_name) == LowerAscii(right.tool_name));
}
size_t TurnEnd(const std::vector<ChatItem>& items, size_t index) {
    for (size_t end = index + 1; end < items.size(); ++end) {
        if (items[end].kind == ChatItemKind::User || items[end].kind == ChatItemKind::TurnStatus) return end;
    }
    return items.size();
}
std::string ReasoningText(const std::string& text) {
    auto output = SanitizeDisplayText(text);
    if (output.size() > 16384) {
        size_t start = output.size() - 16384;
        while (start < output.size() && (static_cast<unsigned char>(output[start]) & 0xc0) == 0x80) ++start;
        output.erase(0, start);
    }
    return output;
}
const nlohmann::json& Metadata(const SessionState& state, const char* key) {
    static const nlohmann::json empty;
    const auto found = state.metadata.find(key);
    return found == state.metadata.end() ? empty : *found;
}
uint64_t Number(const nlohmann::json& value) {
    return value.is_number_unsigned() ? value.get<uint64_t>() : 0;
}
void ConsumeSavedText(std::string& live, const std::string& saved) {
    // A polling snapshot may overlap either a complete reply or its last streamed prefix.
    if (live.compare(0, saved.size(), saved) == 0) live.erase(0, saved.size());
    else if (saved.compare(0, live.size(), live) == 0) live.clear();
}
}  // namespace

struct Transcript::Replay {
    SessionState state;
    TranscriptTime now;
    TranscriptRun run;
    size_t message = 0, turn = 0, total_turns = 0, compaction = 0;
    size_t pending_status = kNoChatItem, pending_turn = kNoChatItem;
    size_t live_turn = kNoChatItem, live_tool = 0;
    std::string live_text, live_reasoning;
    std::map<std::string, size_t> tools, turn_tools;

    void FinishPending(Transcript& model, bool paused = false) {
        if (pending_status == kNoChatItem) return;
        ChatItem completion;
        const auto& durations = Metadata(state, "turn_durations_ms");
        completion.elapsed_ms = durations.is_array() && pending_turn < durations.size() ? Number(durations[pending_turn]) : 0;
        if (paused) completion.elapsed_ms = Metadata(state, "paused_turn").value("elapsed_ms", uint64_t{0});
        model.Apply({paused ? TranscriptEventKind::PauseTurn : TranscriptEventKind::FinishTurn,
                     pending_status, completion}, now);
        const auto& errors = Metadata(state, "turn_errors");
        if (errors.is_array()) for (const auto& error : errors) {
            if (!error.is_object() || error.value("turn_index", int64_t{-1}) != static_cast<int64_t>(pending_turn)) continue;
            ChatItem item;
            item.text = error.value("error", std::string{});
            if (item.text.empty()) continue;
            item.kind = IsQuotaExhaustedError(item.text) ? ChatItemKind::Quota : ChatItemKind::Error;
            item.timestamp = error.value("timestamp", uint64_t{0});
            model.Apply({TranscriptEventKind::Add, kNoChatItem, item}, now);
        }
        const auto& usages = Metadata(state, "turn_usages");
        if (usages.is_array() && pending_turn < usages.size() && usages[pending_turn].is_object()) {
            const auto& usage = usages[pending_turn];
            ChatItem item;
            item.kind = ChatItemKind::UsageStats;
            item.input_tokens = usage.value("input_tokens", uint64_t{0});
            item.output_tokens = usage.value("output_tokens", uint64_t{0});
            item.cached_input_tokens = usage.value("cached_input_tokens", uint64_t{0});
            item.model_calls = usage.value("model_calls", uint64_t{0});
            item.context_tokens = usage.value("context_tokens", uint64_t{0});
            item.context_limit_tokens = usage.value("context_limit_tokens", uint64_t{0});
            item.elapsed_ms = usage.value("elapsed_ms", uint64_t{0});
            model.Apply({TranscriptEventKind::Add, kNoChatItem, item}, now);
        }
        pending_status = pending_turn = kNoChatItem;
    }

    void CompactionsThrough(Transcript& model, size_t count) {
        const auto& events = Metadata(state, "context_compactions");
        if (!events.is_array()) return;
        while (compaction < events.size()) {
            const auto& event = events[compaction];
            if (!event.is_object()) { ++compaction; continue; }
            if (event.value("message_count", uint64_t{0}) > count) break;
            ChatItem item;
            item.kind = ChatItemKind::Compaction;
            item.success = event.value("trigger", std::string("auto")) != "manual";
            model.Apply({TranscriptEventKind::Add, kNoChatItem, item}, now);
            ++compaction;
        }
    }
};

Transcript::Transcript() = default;
Transcript::~Transcript() = default;

size_t Transcript::Apply(const TranscriptEvent& event, TranscriptTime now) {
    ChatItem value = event.item;
    const auto timestamp = value.timestamp == 0 ? now.timestamp : value.timestamp;
    if (event.kind == TranscriptEventKind::Add) {
        value.text = value.kind == ChatItemKind::Reasoning ? ReasoningText(value.text) : SanitizeDisplayText(value.text);
        if (value.kind != ChatItemKind::Reasoning) value.timestamp = timestamp;
        if (value.kind == ChatItemKind::Tool || value.kind == ChatItemKind::TurnStatus) {
            value.start_tick_ms = value.start_tick_ms == 0 ? now.tick_ms : value.start_tick_ms;
            value.expanded = value.kind == ChatItemKind::TurnStatus;
        }
        if (value.kind == ChatItemKind::Tool || value.kind == ChatItemKind::TurnStatus ||
            value.kind == ChatItemKind::Assistant || value.kind == ChatItemKind::Reasoning) value.finished = false;
        if (value.kind == ChatItemKind::Tool) {
            value.tool_name = SanitizeDisplayText(value.tool_name);
            value.arguments = SanitizeDisplayText(value.arguments);
        }
        if (value.kind == ChatItemKind::Compaction)
            value.text = value.success ? "Context automatically compacted" : "Context manually compacted";
        if (value.kind == ChatItemKind::Error || value.kind == ChatItemKind::Quota) value.success = false;
        if (value.kind == ChatItemKind::Quota) value.text =
            "Quota exhausted: this API key has no remaining balance or quota. Recharge or switch API key/model, then retry.";
        if (value.kind == ChatItemKind::Error) for (size_t index = items_.size(); index > 0; --index) {
            if (items_[index - 1].kind == ChatItemKind::User) break;
            if (items_[index - 1].kind == ChatItemKind::Error || items_[index - 1].kind == ChatItemKind::Quota) {
                items_[index - 1] = std::move(value);
                return index - 1;
            }
        }
        items_.push_back(std::move(value));
        return items_.size() - 1;
    }
    if (event.index >= items_.size()) return kNoChatItem;
    ChatItem& item = items_[event.index];
    ++item.revision;
    switch (event.kind) {
        case TranscriptEventKind::Add: break;
        case TranscriptEventKind::AppendText:
            if (item.kind == ChatItemKind::Assistant) item.text += SanitizeDisplayText(value.text);
            if (item.kind == ChatItemKind::Reasoning) item.text = ReasoningText(item.text + value.text);
            break;
        case TranscriptEventKind::Finish:
            if (item.kind != ChatItemKind::Tool && item.kind != ChatItemKind::Reasoning &&
                item.kind != ChatItemKind::Assistant) break;
            item.finished = true;
            if (item.kind != ChatItemKind::Assistant || value.timestamp != 0 || item.timestamp == 0) item.timestamp = timestamp;
            if (item.kind == ChatItemKind::Tool) {
                item.success = value.success;
                item.result = SanitizeDisplayText(value.result);
                item.elapsed_ms = value.elapsed_ms > 0 ? value.elapsed_ms :
                    (item.start_tick_ms == 0 || now.tick_ms < item.start_tick_ms ? 0 : now.tick_ms - item.start_tick_ms);
            }
            break;
        case TranscriptEventKind::Hide:
            if (item.kind == ChatItemKind::Reasoning) item.visible = false;
            break;
        case TranscriptEventKind::ResumeTurn:
            if (item.kind != ChatItemKind::TurnStatus) break;
            item.start_tick_ms = value.start_tick_ms;
            item.elapsed_ms = value.elapsed_ms;
            item.finished = item.paused = false;
            item.expanded = true;
            break;
        case TranscriptEventKind::FinishTurn:
        case TranscriptEventKind::PauseTurn:
            if (item.kind != ChatItemKind::TurnStatus) break;
            item.finished = true;
            item.paused = event.kind == TranscriptEventKind::PauseTurn;
            item.expanded = item.paused;
            item.elapsed_ms = value.elapsed_ms;
            item.timestamp = timestamp;
            if (!item.paused) FoldTurn(event.index);
            break;
    }
    return event.index;
}

void Transcript::FoldTurn(size_t index) {
    const auto end = TurnEnd(items_, index);
    size_t last = kNoChatItem;
    for (size_t next = index + 1; next < end; ++next) if (items_[next].kind == ChatItemKind::Assistant) last = next;
    for (size_t next = index + 1; next < end; ++next) {
        auto& item = items_[next];
        ++item.revision;
        if (item.kind == ChatItemKind::Tool || item.kind == ChatItemKind::Reasoning) item.visible = false;
        if (item.kind == ChatItemKind::Assistant) item.visible = item.final_assistant = next == last;
    }
}
void Transcript::ToggleTurnStatus(size_t index) {
    if (index >= items_.size() || items_[index].kind != ChatItemKind::TurnStatus || !items_[index].finished) return;
    const bool expanded = items_[index].expanded = !items_[index].expanded;
    ++items_[index].revision;
    const auto end = TurnEnd(items_, index);
    size_t last = kNoChatItem;
    for (size_t next = index + 1; next < end; ++next) if (items_[next].kind == ChatItemKind::Assistant) last = next;
    for (size_t next = index + 1; next < end; ++next) {
        ++items_[next].revision;
        if (items_[next].kind == ChatItemKind::Tool) items_[next].visible = expanded;
        if (items_[next].kind == ChatItemKind::Assistant) items_[next].visible = expanded || next == last;
    }
}
void Transcript::ToggleTool(size_t index) {
    if (index < items_.size() && items_[index].kind == ChatItemKind::Tool) {
        items_[index].expanded = !items_[index].expanded;
        ++items_[index].revision;
    }
}
void Transcript::ToggleToolGroup(size_t index) {
    if (index < items_.size() && items_[index].kind == ChatItemKind::Tool)
        items_[index].group_collapsed = !items_[index].group_collapsed;
}
void Transcript::Clear() {
    replay_.reset();
    ++generation_;
    items_.clear();
}
void Transcript::EraseFrom(size_t index) {
    if (index >= items_.size()) return;
    replay_.reset();
    ++generation_;
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(index), items_.end());
}
const ChatItem* Transcript::Item(size_t index) const { return index < items_.size() ? &items_[index] : nullptr; }
size_t Transcript::LastUserIndex() const {
    for (size_t index = items_.size(); index > 0; --index) if (items_[index - 1].kind == ChatItemKind::User) return index - 1;
    return kNoChatItem;
}
uint64_t Transcript::Elapsed(const ChatItem& item, uint64_t now_tick_ms) {
    return item.finished ? item.elapsed_ms :
        item.elapsed_ms + (item.start_tick_ms == 0 || now_tick_ms < item.start_tick_ms ? 0 : now_tick_ms - item.start_tick_ms);
}
bool Transcript::NeedsAnimation() const {
    return std::any_of(items_.begin(), items_.end(), [](const ChatItem& item) {
        return !item.finished && (item.kind == ChatItemKind::Tool || item.kind == ChatItemKind::Reasoning ||
                                  item.kind == ChatItemKind::TurnStatus);
    });
}
std::vector<TranscriptGroup> Transcript::Groups(uint64_t now_tick_ms) const {
    std::vector<TranscriptGroup> groups(items_.size());
    for (size_t index = 0; index < items_.size(); ++index) {
        if (items_[index].kind != ChatItemKind::Tool) continue;
        auto& group = groups[index];
        size_t end = index, count = 1;
        for (;;) {
            size_t next = end + 1;
            while (next < items_.size() && items_[next].kind == ChatItemKind::Reasoning) ++next;
            if (next >= items_.size() || items_[next].kind != ChatItemKind::Tool || !CanGroup(items_[end], items_[next])) break;
            end = next;
            ++count;
        }
        if (count > 1) {
            group.leader = true;
            group.size = count;
            group.last_member = end;
            for (size_t member = index; member <= end; ++member) {
                const auto& item = items_[member];
                if (item.kind != ChatItemKind::Tool) continue;
                group.finished += item.finished ? 1 : 0;
                group.elapsed_ms += Elapsed(item, now_tick_ms);
                group.active = group.active || !item.finished;
                group.failed = group.failed || (item.finished && !item.success);
                const auto category = Category(item.tool_name);
                if (category == Activity::Read) ++group.read_count;
                if (category == Activity::Search) ++group.search_count;
                if (category == Activity::Command) ++group.command_count;
                if (member != index) { groups[member].hidden = items_[index].group_collapsed; groups[member].indented = true; }
            }
        }
        index = end;
    }
    return groups;
}

uint64_t Transcript::BeginReplay(const SessionState& state, TranscriptTime now, TranscriptRun run) {
    Clear();
    replay_ = std::make_unique<Replay>();
    replay_->state.messages = state.messages;
    replay_->state.metadata = state.metadata;
    replay_->now = now;
    replay_->run = run;
    replay_->total_turns = static_cast<size_t>(std::count_if(state.messages.begin(), state.messages.end(),
        [](const ChatMessage& message) { return message.role == MessageRole::User; }));
    const auto& live = Metadata(replay_->state, "live_turn");
    if (replay_->total_turns > 0 && live.is_object() && live.contains("active") && live["active"] == true &&
        live.contains("turn_index") && live["turn_index"].is_number_integer() &&
        live["turn_index"] == replay_->total_turns - 1) {
        replay_->live_turn = replay_->total_turns - 1;
        replay_->live_text = live.value("text", std::string{});
        replay_->live_reasoning = live.value("reasoning", std::string{});
    }
    return generation_;
}
bool Transcript::ReplayStep(uint64_t generation, size_t max_messages) {
    if (generation != generation_) return false;
    if (!replay_) return true;
    auto& replay = *replay_;
    size_t restored = 0;
    const auto budget = std::min<size_t>(64, max_messages);
    const auto& live = Metadata(replay.state, "live_turn");
    while (replay.message < replay.state.messages.size()) {
        if (restored++ >= budget) return false;
        replay.CompactionsThrough(*this, replay.message);
        const auto& message = replay.state.messages[replay.message++];
        ChatItem item;
        item.timestamp = message.timestamp;
        if (message.role == MessageRole::User) {
            replay.FinishPending(*this);
            replay.turn_tools.clear();
            item.kind = ChatItemKind::User; item.text = message.content;
            Apply({TranscriptEventKind::Add, kNoChatItem, item}, replay.now);
            item = {}; item.kind = ChatItemKind::TurnStatus;
            if (replay.run.active && replay.turn + 1 == replay.total_turns) item.start_tick_ms = replay.run.started_tick_ms;
            if (replay.turn == replay.live_turn) {
                if (item.start_tick_ms == 0) item.start_tick_ms = live.value("started_tick", uint64_t{0});
                item.elapsed_ms = live.value("elapsed_ms", uint64_t{0});
                item.text = live.value("status", std::string{});
            }
            replay.pending_status = Apply({TranscriptEventKind::Add, kNoChatItem, item}, replay.now);
            replay.pending_turn = replay.turn++;
        } else if (message.role == MessageRole::Assistant) {
            if (replay.pending_turn == replay.live_turn && replay.live_turn != kNoChatItem) {
                ConsumeSavedText(replay.live_text, message.content);
                ConsumeSavedText(replay.live_reasoning, message.reasoning_text);
            }
            if (!message.content.empty()) {
                item.kind = ChatItemKind::Assistant; item.text = message.content;
                const auto index = Apply({TranscriptEventKind::Add, kNoChatItem, item}, replay.now);
                Apply({TranscriptEventKind::Finish, index, item}, replay.now);
            }
            for (const auto& call : message.tool_calls) {
                item = {}; item.kind = ChatItemKind::Tool; item.timestamp = message.timestamp;
                item.call_id = call.id; item.tool_name = call.name; item.arguments = call.arguments_json;
                const auto index = Apply({TranscriptEventKind::Add, kNoChatItem, item}, replay.now);
                replay.tools[call.id] = replay.turn_tools[call.id] = index;
            }
        } else if (message.role == MessageRole::Tool) {
            const auto pending = replay.tools.find(message.tool_call_id);
            size_t index;
            if (pending != replay.tools.end()) { index = pending->second; replay.tools.erase(pending); }
            else {
                item.kind = ChatItemKind::Tool; item.tool_name = message.name;
                item.arguments = "{}"; item.call_id = message.tool_call_id;
                index = Apply({TranscriptEventKind::Add, kNoChatItem, item}, replay.now);
            }
            replay.turn_tools[message.tool_call_id] = index;
            item.result = message.content;
            const auto& durations = Metadata(replay.state, "tool_durations_ms");
            if (durations.is_object() && durations.contains(message.tool_call_id))
                item.elapsed_ms = Number(durations[message.tool_call_id]);
            Apply({TranscriptEventKind::Finish, index, item}, replay.now);
        }
    }
    replay.CompactionsThrough(*this, replay.message);
    if (replay.live_turn != kNoChatItem) {
        const auto tools = live.find("tools");
        while (tools != live.end() && tools->is_array() && replay.live_tool < tools->size()) {
            if (restored++ >= budget) return false;
            const auto& tool = (*tools)[replay.live_tool++];
            if (!tool.is_object()) continue;
            const auto id = tool.value("tool_call_id", std::string{});
            if (id.empty()) continue;
            const auto found = replay.turn_tools.find(id);
            size_t index;
            if (found != replay.turn_tools.end()) index = found->second;
            else {
                ChatItem item;
                item.kind = ChatItemKind::Tool; item.call_id = id;
                item.tool_name = tool.value("tool_name", std::string("tool"));
                item.arguments = tool.value("arguments", std::string{});
                index = Apply({TranscriptEventKind::Add, kNoChatItem, item}, replay.now);
                replay.turn_tools[id] = index;
            }
            auto& item = items_[index];
            const auto start = tool.value("started_tick", uint64_t{0});
            if (start != 0) item.start_tick_ms = start;
            if (tool.value("finished", false)) {
                if (!item.finished) {
                    ChatItem completion;
                    completion.result = tool.value("output", std::string{});
                    Apply({TranscriptEventKind::Finish, index, completion}, replay.now);
                }
                item.success = tool.value("success", false);
                item.elapsed_ms = tool.value("elapsed_ms", item.elapsed_ms);
            }
        }
        if (!replay.live_reasoning.empty()) {
            ChatItem item;
            item.kind = ChatItemKind::Reasoning; item.text = replay.live_reasoning;
            item.visible = replay.live_text.empty() && live.value("phase", std::string{}) != "running_tool";
            Apply({TranscriptEventKind::Add, kNoChatItem, item}, replay.now);
        }
        if (!replay.live_text.empty()) {
            ChatItem item;
            item.kind = ChatItemKind::Assistant; item.text = replay.live_text;
            Apply({TranscriptEventKind::Add, kNoChatItem, item}, replay.now);
        }
    }
    if (!replay.run.active && replay.live_turn == kNoChatItem) {
        for (const auto& pending : replay.tools) {
            ChatItem completion;
            completion.success = false;
            completion.result = "Tool did not report completion before the session stopped.";
            Apply({TranscriptEventKind::Finish, pending.second, completion}, replay.now);
        }
        replay.FinishPending(*this, Metadata(replay.state, "paused_turn").is_object());
    }
    replay_.reset();
    return true;
}

Result<TranscriptHistory> ReadTranscriptHistory(const std::string& path, const CancellationToken* cancellation) {
    if (cancellation && cancellation->IsCancelled()) return Result<TranscriptHistory>::Failure("cancelled");
    spark::fs_ifstream stream(NativePath(path), std::ios::binary);
    if (!stream) return Result<TranscriptHistory>::Failure("could not open session journal");
    TranscriptHistory summary;
    summary.path = path;
    std::string line;
    bool first_line = true;
    while (std::getline(stream, line)) {
        if (cancellation && cancellation->IsCancelled()) return Result<TranscriptHistory>::Failure("cancelled");
        if (first_line && line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
        first_line = false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        try {
            const auto event = nlohmann::json::parse(line);
            const auto type = event.value("type", std::string{});
            const auto payload = event.value("payload", nlohmann::json::object());
            summary.updated_at = event.value("timestamp", summary.updated_at);
            if (type == "session.created") {
                summary.title = payload.value("title", std::string(u8"\u65b0\u5bf9\u8bdd"));
                summary.workspace = payload.value("workspace", std::string{});
            } else if (type == "session.title") summary.title = payload.value("title", summary.title);
            else if (type == "session.pinned") summary.pinned = payload.value("pinned", summary.pinned);
            else if (type == "session.scope") summary.scope = payload.value("scope", summary.scope);
            else if (type == "message") summary.has_messages = true;
            else if (type == "snapshot") {
                summary.title = payload.value("title", summary.title);
                summary.workspace = payload.value("workspace", summary.workspace);
                summary.has_messages = summary.has_messages ||
                    (payload.contains("messages") && payload["messages"].is_array() && !payload["messages"].empty());
            }
        } catch (const nlohmann::json::exception&) {}
    }
    if (stream.bad()) return Result<TranscriptHistory>::Failure("could not read session journal");
    if (summary.title.empty()) summary.title = u8"\u65b0\u5bf9\u8bdd";
    return Result<TranscriptHistory>::Success(std::move(summary));
}

}  // namespace spark
