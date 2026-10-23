#pragma once

#include "core/agent_types.h"
#include "core/config.h"

#include <cstdint>
#include <string>
#include <vector>

namespace spark {

uint64_t EstimateTextTokens(const std::string& text);
uint64_t EstimateMessageTokens(const ChatMessage& message);
uint64_t EstimateMessagesTokens(const std::vector<ChatMessage>& messages);
uint64_t EstimateToolDefinitionTokens(const ToolDefinition& definition);
uint64_t EstimateToolsTokens(const std::vector<ToolDefinition>& tools);
uint64_t EstimateContextTokens(const std::vector<ChatMessage>& messages, const std::vector<ToolDefinition>& tools,
                               const std::string& system_prompt);
uint64_t EffectiveContextLimit(const ContextConfig& context, const ModelConfig& model);
bool ShouldCompactContext(const ContextConfig& context, const ModelConfig& model, uint64_t estimated_tokens);

std::string BuildHandoffSummary(const std::vector<ChatMessage>& messages, const std::string& trigger,
                                const std::string& workspace, size_t max_tokens);
// Makes a history acceptable to tool-calling providers without changing the stored transcript:
// orphan tool outputs are removed, and retained tool calls with no result get a synthetic result.
std::vector<ChatMessage> NormalizeToolCallHistory(const std::vector<ChatMessage>& messages);
std::vector<ChatMessage> BuildCompactedHistory(const std::vector<ChatMessage>& messages, const std::string& summary);

}  // namespace spark
