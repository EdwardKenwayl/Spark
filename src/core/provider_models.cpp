#include "core/provider_models.h"

#include "core/provider.h"
#include "platform/http.h"

#include <algorithm>
#include <charconv>
#include <exception>
#include <set>

namespace spark {

Result<std::vector<std::string>> FetchProviderModelIds(const ProviderConfig& provider,
                                                      CancellationToken* cancellation) {
    using ModelIdsResult = Result<std::vector<std::string>>;
    constexpr size_t kMaxResponseBytes = 1024 * 1024;
    constexpr size_t kMaxModelIds = 256;
    constexpr size_t kMaxModelIdBytes = 2048;
    if (cancellation != nullptr && cancellation->IsCancelled()) return ModelIdsResult::Failure("cancelled");

    try {
        std::string api_key = provider.api_key;
        if (!provider.api_key_expression.empty()) {
            const nlohmann::json wrapper = {
                {"providers", {{"models_fetch", {{"api_key", provider.api_key_expression}}}}}
            };
            const AppConfig resolved = ConfigFromJson(wrapper, {});
            api_key = resolved.providers.front().api_key;
        }

        HttpRequest request;
        request.method = "GET";
        request.url = ProviderConnectionCheckUrl(provider);
        request.headers = provider.headers;
        request.timeout_seconds = 20;
        request.header_timeout_seconds = 20;
        request.max_attempts = 1;
        for (auto iterator = request.headers.begin(); iterator != request.headers.end();) {
            if (EqualsIgnoreCaseAscii(iterator->first, "Accept")) {
                iterator = request.headers.erase(iterator);
            } else {
                ++iterator;
            }
        }
        request.headers["Accept"] = "application/json";
        const auto default_header = [&](const char* name, const std::string& value) {
            const bool present = std::any_of(request.headers.begin(), request.headers.end(), [&](const auto& header) {
                return EqualsIgnoreCaseAscii(header.first, name);
            });
            if (!present) request.headers[name] = value;
        };
        if (LowerAscii(Trim(provider.protocol)) == "anthropic_messages") {
            if (!api_key.empty()) default_header("x-api-key", api_key);
            default_header("anthropic-version", "2023-06-01");
        } else if (!api_key.empty()) {
            default_header("Authorization", "Bearer " + api_key);
        }

        std::string body;
        bool oversized = false;
        const auto response = HttpRequestStreaming(request, [&](const char* data, size_t size) {
            if (cancellation != nullptr && cancellation->IsCancelled()) return StreamAction::Cancel;
            if (size > kMaxResponseBytes - body.size()) {
                oversized = true;
                // Stop retains the HTTP status; the flag still forbids partial success.
                return StreamAction::Stop;
            }
            body.append(data, size);
            return StreamAction::Continue;
        }, cancellation);
        if (cancellation != nullptr && cancellation->IsCancelled()) return ModelIdsResult::Failure("cancelled");
        if (!response.ok) return ModelIdsResult::Failure("Could not fetch provider models: HTTP transport failed.");
        if (response.value.status < 200 || response.value.status >= 300) {
            return ModelIdsResult::Failure("Provider models request returned HTTP " + std::to_string(response.value.status) + ".");
        }
        if (oversized) return ModelIdsResult::Failure("Provider models response exceeds 1 MiB.");
        for (const auto& [name, value] : response.value.headers) {
            if (!EqualsIgnoreCaseAscii(name, "Content-Length")) continue;
            size_t expected_bytes = 0;
            const auto length = std::from_chars(value.data(), value.data() + value.size(), expected_bytes);
            if (length.ec != std::errc{} || length.ptr != value.data() + value.size() || expected_bytes != body.size()) {
                return ModelIdsResult::Failure("Incomplete provider models HTTP response.");
            }
        }

        bool excessive_depth = false;
        const auto document = nlohmann::json::parse(body, [&](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
            if (depth > 64) excessive_depth = true;
            return !excessive_depth;
        }, false);
        if (cancellation != nullptr && cancellation->IsCancelled()) return ModelIdsResult::Failure("cancelled");
        if (excessive_depth || !document.is_object()) return ModelIdsResult::Failure("Invalid provider models JSON response.");
        const auto data = document.find("data");
        if (data == document.end() || !data->is_array()) {
            return ModelIdsResult::Failure("Provider models response must contain a data array.");
        }

        std::vector<std::string> ids;
        std::set<std::string> seen;
        for (const auto& model : *data) {
            if (cancellation != nullptr && cancellation->IsCancelled()) return ModelIdsResult::Failure("cancelled");
            if (!model.is_object()) continue;
            const auto id = model.find("id");
            if (id == model.end() || !id->is_string()) continue;
            const auto& value = id->get_ref<const std::string&>();
            if (value.empty()) continue;
            if (value.size() > kMaxModelIdBytes) return ModelIdsResult::Failure("Provider model ID exceeds 2048 bytes.");
            if (seen.find(value) != seen.end()) continue;
            if (ids.size() >= kMaxModelIds) return ModelIdsResult::Failure("Provider models response exceeds 256 unique IDs.");
            seen.insert(value);
            ids.push_back(value);
        }
        if (ids.empty()) return ModelIdsResult::Failure("Provider models response contains no model IDs.");
        if (cancellation != nullptr && cancellation->IsCancelled()) return ModelIdsResult::Failure("cancelled");
        return ModelIdsResult::Success(std::move(ids));
    } catch (const std::exception&) {
        // Exception text can include upstream JSON or credential-bearing input.
        return ModelIdsResult::Failure("Could not fetch provider models.");
    }
}

}  // namespace spark
