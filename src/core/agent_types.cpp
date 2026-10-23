#include "core/agent_types.h"

namespace spark {

std::string MessageRoleToString(MessageRole role) {
    switch (role) {
        case MessageRole::System: return "system";
        case MessageRole::User: return "user";
        case MessageRole::Assistant: return "assistant";
        case MessageRole::Tool: return "tool";
    }
    return "user";
}

MessageRole MessageRoleFromString(const std::string& value) {
    if (value == "system") return MessageRole::System;
    if (value == "assistant") return MessageRole::Assistant;
    if (value == "tool") return MessageRole::Tool;
    return MessageRole::User;
}

std::string ApprovalKindToString(ApprovalKind kind) {
    switch (kind) {
        case ApprovalKind::FileWrite: return "file_write";
        case ApprovalKind::Command: return "command";
        case ApprovalKind::McpTool: return "mcp_tool";
        case ApprovalKind::OutsideWorkspaceRead: return "outside_workspace_read";
        case ApprovalKind::OutsideWorkspaceWrite: return "outside_workspace_write";
    }
    return "unknown";
}

void to_json(nlohmann::json& output, const ToolCall& call) {
    output = {{"id", call.id}, {"name", call.name}, {"arguments", call.arguments_json}};
}

void from_json(const nlohmann::json& input, ToolCall& call) {
    call.id = input.value("id", std::string{});
    call.name = input.value("name", std::string{});
    if (input.contains("arguments")) {
        call.arguments_json = input["arguments"].is_string()
                                  ? input["arguments"].get<std::string>()
                                  : input["arguments"].dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
}

void to_json(nlohmann::json& output, const SourceReference& source) {
    output = {{"kind", source.kind}, {"label", source.label}, {"detail", source.detail}};
}

void from_json(const nlohmann::json& input, SourceReference& source) {
    source.kind = input.value("kind", std::string{});
    source.label = input.value("label", std::string{});
    source.detail = input.value("detail", std::string{});
}

void to_json(nlohmann::json& output, const ChatMessage& message) {
    output = {
        {"role", MessageRoleToString(message.role)},
        {"content", message.content},
    };
    if (!message.image_paths.empty()) output["image_paths"] = message.image_paths;
    if (!message.name.empty()) output["name"] = message.name;
    if (!message.tool_call_id.empty()) output["tool_call_id"] = message.tool_call_id;
    if (!message.tool_calls.empty()) output["tool_calls"] = message.tool_calls;
    if (!message.reasoning_text.empty()) output["reasoning_text"] = message.reasoning_text;
    if (!message.reasoning_items.empty()) output["reasoning_items"] = message.reasoning_items;
    if (!message.sources.empty()) output["sources"] = message.sources;
    if (message.timestamp != 0) output["timestamp"] = message.timestamp;
}

void from_json(const nlohmann::json& input, ChatMessage& message) {
    message.role = MessageRoleFromString(input.value("role", std::string("user")));
    message.content = input.value("content", std::string{});
    if (input.contains("image_paths") && input["image_paths"].is_array()) {
        message.image_paths = input["image_paths"].get<std::vector<std::string>>();
    }
    message.name = input.value("name", std::string{});
    message.tool_call_id = input.value("tool_call_id", std::string{});
    if (input.contains("tool_calls") && input["tool_calls"].is_array()) {
        message.tool_calls = input["tool_calls"].get<std::vector<ToolCall>>();
    }
    message.reasoning_text = input.value("reasoning_text", std::string{});
    if (input.contains("reasoning_items") && input["reasoning_items"].is_array()) {
        message.reasoning_items = input["reasoning_items"].get<std::vector<std::string>>();
    }
    if (input.contains("sources") && input["sources"].is_array()) {
        message.sources = input["sources"].get<std::vector<SourceReference>>();
    }
    message.timestamp = input.value("timestamp", uint64_t{0});
}

}  // namespace spark
