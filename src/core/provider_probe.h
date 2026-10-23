#pragma once

#include "core/config.h"

#include <string>

namespace spark {

struct ProviderProbeResult {
    // Display-only, credential-redacted UTF-8 strings, at most 512 bytes each.
    std::string model_id;
    std::string text;
    long long elapsed_ms = 0;
    int http_status = 0;  // Zero means no HTTP response headers were received.
};

std::string ProviderProbeModelId(const ProviderConfig& provider);

// Synchronous inference probe; does not create sessions or change the provider.
// Chooses test_model, otherwise the first configured model. Failure values retain
// model, HTTP status and timing; errors are redacted and bounded.
Result<ProviderProbeResult> ProbeProvider(const ProviderConfig& provider, CancellationToken* cancellation = nullptr);

}  // namespace spark
