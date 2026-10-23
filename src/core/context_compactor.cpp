#include "core/context_compactor.h"

#include "core/common.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace spark {
namespace {

constexpr uint64_t kMinimumEffectiveLimit = 1024;

std::string ClipText(const std::string& text, size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    std::string clipped = text.substr(0, max_bytes);
    clipped += "\n[summary truncated]";
    return clipped;
}

bool HasErrorEvidence(const std::string& text) {
    const std::string lower = LowerAscii(text);
    return lower.find("error") != std::string::npos || lower.find("failed") != std::string::npos ||
           lower.find("exception") != std::string::npos || lower.find("失败") != std::string::npos ||
           lower.find("错误") != std::string::npos;
}

std::string FileOperationLine(const ChatMessage& message) {
    if (message.role != MessageRole::Tool) return {};
    if (message.name != "write_file" && message.name != "apply_patch") return {};
    return "- " + ClipText(message.content, 800);
}

std::string ToolEvidenceLine(const ChatMessage& message) {
    if (message.role != MessageRole::Tool) return {};
    if (!HasErrorEvidence(message.content)) return {};
    return "- " + message.name + ": " + ClipText(message.content, 1000);
}

}  // namespace

uint64_t EstimateTextTokens(const std::string& text) {
    double tokens = 0.0;
    for (size_t index = 0; index < text.size();) {
        const unsigned char character = static_cast<unsigned char>(text[index]);
        if (character < 0x80) {
            tokens += character == '\n' || character == '\t' ? 0.5 : 0.25;
            ++index;
            continue;
        }
        size_t sequence_length = 1;
        if ((character & 0xE0) == 0xC0) sequence_length = 2;
        else if ((character & 0xF0) == 0xE0) sequence_length = 3;
        else if ((character & 0xF8) == 0xF0) sequence_length = 4;
        sequence_length = std::min(sequence_length, text.size() - index);
        tokens += sequence_length >= 4 ? 2.0 : 1.0;
        index += sequence_length;
    }
    return static_cast<uint64_t>(std::ceil(tokens)) + 1;
}

uint64_t EstimateMessageTokens(const ChatMessage& message) {
    uint64_t tokens = 4 + EstimateTextTokens(message.content) + EstimateTextTokens(message.name) +
                      EstimateTextTokens(message.tool_call_id);
    for (const std::string& path : message.image_paths) {
        uint64_t image_tokens = 256;
        std::error_code error;
        const uint64_t image_bytes = spark::fs::file_size(NativePath(path), error);
        if (!error && image_bytes > 0) {
            // Images are sent as base64 in the request. Base64 text is roughly
            // one token per three source bytes, so the old fixed 32-token estimate
            // could miss a very large request by hundreds of thousands of tokens.
            image_tokens += std::min<uint64_t>(image_bytes / 3, 500000);
        } else {
            image_tokens += 4096;
        }
        tokens += image_tokens + EstimateTextTokens(path);
    }
    for (const ToolCall& call : message.tool_calls) {
        tokens += 6 + EstimateTextTokens(call.id) + EstimateTextTokens(call.name) +
                  EstimateTextTokens(call.arguments_json);
    }
    return tokens;
}

uint64_t EstimateMessagesTokens(const std::vector<ChatMessage>& messages) {
    uint64_t tokens = 0;
    for (const ChatMessage& message : messages) tokens += EstimateMessageTokens(message);
    return tokens;
}

uint64_t EstimateToolDefinitionTokens(const ToolDefinition& definition) {
    return 8 + EstimateTextTokens(definition.name) + EstimateTextTokens(definition.description) +
           EstimateTextTokens(definition.parameters.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

uint64_t EstimateToolsTokens(const std::vector<ToolDefinition>& tools) {
    uint64_t tokens = 0;
    for (const ToolDefinition& definition : tools) tokens += EstimateToolDefinitionTokens(definition);
    return tokens;
}

uint64_t EstimateContextTokens(const std::vector<ChatMessage>& messages, const std::vector<ToolDefinition>& tools,
                               const std::string& system_prompt) {
    return EstimateMessagesTokens(messages) + EstimateToolsTokens(tools) + EstimateTextTokens(system_prompt) + 64;
}

std::vector<ChatMessage> NormalizeToolCallHistory(const std::vector<ChatMessage>& messages) {
    std::vector<std::string> retained_call_ids;
    for (const ChatMessage& message : messages) {
        if (message.role != MessageRole::Assistant) continue;
        for (const ToolCall& call : message.tool_calls) retained_call_ids.push_back(call.id);
    }
    std::vector<std::string> output_ids;
    for (const ChatMessage& message : messages) {
        if (message.role != MessageRole::Tool) continue;
        if (std::find(retained_call_ids.begin(), retained_call_ids.end(), message.tool_call_id) !=
            retained_call_ids.end()) {
            output_ids.push_back(message.tool_call_id);
        }
    }

    std::vector<ChatMessage> normalized;
    normalized.reserve(messages.size() + retained_call_ids.size());
    for (const ChatMessage& source : messages) {
        // A malformed or interrupted journal can contain a tool result whose function call was
        // already removed. That result must never be sent on its own.
        if (source.role == MessageRole::Tool &&
            std::find(retained_call_ids.begin(), retained_call_ids.end(), source.tool_call_id) ==
                retained_call_ids.end()) {
            continue;
        }
        ChatMessage message = source;
        const bool opens_calls = message.role == MessageRole::Assistant && !message.tool_calls.empty();
        std::vector<ToolCall> calls = opens_calls ? message.tool_calls : std::vector<ToolCall>{};
        normalized.push_back(std::move(message));
        for (const ToolCall& call : calls) {
            if (std::find(output_ids.begin(), output_ids.end(), call.id) != output_ids.end()) continue;
            ChatMessage missing;
            missing.role = MessageRole::Tool;
            missing.name = call.name;
            missing.tool_call_id = call.id;
            missing.content = "[tool result unavailable: the call did not return output]";
            output_ids.push_back(call.id);
            normalized.push_back(std::move(missing));
        }
    }
    return normalized;
}

uint64_t EffectiveContextLimit(const ContextConfig& context, const ModelConfig& model) {
    const uint64_t limit = static_cast<uint64_t>(std::max(1, model.capabilities.context_window > 0
        ? model.capabilities.context_window : context.fallback_context_window));
    return std::max(limit, kMinimumEffectiveLimit);
}

bool ShouldCompactContext(const ContextConfig& context, const ModelConfig& model, uint64_t estimated_tokens) {
    uint64_t limit = EffectiveContextLimit(context, model);
    const int configured_percent = model.capabilities.auto_compact_threshold_percent > 0
        ? model.capabilities.auto_compact_threshold_percent
        : context.auto_compact_threshold_percent;
    const int percent = std::max(1, std::min(100, configured_percent));
    const uint64_t threshold = std::max<uint64_t>(1, limit * static_cast<uint64_t>(percent) / 100);
    const uint64_t reserve = static_cast<uint64_t>(std::max(0, context.reserve_output_tokens));
    if (reserve > 0) {
        limit = limit > reserve + kMinimumEffectiveLimit ? limit - reserve : std::max(limit * 3 / 4, kMinimumEffectiveLimit);
    }
    return estimated_tokens >= std::min(limit, threshold);
}

std::string BuildHandoffSummary(const std::vector<ChatMessage>& messages, const std::string& trigger,
                                const std::string& workspace, size_t max_tokens) {
    std::string last_goal;
    std::vector<std::string> completed;
    std::vector<std::string> files;
    std::vector<std::string> evidence;
    std::vector<std::string> side_effects;

    for (const ChatMessage& message : messages) {
        if (message.role == MessageRole::User && !message.content.empty()) {
            last_goal = message.content;
        } else if (message.role == MessageRole::Assistant && !message.content.empty()) {
            completed.push_back("- " + ClipText(message.content, 1400));
        } else if (message.role == MessageRole::Tool) {
            const std::string file_line = FileOperationLine(message);
            if (!file_line.empty()) files.push_back(file_line);
            const std::string evidence_line = ToolEvidenceLine(message);
            if (!evidence_line.empty()) evidence.push_back(evidence_line);
            if (message.name == "write_file" || message.name == "apply_patch" || message.name == "run_command") {
                side_effects.push_back("- " + message.name + ": " + ClipText(message.content, 500));
            }
        }
    }

    std::ostringstream summary;
    summary << "[目标]\n" << ClipText(last_goal.empty() ? "继续当前会话未完成的任务。" : last_goal, 4000) << "\n\n";
    summary << "[用户偏好]\n"
            << "沿用会话中已经确认的语言、路径、权限和工作区约束；不得自行扩大访问范围。\n\n";
    summary << "[已完成]\n";
    if (completed.empty()) summary << "尚无已完成的助手结论。\n";
    else for (size_t index = 0; index < std::min<size_t>(completed.size(), 8); ++index) summary << completed[index] << "\n";
    summary << "\n[已修改文件]\n";
    if (files.empty()) summary << "尚未记录成功写入、补丁或明确文件变更。\n";
    else for (size_t index = 0; index < std::min<size_t>(files.size(), 12); ++index) summary << files[index] << "\n";
    summary << "\n[当前状态]\n"
            << "触发压缩：" << trigger << "。工作区：" << workspace
            << "。压缩后只恢复摘要和最近一轮未完成工具链；没有自动重放任何工具。\n\n";
    summary << "[重要证据]\n";
    if (evidence.empty()) summary << "没有保留到错误证据。\n";
    else for (size_t index = 0; index < std::min<size_t>(evidence.size(), 8); ++index) summary << evidence[index] << "\n";
    summary << "\n[禁止重复]\n";
    if (side_effects.empty()) summary << "没有检测到已执行但可能重复产生副作用的工具调用。\n";
    else {
        summary << "以下操作已经执行或请求执行，恢复后不得自动重放：\n";
        for (size_t index = 0; index < std::min<size_t>(side_effects.size(), 12); ++index) {
            summary << side_effects[index] << "\n";
        }
    }
    summary << "\n[下一步]\n"
            << "1. 只根据最近用户消息继续。\n"
            << "2. 先检查当前工作区和文件状态，再决定是否需要新的工具调用。\n"
            << "3. 对不确定的旧操作重新读取证据，不假设旧工具结果仍然有效。\n";

    return ClipText(summary.str(), std::max<size_t>(2048, max_tokens * 3));
}

std::vector<ChatMessage> BuildCompactedHistory(const std::vector<ChatMessage>& messages, const std::string& summary) {
    std::vector<ChatMessage> compacted;
    ChatMessage handoff;
    handoff.role = MessageRole::System;
    handoff.content = "Conversation history handoff summary:\n" + summary;
    compacted.push_back(std::move(handoff));

    // A retained tail must begin where the provider can accept it: either on a user turn or on
    // the assistant message that opened a tool-call group. Cutting anywhere else (for example on
    // a tool result) sends a function_call_output with no matching function_call, which providers
    // reject with "No tool call found for tool output" and which also derails the model because
    // the replayed transcript no longer reads coherently.
    const auto safe_boundary = [&](size_t index) {
        if (index >= messages.size()) return false;
        if (messages[index].role == MessageRole::User) return true;
        return messages[index].role == MessageRole::Assistant && !messages[index].tool_calls.empty();
    };

    size_t tail_start = messages.size();
    bool has_user_turn = false;
    for (size_t index = messages.size(); index > 0; --index) {
        if (messages[index - 1].role == MessageRole::User) {
            tail_start = index - 1;
            has_user_turn = true;
            break;
        }
    }
    if (has_user_turn) {
        // Prefer the last assistant tool-call group so the retained tail stays small.
        for (size_t index = messages.size(); index > tail_start; --index) {
            if (messages[index - 1].role == MessageRole::Assistant && !messages[index - 1].tool_calls.empty()) {
                tail_start = index - 1;
                break;
            }
        }
    } else {
        // Already-compacted context carries the handoff summary instead of a user turn. Keep a
        // bounded window and then snap the start forward to the first safe boundary.
        const size_t window_start = messages.size() > 4 ? messages.size() - 4 : 0;
        tail_start = messages.size();
        for (size_t index = window_start; index < messages.size(); ++index) {
            if (safe_boundary(index)) {
                tail_start = index;
                break;
            }
        }
        if (tail_start == messages.size()) {
            for (size_t index = window_start; index > 0; --index) {
                if (safe_boundary(index - 1)) {
                    tail_start = index - 1;
                    break;
                }
            }
        }
    }
    for (size_t index = tail_start; index < messages.size(); ++index) {
        ChatMessage tail = messages[index];
        tail.content = ClipText(tail.content, 16000);
        for (ToolCall& call : tail.tool_calls) call.arguments_json = ClipText(call.arguments_json, 8000);
        compacted.push_back(std::move(tail));
    }

    return NormalizeToolCallHistory(compacted);
}

}  // namespace spark
