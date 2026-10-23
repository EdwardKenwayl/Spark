#include "core/model_catalog.h"

#include <algorithm>

namespace spark {

Result<ActiveSelection> ResolveSubAgentSelection(const AppConfig& config, const ActiveSelection& requested) {
    ActiveSelection selected = config.active;
    if (!Trim(requested.provider).empty()) selected.provider = Trim(requested.provider);
    if (!Trim(requested.model).empty()) selected.model = Trim(requested.model);
    const auto* provider = FindProvider(config, selected.provider);
    if (provider == nullptr) return Result<ActiveSelection>::Failure("unknown provider: " + selected.provider);
    const auto* model = FindModel(*provider, selected.model);
    if (model == nullptr) {
        return Result<ActiveSelection>::Failure("provider " + selected.provider + " has no model " +
                                               selected.model + "; select provider and model from list_models");
    }
    if (provider->api_key.empty()) return Result<ActiveSelection>::Failure("selected provider has no API key");
    const auto efforts = EffectiveReasoningEfforts(*model);
    const std::string explicit_effort = Trim(requested.reasoning_effort);
    if (!explicit_effort.empty()) {
        if (!model->capabilities.reasoning ||
            std::find(efforts.begin(), efforts.end(), explicit_effort) == efforts.end()) {
            return Result<ActiveSelection>::Failure("reasoning_effort is not supported by the selected model");
        }
        selected.reasoning_effort = explicit_effort;
    } else if (selected.provider != config.active.provider || selected.model != config.active.model) {
        selected.reasoning_effort = EffectiveDefaultReasoningEffort(*model);
    }
    return Result<ActiveSelection>::Success(std::move(selected));
}

nlohmann::json ConfiguredModelsPage(const AppConfig& config, size_t offset, size_t limit) {
    constexpr size_t kMaxBytes = 4096;
    limit = std::clamp<size_t>(limit, 1, 16);
    nlohmann::json page = {{"models", nlohmann::json::array()}, {"next_offset", nullptr}};
    size_t index = 0;
    for (const auto& provider : config.providers) {
        for (const auto& model : provider.models) {
            if (index++ < offset) continue;
            if (page["models"].size() == limit) {
                page["next_offset"] = index - 1;
                return page;
            }
            page["models"].push_back({
                {"provider", provider.id}, {"model", model.id},
                {"display_name", TruncateUtf8(model.display_name.empty() ? model.id : model.display_name, 128)},
                {"available", !provider.api_key.empty()},
                {"supports_tools", model.capabilities.tool_calls}, {"supports_images", model.capabilities.images},
                {"supports_reasoning", model.capabilities.reasoning},
                {"reasoning_efforts", model.capabilities.reasoning ? EffectiveReasoningEfforts(model)
                                                               : std::vector<std::string>{}},
                {"default_reasoning_effort", EffectiveDefaultReasoningEffort(model)},
            });
            // Reserve room for the continuation offset and a diagnostic.
            if (page.dump().size() > kMaxBytes - 128) {
                page["models"].erase(page["models"].end() - 1);
                page["next_offset"] = index - 1;
                if (page["models"].empty()) {
                    page["error"] = "Model metadata exceeds the page size limit.";
                    page["next_offset"] = index;
                }
                return page;
            }
        }
    }
    return page;
}

}  // namespace spark
