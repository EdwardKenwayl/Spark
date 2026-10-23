#include "core/compaction_runner.h"

#include "core/common.h"
#include "core/context_compactor.h"

#include <algorithm>
#include <sstream>

namespace spark {
namespace {

constexpr size_t kMaximumMessageBytes = 192 * 1024;
constexpr size_t kMaximumToolArgumentBytes = 64 * 1024;
constexpr uint64_t kFallbackSummaryWindow = 200000;
constexpr uint64_t kPromptReserveTokens = 4096;

std::string SummaryPrompt(const ContextConfig& context, const std::string& trigger, const std::string& workspace,
                          int pass, int total_passes) {
    std::ostringstream prompt;
    prompt << "You are performing a CONTEXT CHECKPOINT COMPACTION for Spark.\n"
           << "Return only a structured handoff summary. Do not call tools. Do not continue the user's task.\n"
           << "Merge the previous handoff summary, when present, with the conversation chunk in this request.\n"
           << "Preserve concrete paths, commands, errors, decisions, completed work, current state, and next steps.\n"
           << "Do not preserve irrelevant raw output. Never claim an action succeeded without evidence in the input.\n"
           << "Use these sections exactly: [目标] [用户偏好] [已完成] [已修改文件] [当前状态] [重要证据] [禁止重复] [下一步].\n"
           << "Keep the result under " << std::max(512, context.summary_max_tokens) << " tokens.\n"
           << "Trigger: " << trigger << "\n"
           << "Workspace: " << workspace << "\n"
           << "Summary pass: " << pass << " of " << total_passes << "\n";
    return prompt.str();
}

uint64_t SummaryInputBudget(const ContextConfig& context, const ModelConfig& model) {
    uint64_t window = model.capabilities.context_window > 0
                          ? static_cast<uint64_t>(model.capabilities.context_window)
                          : std::min<uint64_t>(kFallbackSummaryWindow,
                                               static_cast<uint64_t>(std::max(1, context.fallback_context_window)));
    const uint64_t reserve = static_cast<uint64_t>(std::max(1, context.reserve_output_tokens)) + kPromptReserveTokens;
    if (window > reserve) window -= reserve;
    return std::max<uint64_t>(4096, window * 9 / 10);
}

ChatMessage PrepareMessageForSummary(const ChatMessage& source) {
    ChatMessage message = source;
    message.content = TruncateUtf8(message.content, kMaximumMessageBytes);
    for (ToolCall& call : message.tool_calls) {
        call.arguments_json = TruncateUtf8(call.arguments_json, kMaximumToolArgumentBytes);
    }
    if (!message.image_paths.empty()) {
        std::ostringstream note;
        note << "\n[image attachments not resent during compaction:";
        const size_t shown = std::min<size_t>(message.image_paths.size(), 8);
        for (size_t index = 0; index < shown; ++index) note << " " << message.image_paths[index];
        if (message.image_paths.size() > shown) note << " ...";
        note << "]";
        message.content = TruncateUtf8(message.content + note.str(), kMaximumMessageBytes);
        message.image_paths.clear();
    }
    return message;
}

std::vector<std::vector<ChatMessage>> BuildSummaryChunks(const std::vector<ChatMessage>& messages,
                                                         uint64_t token_budget) {
    std::vector<std::vector<ChatMessage>> chunks;
    std::vector<ChatMessage> current;
    uint64_t current_tokens = 0;
    for (const ChatMessage& source : messages) {
        ChatMessage message = PrepareMessageForSummary(source);
        const uint64_t message_tokens = EstimateMessageTokens(message);
        const bool user_boundary = message.role == MessageRole::User;
        if (!current.empty() && current_tokens + message_tokens > token_budget && user_boundary) {
            chunks.push_back(std::move(current));
            current.clear();
            current_tokens = 0;
        }
        current.push_back(std::move(message));
        current_tokens += message_tokens;
    }
    if (!current.empty()) chunks.push_back(std::move(current));
    return chunks;
}

}  // namespace

Result<CompactionSummaryResult> GenerateCompactionSummary(
    const ContextConfig& context, const ProviderConfig& provider, const ModelConfig& model,
    ProviderAdapter& adapter, const std::vector<ChatMessage>& messages, const std::string& trigger,
    const std::string& workspace, int request_timeout_seconds, CancellationToken* cancellation) {
    if (messages.empty()) return Result<CompactionSummaryResult>::Failure("there is no context to summarize");
    if (cancellation != nullptr && cancellation->IsCancelled()) {
        return Result<CompactionSummaryResult>::Failure("cancelled");
    }

    const std::vector<std::vector<ChatMessage>> chunks = BuildSummaryChunks(messages, SummaryInputBudget(context, model));
    if (chunks.empty()) return Result<CompactionSummaryResult>::Failure("could not build a compaction chunk");

    CompactionSummaryResult result;
    result.before_tokens = EstimateMessagesTokens(messages);
    std::string rolling_summary;
    for (size_t index = 0; index < chunks.size(); ++index) {
        if (cancellation != nullptr && cancellation->IsCancelled()) {
            return Result<CompactionSummaryResult>::Failure("cancelled");
        }
        ProviderRequest request;
        request.provider = &provider;
        request.model = &model;
        request.request_timeout_seconds = std::max(1, request_timeout_seconds);
        if (!rolling_summary.empty()) {
            ChatMessage previous;
            previous.role = MessageRole::System;
            previous.content = "Previous handoff summary to merge:\n" + rolling_summary;
            request.messages.push_back(std::move(previous));
        }
        request.messages.insert(request.messages.end(), chunks[index].begin(), chunks[index].end());
        ChatMessage instruction;
        instruction.role = MessageRole::User;
        instruction.content = SummaryPrompt(context, trigger, workspace, static_cast<int>(index + 1),
                                            static_cast<int>(chunks.size()));
        request.messages.push_back(std::move(instruction));

        const auto reply = adapter.Complete(request, {}, cancellation);
        if (!reply.ok) return Result<CompactionSummaryResult>::Failure(reply.error);
        rolling_summary = Trim(reply.value.text);
        if (rolling_summary.empty()) {
            return Result<CompactionSummaryResult>::Failure("the model returned an empty compaction summary");
        }
        ++result.passes;
    }
    result.summary = std::move(rolling_summary);
    return Result<CompactionSummaryResult>::Success(std::move(result));
}

}  // namespace spark
