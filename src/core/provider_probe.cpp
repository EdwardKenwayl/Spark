#include "core/provider_probe.h"

#include "core/provider.h"

#include <algorithm>
#include <chrono>
#include <exception>

namespace spark {
namespace {

std::string ProbeDisplayText(std::string text, const std::vector<std::string>& secrets) {
    text = DecodeBytesToUtf8(SanitizeDisplayText(text), 65001);
    std::string redacted;
    for (size_t offset = 0; offset < text.size();) {
        const auto secret = std::find_if(secrets.begin(), secrets.end(), [&](const std::string& value) {
            return !value.empty() && text.compare(offset, value.size(), value) == 0;
        });
        if (secret == secrets.end()) {
            redacted.push_back(text[offset++]);
        } else {
            redacted += "[REDACTED]";
            offset += secret->size();
        }
    }
    return TruncateUtf8(std::move(redacted), 512);
}

}  // namespace

std::string ProviderProbeModelId(const ProviderConfig& provider) {
    const auto test_model = Trim(provider.test_model);
    return !test_model.empty() ? test_model : provider.models.empty() ? "" : Trim(provider.models.front().id);
}

Result<ProviderProbeResult> ProbeProvider(const ProviderConfig& provider, CancellationToken* cancellation) {
    using ProbeResult = Result<ProviderProbeResult>;
    const auto started = std::chrono::steady_clock::now();
    ProviderProbeResult result;
    const auto finish = [&](bool ok, std::string error = {}) {
        result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        return ProbeResult{ok, std::move(result), std::move(error)};
    };
    if (cancellation != nullptr && cancellation->IsCancelled()) return finish(false, "cancelled");
    try {
        ProviderConfig resolved = provider;
        if (Trim(resolved.base_url).empty()) return finish(false, "Provider base URL is empty.");
        auto adapter = CreateProviderAdapter(resolved.protocol);
        if (adapter == nullptr) return finish(false, "Unsupported provider protocol.");

        const std::string model_id = ProviderProbeModelId(resolved);
        const ModelConfig* selected = FindModel(resolved, model_id);
        ModelConfig model;
        if (selected != nullptr) {
            model = *selected;
        } else {
            if (!resolved.models.empty()) model = resolved.models.front();
            model.id = model_id;
        }
        if (Trim(model.id).empty()) return finish(false, "No model is available for the provider probe.");

        if (!resolved.api_key_expression.empty()) {
            // Reuse the configuration parser's literal / ${ENV:NAME} semantics.
            const nlohmann::json wrapper = {
                {"providers", {{"probe", {{"api_key", resolved.api_key_expression}}}}}
            };
            resolved.api_key = ConfigFromJson(wrapper, {}).providers.front().api_key;
        }
        std::vector<std::string> secrets;
        const auto add_secret = [&](const std::string& value) {
            if (value.empty()) return;
            secrets.push_back(value);
            secrets.push_back(SanitizeDisplayText(value));
            const std::string escaped = nlohmann::json(value).dump(
                -1, ' ', false, nlohmann::json::error_handler_t::replace);
            secrets.push_back(escaped.substr(1, escaped.size() - 2));
        };
        add_secret(provider.api_key);
        add_secret(provider.api_key_expression);
        add_secret(resolved.api_key);
        for (const auto& [name, value] : resolved.headers) {
            // Every custom header is private, not just names containing "key".
            add_secret(value);
            add_secret(Trim(value));
            if (EqualsIgnoreCaseAscii(name, "Authorization") || EqualsIgnoreCaseAscii(name, "Proxy-Authorization")) {
                const size_t separator = value.find(' ');
                if (separator != std::string::npos) add_secret(Trim(value.substr(separator + 1)));
            }
        }
        std::sort(secrets.begin(), secrets.end(), [](const auto& left, const auto& right) {
            return left.size() > right.size() || (left.size() == right.size() && left < right);
        });
        secrets.erase(std::unique(secrets.begin(), secrets.end()), secrets.end());
        result.model_id = ProbeDisplayText(model.id, secrets);

        ChatMessage message;
        message.role = MessageRole::User;
        message.content = "hi";
        ProviderRequest request;
        request.provider = &resolved;
        request.model = &model;
        request.messages.push_back(std::move(message));
        request.request_timeout_seconds = 90;
        request.response_header_timeout_seconds = 90;
        ProviderCallbacks callbacks;
        callbacks.on_http_status = [&](int status) { result.http_status = status; };
        const auto reply = adapter->Complete(request, callbacks, cancellation);
        if (cancellation != nullptr && cancellation->IsCancelled()) return finish(false, "cancelled");
        if (!reply.ok) {
            std::string error = ProbeDisplayText(reply.error, secrets);
            if (Trim(error).empty()) error = "Provider probe failed.";
            return finish(false, std::move(error));
        }
        const std::string text = ProbeDisplayText(reply.value.text, secrets);
        if (Trim(text).empty()) return finish(false, "Provider probe returned no text.");
        result.text = text;
        return finish(true);
    } catch (const std::exception&) {
        // Adapter/JSON exception messages may embed raw requests or credentials.
        if (cancellation != nullptr && cancellation->IsCancelled()) return finish(false, "cancelled");
        return finish(false, "Provider probe failed.");
    }
}

}  // namespace spark
