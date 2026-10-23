#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace spark {

enum class MessageRole {
    System,
    User,
    Assistant,
    Tool,
};

enum class AgentPhase {
    WaitingModel,
    Streaming,
    Compacting,
    RunningTool,
    WaitingApproval,
    Cancelling,
    Completed,
};

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments_json;
};

// Provenance shown by the lightweight turn-summary popup. Keep this as plain
// transcript metadata so old journals remain readable and no provider payload
// needs to change.
struct SourceReference {
    std::string kind;
    std::string label;
    std::string detail;
};

struct ChatMessage {
    MessageRole role = MessageRole::User;
    std::string content;
    // Image paths stay in the journal as references. Providers load and cap the
    // binary data only while building the current request.
    std::vector<std::string> image_paths;
    std::string name;
    std::string tool_call_id;
    std::vector<ToolCall> tool_calls;
    // Thinking models require their reasoning to be handed back to the API together
    // with the assistant turn, otherwise the next request is rejected (for example
    // "The `reasoning_text` in the thinking mode must be passed back to the API").
    std::string reasoning_text;
    std::vector<std::string> reasoning_items;
    std::vector<SourceReference> sources;
    // Unix seconds when the message was emitted. This is transcript metadata
    // persisted in the session journal and is not sent to model providers.
    uint64_t timestamp = 0;
};

struct ToolDefinition {
    std::string name;
    std::string description;
    nlohmann::json parameters = nlohmann::json::object();
};

struct ModelReply {
    static constexpr std::size_t kMaxSources = 64;
    static constexpr std::size_t kMaxSourceStringBytes = 2048;

    std::string text;
    std::vector<ToolCall> tool_calls;
    std::string finish_reason;
    int input_tokens = 0;
    int output_tokens = 0;
    int cached_input_tokens = 0;
    int cache_write_input_tokens = 0;
    std::string provider_request_id;
    // HTTP level facts about the request that produced this reply, so a stalled or
    // unparseable answer can be diagnosed from the session journal alone.
    std::string diagnostics;
    std::string reasoning_text;
    std::vector<std::string> reasoning_items;
    // Inbound provenance only. Adapters deduplicate and cap this metadata; it is
    // persisted on the assistant message but never added to provider requests.
    std::vector<SourceReference> sources;
};

enum class ApprovalKind {
    FileWrite,
    Command,
    McpTool,
    OutsideWorkspaceRead,
    OutsideWorkspaceWrite,
};

struct ApprovalRequest {
    ApprovalKind kind = ApprovalKind::FileWrite;
    std::string title;
    std::string detail;
};

std::string MessageRoleToString(MessageRole role);
MessageRole MessageRoleFromString(const std::string& value);
std::string ApprovalKindToString(ApprovalKind kind);

void to_json(nlohmann::json& output, const ToolCall& call);
void from_json(const nlohmann::json& input, ToolCall& call);
void to_json(nlohmann::json& output, const SourceReference& source);
void from_json(const nlohmann::json& input, SourceReference& source);
void to_json(nlohmann::json& output, const ChatMessage& message);
void from_json(const nlohmann::json& input, ChatMessage& message);

}  // namespace spark
