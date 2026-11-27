#include "core/session_exchange.h"

#include "core/agent_types.h"
#include "core/session.h"

#ifdef _WIN32
#include <objbase.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include "platform/filesystem.h"
#include <fstream>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>

namespace spark {
namespace {

std::string GenerateUuid() {
#ifdef _WIN32
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        std::random_device device;
        std::mt19937_64 random(static_cast<uint64_t>(device()) ^
                               static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
        unsigned char* guid_bytes = reinterpret_cast<unsigned char*>(&guid);
        for (size_t index = 0; index < sizeof(guid); ++index) guid_bytes[index] = static_cast<unsigned char>(random());
        guid.Data3 = static_cast<unsigned short>((guid.Data3 & 0x0FFF) | 0x4000);
        guid.Data4[0] = static_cast<unsigned char>((guid.Data4[0] & 0x3F) | 0x80);
    }
    char text[40]{};
    std::snprintf(text, sizeof(text), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
                  guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return text;
#else
    std::array<unsigned char, 16> bytes{};
    std::random_device device;
    for (auto& byte : bytes) byte = static_cast<unsigned char>(device());
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);
    constexpr char hex[] = "0123456789abcdef";
    std::string text;
    text.reserve(36);
    for (size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) text.push_back('-');
        text.push_back(hex[bytes[index] >> 4]);
        text.push_back(hex[bytes[index] & 0x0F]);
    }
    return text;
#endif
}

std::tm UtcTime(uint64_t seconds) {
    const std::time_t value = static_cast<std::time_t>(seconds == 0 ? UnixTimeSeconds() : seconds);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &value);
#else
    gmtime_r(&value, &utc);
#endif
    return utc;
}

std::string IsoTimeFromSeconds(uint64_t seconds) {
    const std::tm utc = UtcTime(seconds);
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

std::string FileTimestamp(uint64_t seconds) {
    const std::tm utc = UtcTime(seconds);
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H-%M-%S");
    return stream.str();
}

std::string DatePath(uint64_t seconds) {
    const std::tm utc = UtcTime(seconds);
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y/%m/%d");
    auto path = NativePath(stream.str());
    return PathToUtf8(path.make_preferred());
}

uint64_t ParseIsoTime(const std::string& text) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    constexpr char format[] = "%4d-%2d-%2dT%2d:%2d:%2d";
#ifdef _WIN32
    const int parsed = sscanf_s(text.c_str(), format, &year, &month, &day, &hour, &minute, &second);
#else
    const int parsed = std::sscanf(text.c_str(), format, &year, &month, &day, &hour, &minute, &second);
#endif
    if (parsed != 6) return 0;
    std::tm utc{};
    utc.tm_year = year - 1900;
    utc.tm_mon = month - 1;
    utc.tm_mday = day;
    utc.tm_hour = hour;
    utc.tm_min = minute;
    utc.tm_sec = second;
#ifdef _WIN32
    const __time64_t value = _mkgmtime64(&utc);
#else
    const std::time_t value = ::timegm(&utc);
#endif
    return value < 0 ? 0 : static_cast<uint64_t>(value);
}

std::string CodexRolloutId(const std::string& filename) {
    constexpr size_t timestamp_start = 8;
    constexpr size_t timestamp_size = 19;
    constexpr size_t id_start = timestamp_start + timestamp_size + 1;
    constexpr size_t id_size = 36;
    if (filename.size() != id_start + id_size + 6 || filename.compare(0, timestamp_start, "rollout-") != 0 ||
        filename[id_start - 1] != '-' || filename.compare(id_start + id_size, 6, ".jsonl") != 0) return {};

    std::string timestamp = filename.substr(timestamp_start, timestamp_size);
    timestamp[13] = timestamp[16] = ':';
    const uint64_t seconds = ParseIsoTime(timestamp);
    if (seconds == 0 || filename.compare(timestamp_start, timestamp_size, FileTimestamp(seconds)) != 0) return {};

    const auto id = filename.substr(id_start, id_size);
    for (size_t index = 0; index < id.size(); ++index) {
        const char c = id[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (c != '-') return {};
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return {};
        }
    }
    return id;
}

nlohmann::json Line(const std::string& type, const nlohmann::json& payload, uint64_t timestamp) {
    return {
        {"timestamp", IsoTimeFromSeconds(timestamp)},
        {"type", type},
        {"payload", payload},
    };
}

struct CodexImportImages {
    std::string directory;
    std::string workspace;

    std::string Resolve(const std::string& reference) const {
        if (reference.compare(0, 5, "data:") != 0) {
            if (reference.empty() || reference.find("://") != std::string::npos ||
                reference.find('\0') != std::string::npos) return {};
            const auto path = NativePath(reference);
            const std::string extension = LowerAscii(PathToUtf8(path.extension()));
            if (extension != ".png" && extension != ".jpg" && extension != ".jpeg" &&
                extension != ".webp" && extension != ".gif" && extension != ".bmp") return {};
            return AbsolutePath(reference, workspace);
        }

        const auto comma = reference.find(',');
        if (comma == std::string::npos || comma > 32) return {};
        const std::string_view header(reference.data(), comma);
        const char* extension = nullptr;
        if (header == "data:image/png;base64") extension = ".png";
        if (header == "data:image/jpeg;base64") extension = ".jpg";
        if (header == "data:image/webp;base64") extension = ".webp";
        if (header == "data:image/gif;base64") extension = ".gif";
        if (header == "data:image/bmp;base64") extension = ".bmp";
        if (!extension) return {};

        const std::string_view encoded(reference.data() + comma + 1, reference.size() - comma - 1);
        constexpr size_t max_image_bytes = 32 * 1024 * 1024;
        if (encoded.empty() || encoded.size() > ((max_image_bytes + 2) / 3) * 4 ||
            encoded.size() % 4 != 0) return {};
        constexpr std::string_view alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string bytes;
        bytes.reserve(encoded.size() / 4 * 3);
        for (size_t offset = 0; offset < encoded.size(); offset += 4) {
            unsigned value = 0;
            size_t padding = 0;
            for (size_t digit = 0; digit < 4; ++digit) {
                const auto character = encoded[offset + digit];
                value <<= 6;
                if (character == '=' && digit >= 2 && offset + 4 == encoded.size()) {
                    ++padding;
                } else {
                    const auto index = alphabet.find(character);
                    if (padding || index == std::string_view::npos) return {};
                    value |= static_cast<unsigned>(index);
                }
            }
            bytes.push_back(static_cast<char>((value >> 16) & 0xff));
            if (padding < 2) bytes.push_back(static_cast<char>((value >> 8) & 0xff));
            if (padding == 0) bytes.push_back(static_cast<char>(value & 0xff));
        }
        if (bytes.size() > max_image_bytes) return {};
        std::error_code error;
        fs::create_directories(NativePath(directory), error);
        if (error) return {};
        const std::string path = JoinPath(directory, GenerateUuid() + extension);
        return WriteFileUtf8Atomic(path, bytes) ? path : std::string{};
    }

    void Append(const std::string& reference, ChatMessage& message) const {
        auto path = Resolve(reference);
        if (!path.empty() && std::find(message.image_paths.begin(), message.image_paths.end(), path) ==
            message.image_paths.end()) {
            message.image_paths.push_back(std::move(path));
        }
    }
};

void AppendCodexContent(const nlohmann::json& content, const CodexImportImages& images, ChatMessage& message) {
    if (!content.is_array()) return;
    for (const auto& part : content) {
        if (!part.is_object()) continue;
        const std::string type = part.value("type", std::string{});
        if (type == "input_text" || type == "output_text" || type == "text") {
            const std::string value = part.value("text", std::string{});
            if (!value.empty()) {
                if (!message.content.empty()) message.content += "\n";
                message.content += value;
            }
        } else if (type == "input_image" || type == "image_url") {
            const auto url = part.find("image_url");
            const auto count = message.image_paths.size();
            if (url != part.end()) {
                if (url->is_string()) images.Append(url->get_ref<const std::string&>(), message);
                else if (url->is_object()) images.Append(url->value("url", std::string{}), message);
            }
            if (message.image_paths.size() == count) {
                if (!message.content.empty()) message.content += "\n";
                message.content += "[image unavailable]";
            }
        }
    }
}

std::string OutputText(const nlohmann::json& output) {
    if (output.is_string()) return output.get<std::string>();
    if (output.is_null()) return {};
    return output.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string ReasoningText(const nlohmann::json& payload) {
    std::string text;
    const auto append = [&](const nlohmann::json& values) {
        if (!values.is_array()) return;
        for (const auto& item : values) {
            if (!item.is_object()) continue;
            const std::string value = item.value("text", std::string{});
            if (value.empty()) continue;
            if (!text.empty()) text += "\n";
            text += value;
        }
    };
    append(payload.value("summary", nlohmann::json::array()));
    append(payload.value("content", nlohmann::json::array()));
    return text;
}

void AppendCodexResponse(std::vector<std::string>& lines, uint64_t timestamp, const nlohmann::json& payload) {
    lines.push_back(Line("response_item", payload, timestamp).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

struct CodexImportStep {
    enum class Kind { Message, Compaction, Usage };
    Kind kind = Kind::Message;
    ChatMessage message;
    nlohmann::json payload;
};

bool RemoveCodexCompactionOutput(std::vector<CodexImportStep>& steps, const std::string& summary) {
    if (steps.empty() || steps.back().kind != CodexImportStep::Kind::Message) return false;
    const ChatMessage& message = steps.back().message;
    if (message.role != MessageRole::Assistant || message.content.empty() || !message.tool_calls.empty())
        return false;
    const std::string prefix =
        "Another language model started to solve this problem and produced a summary of its thinking process. "
        "You also have access to the state of the tools that were used by that language model. Use this to "
        "build on the work that has already been done and avoid duplicating work. Here is the summary "
        "produced by the other language model, use the information in this summary to assist with your own analysis:";
    // Codex persists the compaction response immediately before its checkpoint.
    // That response is internal input for the replacement history, not a chat reply.
    const std::string text = Trim(message.content);
    if (text != Trim(summary) &&
        (summary.compare(0, prefix.size(), prefix) != 0 || text != Trim(summary.substr(prefix.size()))))
        return false;
    steps.pop_back();
    return true;
}

bool IsCodexContextualUserMessage(const nlohmann::json& payload) {
    const auto metadata = payload.find("internal_chat_message_metadata_passthrough");
    if (metadata == payload.end() || !metadata->is_object()) return false;
    const auto kinds_field = metadata->find("content_item_kinds");
    if (kinds_field == metadata->end() || !kinds_field->is_array()) return false;
    const nlohmann::json& kinds = *kinds_field;
    if (!kinds.is_array() || kinds.empty()) return false;

    bool has_context_fragment = false;
    for (const nlohmann::json& item : kinds) {
        const std::string kind = item.is_string()
                                     ? item.get<std::string>()
                                     : item.is_object() ? item.value("kind", item.value("type", std::string{}))
                                                        : std::string{};
        if (kind.empty() || kind == "unknown" || kind.compare(0, 5, "user.") == 0) return false;
        if (kind == "agents_md.instructions" || kind == "environments.environment_context" ||
            kind == "hooks.additional_context" || kind == "memories.instructions" ||
            kind == "host_skills.instructions" || kind == "permissions.instructions" ||
            kind == "collaboration_mode.instructions" || kind == "plugins.usage_instructions" ||
            kind == "tools.deferred_namespaces" || kind == "multi_agent.usage_hint" ||
            kind == "multi_agent.mode_instructions" ||
            kind == "additional_content.codex_apps_client_time_context" ||
            kind == "generic.developer_instructions" || kind == "generic.turn_aborted") {
            has_context_fragment = true;
            continue;
        }
        return false;
    }
    return has_context_fragment;
}

bool ParseCodexResponseItem(const nlohmann::json& payload, uint64_t timestamp, ChatMessage* message,
                            bool* is_plain_message, const CodexImportImages& images) {
    if (message == nullptr) return false;
    if (is_plain_message != nullptr) *is_plain_message = false;
    const std::string item_type = payload.value("type", std::string{});
    if (item_type == "message") {
        const std::string role = payload.value("role", std::string{});
        if (role == "user") {
            if (IsCodexContextualUserMessage(payload)) return false;
            message->role = MessageRole::User;
        } else if (role == "assistant") {
            message->role = MessageRole::Assistant;
        } else if (role == "tool") {
            message->role = MessageRole::Tool;
        } else {
            return false;
        }
        const auto content = payload.find("content");
        if (content != payload.end()) AppendCodexContent(*content, images, *message);
        message->timestamp = timestamp;
        if (is_plain_message != nullptr) *is_plain_message = true;
        return !message->content.empty() || !message->image_paths.empty();
    }
    if (item_type == "reasoning") {
        const std::string reasoning = ReasoningText(payload);
        if (reasoning.empty()) return false;
        message->role = MessageRole::Assistant;
        message->reasoning_text = reasoning;
        message->reasoning_items.push_back(reasoning);
        message->timestamp = timestamp;
        return true;
    }
    if (item_type == "function_call" || item_type == "custom_tool_call") {
        ToolCall call;
        call.id = payload.value("call_id", std::string{});
        call.name = payload.value("name", std::string{});
        const nlohmann::json arguments = payload.value("arguments", payload.value("input", nlohmann::json::object()));
        call.arguments_json = arguments.is_string()
                                  ? arguments.get<std::string>()
                                  : arguments.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        if (call.name.empty()) return false;
        message->role = MessageRole::Assistant;
        message->timestamp = timestamp;
        message->tool_calls.push_back(std::move(call));
        return true;
    }
    if (item_type == "function_call_output" || item_type == "custom_tool_call_output") {
        message->role = MessageRole::Tool;
        message->tool_call_id = payload.value("call_id", std::string{});
        message->name = payload.value("name", std::string{});
        const auto output = payload.find("output");
        if (output != payload.end()) {
            const bool content_items = output->is_array() && !output->empty() &&
                std::all_of(output->begin(), output->end(), [](const auto& item) {
                    if (!item.is_object()) return false;
                    const auto type = item.value("type", std::string{});
                    return type == "input_text" || type == "output_text" || type == "text" ||
                           type == "input_image" || type == "image_url";
                });
            if (content_items) AppendCodexContent(*output, images, *message);
            else message->content = OutputText(*output);
        }
        message->timestamp = timestamp;
        return true;
    }
    return false;
}

std::string FirstUserTitle(const std::vector<CodexImportStep>& steps) {
    for (const CodexImportStep& step : steps) {
        if (step.kind != CodexImportStep::Kind::Message) continue;
        const ChatMessage& message = step.message;
        if (message.role != MessageRole::User || Trim(message.content).empty()) continue;
        std::string title = Trim(message.content);
        std::replace(title.begin(), title.end(), '\r', ' ');
        std::replace(title.begin(), title.end(), '\n', ' ');
        if (title.size() > 120) title = TruncateUtf8(std::move(title), 120);
        return title;
    }
    return "Codex 导入";
}

bool ParseCodexSessionMeta(const nlohmann::json& payload, std::string* workspace, uint64_t* created_at) {
    if (workspace != nullptr) *workspace = payload.value("cwd", std::string{});
    if (created_at != nullptr) *created_at = ParseIsoTime(payload.value("timestamp", std::string{}));
    return payload.contains("id") || payload.contains("session_id");
}

}  // namespace

std::string DefaultCodexSessionsRoot() {
    const std::string codex_home = EnvironmentVariable("CODEX_HOME");
    if (!codex_home.empty()) return JoinPath(codex_home, "sessions");
#ifdef _WIN32
    const std::string user_profile = EnvironmentVariable("USERPROFILE");
#else
    const std::string user_profile = EnvironmentVariable("HOME");
#endif
    if (!user_profile.empty()) return JoinPath(JoinPath(user_profile, ".codex"), "sessions");
    return JoinPath(GetExecutableDirectory(), "codex-sessions");
}

std::string SuggestedCodexRolloutPath(const std::string& codex_sessions_root, uint64_t created_at) {
    if (created_at == 0) created_at = UnixTimeSeconds();
    const std::string root = AbsolutePath(codex_sessions_root.empty() ? DefaultCodexSessionsRoot() : codex_sessions_root);
    const std::string timestamp = FileTimestamp(created_at);
    const std::string filename = "rollout-" + timestamp + "-" + GenerateUuid() + ".jsonl";
    return JoinPath(JoinPath(root, DatePath(created_at)), filename);
}

Result<void> ExportSparkSessionToCodex(const std::string& spark_session_path,
                                       const std::string& codex_output_path) {
    if (Trim(spark_session_path).empty() || Trim(codex_output_path).empty()) {
        return Result<void>::Failure("export paths must not be empty");
    }
    const auto output_path = NativePath(codex_output_path);
    const std::string session_id = CodexRolloutId(PathToUtf8(output_path.filename()));
    if (session_id.empty()) {
        return Result<void>::Failure("keep the suggested rollout filename so Codex can find the chat by ID");
    }
    SessionJournal source;
    const auto loaded = source.Resume(spark_session_path);
    if (!loaded.ok) return Result<void>::Failure("could not read Spark session: " + loaded.error);

    const SessionState& state = source.State();
    const uint64_t created_at = state.created_at == 0 ? UnixTimeSeconds() : state.created_at;
    const std::string timestamp = IsoTimeFromSeconds(created_at);
    // Leave the provider unset so the target Codex installation can select its own.
    const nlohmann::json meta = {
        {"session_id", session_id},
        {"id", session_id},
        {"timestamp", timestamp},
        {"cwd", state.workspace},
        {"runtime_workspace_roots", nlohmann::json::array({state.workspace})},
        {"originator", "spark_agent"},
        {"cli_version", "0.1.0"},
        {"source", "cli"},
        {"thread_source", "user"},
        {"base_instructions", nullptr},
        {"selected_capability_roots", nlohmann::json::array()},
        {"history_mode", "legacy"},
    };

    std::vector<bool> final_answers(state.messages.size());
    bool has_later_activity = false;
    for (size_t index = state.messages.size(); index > 0; --index) {
        const auto& message = state.messages[index - 1];
        if (message.role == MessageRole::User) {
            has_later_activity = false;
        } else if (message.role == MessageRole::Tool || !message.tool_calls.empty()) {
            has_later_activity = true;
        } else if (message.role == MessageRole::Assistant && !Trim(message.content).empty()) {
            final_answers[index - 1] = !has_later_activity;
            has_later_activity = true;
        }
    }

    std::vector<std::string> lines;
    lines.push_back(Line("session_meta", meta, created_at).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    for (size_t index = 0; index < state.messages.size(); ++index) {
        const auto& message = state.messages[index];
        const uint64_t message_time = message.timestamp == 0 ? created_at : message.timestamp;
        if (message.role == MessageRole::System) continue;
        if (message.role == MessageRole::User) {
            AppendCodexResponse(lines, message_time,
                                {{"type", "message"},
                                 {"role", "user"},
                                 {"content", nlohmann::json::array({{{"type", "input_text"}, {"text", message.content}}})}});
            lines.push_back(Line("event_msg",
                                 {{"type", "user_message"},
                                  {"message", message.content},
                                  {"kind", "plain"},
                                  {"local_images", nlohmann::json::array()},
                                  {"text_elements", nlohmann::json::array()}},
                                 message_time)
                                .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
            continue;
        }
        if (message.role == MessageRole::Tool) {
            AppendCodexResponse(lines, message_time,
                                {{"type", "function_call_output"},
                                 {"call_id", message.tool_call_id},
                                 {"output", message.content}});
            continue;
        }
        if (!message.reasoning_text.empty()) {
            AppendCodexResponse(lines, message_time,
                                {{"type", "reasoning"},
                                 {"summary", nlohmann::json::array({{{"type", "summary_text"}, {"text", message.reasoning_text}}})},
                                 {"encrypted_content", nullptr}});
        }
        if (!Trim(message.content).empty()) {
            const char* phase = final_answers[index] ? "final_answer" : "commentary";
            AppendCodexResponse(lines, message_time,
                                {{"type", "message"},
                                 {"role", "assistant"},
                                 {"phase", phase},
                                 {"content", nlohmann::json::array({{{"type", "output_text"}, {"text", message.content}}})}});
            lines.push_back(Line("event_msg",
                                 {{"type", "agent_message"}, {"message", message.content}, {"phase", phase}},
                                 message_time)
                                .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        }
        for (const ToolCall& call : message.tool_calls) {
            AppendCodexResponse(lines, message_time,
                                {{"type", "function_call"},
                                 {"name", call.name},
                                 {"arguments", call.arguments_json},
                                 {"call_id", call.id}});
        }
    }

    std::error_code error;
    if (output_path.has_parent_path()) spark::fs::create_directories(output_path.parent_path(), error);
    if (error) return Result<void>::Failure("could not create Codex session directory: " + error.message());
    spark::fs_ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) return Result<void>::Failure("could not open Codex session output");
    for (const std::string& line : lines) output << line << "\n";
    output.close();
    if (!output) return Result<void>::Failure("could not write Codex session output");
    return Result<void>::Success();
}

Result<CodexSessionImportResult> ImportCodexSessionToSpark(const std::string& codex_session_path,
                                                           const std::string& data_directory,
                                                           const std::string& workspace,
                                                           const std::string& scope) {
    fs_ifstream input(NativePath(codex_session_path), std::ios::binary);
    if (!input) return Result<CodexSessionImportResult>::Failure("could not open Codex session");

    std::vector<CodexImportStep> import_steps;
    std::vector<CodexImportStep> fallback_steps;
    std::string codex_workspace;
    uint64_t created_at = 0;
    size_t response_messages = 0;
    size_t visible_message_count = 0;
    size_t fallback_message_count = 0;
    CodexImportImages images{JoinPath(AbsolutePath(data_directory), "codex-import-images"), {}};

    bool first_line = true;
    std::string line;
    while (std::getline(input, line)) {
        if (first_line) {
            first_line = false;
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        nlohmann::json event;
        try {
            event = nlohmann::json::parse(line);
        } catch (const nlohmann::json::exception&) {
            continue;
        }
        const std::string type = event.value("type", std::string{});
        const auto payload_field = event.find("payload");
        if (payload_field == event.end() || !payload_field->is_object()) continue;
        const auto& payload = *payload_field;
        const uint64_t timestamp = ParseIsoTime(event.value("timestamp", std::string{}));
        if (type == "session_meta") {
            ParseCodexSessionMeta(payload, &codex_workspace, &created_at);
            images.workspace = workspace.empty() ? codex_workspace : AbsolutePath(workspace);
            continue;
        }
        if (type == "response_item") {
            ChatMessage message;
            bool is_plain_message = false;
            if (ParseCodexResponseItem(payload, timestamp, &message, &is_plain_message, images)) {
                if (is_plain_message && message.role != MessageRole::Tool) ++response_messages;
                ++visible_message_count;
                CodexImportStep step;
                step.kind = CodexImportStep::Kind::Message;
                step.message = std::move(message);
                import_steps.push_back(std::move(step));
            }
            continue;
        }
        if (type == "compacted") {
            const std::string summary = payload.value("message", std::string{});
            if (RemoveCodexCompactionOutput(import_steps, summary)) {
                --visible_message_count;
                --response_messages;
            }
            if (RemoveCodexCompactionOutput(fallback_steps, summary)) --fallback_message_count;
            nlohmann::json compacted_payload = {
                {"trigger", "codex_import"},
                {"summary", summary},
                {"message_count", visible_message_count},
            };
            const auto replacement_history = payload.find("replacement_history");
            if (replacement_history != payload.end() && replacement_history->is_array()) {
                std::vector<ChatMessage> context_messages;
                for (const nlohmann::json& history_item : *replacement_history) {
                    const auto& item_payload =
                        history_item.contains("item") && history_item["item"].is_object()
                            ? history_item["item"]
                            : history_item;
                    ChatMessage message;
                    if (item_payload.is_object() &&
                        ParseCodexResponseItem(item_payload, 0, &message, nullptr, images)) {
                        context_messages.push_back(std::move(message));
                    }
                }
                if (!context_messages.empty()) compacted_payload["context_messages"] = context_messages;
            }
            CodexImportStep step;
            step.kind = CodexImportStep::Kind::Compaction;
            step.payload = std::move(compacted_payload);
            fallback_steps.push_back(step);
            fallback_steps.back().payload["message_count"] = fallback_message_count;
            import_steps.push_back(std::move(step));
            continue;
        }
        if (type == "event_msg") {
            const std::string event_type = payload.value("type", std::string{});
            if (event_type == "token_count") {
                const auto info = payload.find("info");
                if (info == payload.end() || !info->is_object()) continue;
                const auto last = info->find("last_token_usage");
                if (last == info->end() || !last->is_object()) continue;
                const auto tokens = last->value("total_tokens", uint64_t{0});
                if (!tokens) continue;
                CodexImportStep step;
                step.kind = CodexImportStep::Kind::Usage;
                step.payload = {{"context_tokens", tokens},
                                {"context_limit_tokens", info->value("model_context_window", uint64_t{0})},
                                {"context_only", true}};
                import_steps.push_back(step);
                fallback_steps.push_back(std::move(step));
            } else if (event_type == "user_message" || event_type == "agent_message") {
                ChatMessage message;
                message.role = event_type == "user_message" ? MessageRole::User : MessageRole::Assistant;
                message.content = payload.value("message", std::string{});
                message.timestamp = timestamp;
                if (message.role == MessageRole::User) {
                    for (const char* key : {"images", "local_images"}) {
                        const auto attachments = payload.find(key);
                        if (attachments == payload.end() || !attachments->is_array()) continue;
                        for (const auto& attachment : *attachments) {
                            if (attachment.is_string()) images.Append(attachment.get_ref<const std::string&>(), message);
                        }
                    }
                }
                if (!message.content.empty() || !message.image_paths.empty()) {
                    ++fallback_message_count;
                    CodexImportStep step;
                    step.kind = CodexImportStep::Kind::Message;
                    step.message = std::move(message);
                    fallback_steps.push_back(std::move(step));
                }
            }
        }
    }

    if (input.bad()) return Result<CodexSessionImportResult>::Failure("could not read Codex session");
    if (response_messages == 0 && fallback_message_count != 0) {
        import_steps = std::move(fallback_steps);
        visible_message_count = fallback_message_count;
    } else {
        auto next_response = import_steps.begin();
        for (auto& step : fallback_steps) {
            if (step.kind != CodexImportStep::Kind::Message ||
                step.message.role != MessageRole::User) continue;
            const auto match = std::find_if(next_response, import_steps.end(), [&](const CodexImportStep& candidate) {
                return candidate.kind == CodexImportStep::Kind::Message &&
                       candidate.message.role == MessageRole::User &&
                       candidate.message.content == step.message.content;
            });
            if (match == import_steps.end()) continue;
            if (match->message.image_paths.empty()) match->message.image_paths = std::move(step.message.image_paths);
            next_response = match + 1;
        }
    }
    if (visible_message_count == 0) {
        return Result<CodexSessionImportResult>::Failure("Codex session has no importable messages");
    }

    CodexSessionImportResult result;
    result.title = FirstUserTitle(import_steps);
    result.message_count = visible_message_count;
    const std::string target_workspace = workspace.empty() ? (codex_workspace.empty() ? CurrentDirectoryUtf8() : codex_workspace)
                                                           : AbsolutePath(workspace);

    SessionJournal journal;
    const auto opened = journal.Open(data_directory, target_workspace);
    if (!opened.ok) return Result<CodexSessionImportResult>::Failure("could not create Spark session: " + opened.error);
    const std::string normalized_scope = scope == "project" ? "project" : "recent";
    const auto scope_result = journal.Append("session.scope", {{"scope", normalized_scope}});
    if (!scope_result.ok) return Result<CodexSessionImportResult>::Failure(scope_result.error);
    const auto title_result = journal.Append("session.title", {{"title", result.title}});
    if (!title_result.ok) return Result<CodexSessionImportResult>::Failure(title_result.error);
    size_t user_turns = 0;
    std::vector<nlohmann::json> usages;
    for (const CodexImportStep& step : import_steps) {
        if (step.kind == CodexImportStep::Kind::Usage) {
            if (user_turns != 0) usages.back() = step.payload;
            continue;
        }
        if (step.kind == CodexImportStep::Kind::Message && step.message.role == MessageRole::User) {
            ++user_turns;
            usages.emplace_back();
        }
        const auto append_result = step.kind == CodexImportStep::Kind::Message
                                       ? journal.Append("message", step.message)
                                       : journal.Append("context.compacted", step.payload);
        if (!append_result.ok) return Result<CodexSessionImportResult>::Failure(append_result.error);
    }
    for (size_t index = 0; index < usages.size(); ++index) {
        if (usages[index].is_null()) continue;
        usages[index]["turn_index"] = index;
        const auto saved = journal.Append("turn.usage", usages[index]);
        if (!saved.ok) return Result<CodexSessionImportResult>::Failure(saved.error);
    }
    result.path = journal.Path();
    return Result<CodexSessionImportResult>::Success(std::move(result));
}

}  // namespace spark
