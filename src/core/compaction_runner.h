#pragma once

#include "core/agent_types.h"
#include "core/config.h"
#include "core/provider.h"

#include <cstdint>
#include <string>
#include <vector>

namespace spark {

struct CompactionSummaryResult {
    std::string summary;
    std::string source = "model";
    int passes = 0;
    uint64_t before_tokens = 0;
};

Result<CompactionSummaryResult> GenerateCompactionSummary(
    const ContextConfig& context, const ProviderConfig& provider, const ModelConfig& model,
    ProviderAdapter& adapter, const std::vector<ChatMessage>& messages, const std::string& trigger,
    const std::string& workspace, int request_timeout_seconds, CancellationToken* cancellation);

}  // namespace spark
