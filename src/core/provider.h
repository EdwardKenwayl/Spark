#pragma once

#include "core/agent_types.h"
#include "core/config.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace spark {

struct ProviderRequest {
    const ProviderConfig* provider = nullptr;
    const ModelConfig* model = nullptr;
    std::vector<ChatMessage> messages;
    std::vector<ToolDefinition> tools;
    std::string reasoning_effort;
    // Stable per-session routing key used by prompt-cache aware providers.
    std::string prompt_cache_key;
    int request_timeout_seconds = 180;
    int response_header_timeout_seconds = 600;
};

struct ProviderCallbacks {
    std::function<void(const std::string&)> on_text_delta;
    std::function<void(const std::string&)> on_reasoning_delta;
    std::function<void(const std::string&)> on_status;
    std::function<void(int)> on_http_status;
};

class ProviderAdapter {
public:
    virtual ~ProviderAdapter() = default;
    virtual Result<ModelReply> Complete(const ProviderRequest& request, const ProviderCallbacks& callbacks,
                                        CancellationToken* cancellation) = 0;
};

std::unique_ptr<ProviderAdapter> CreateProviderAdapter(const std::string& protocol);
std::string ProviderEndpointFor(const ProviderConfig& provider);
std::string ProviderConnectionCheckUrl(const ProviderConfig& provider);

}  // namespace spark
