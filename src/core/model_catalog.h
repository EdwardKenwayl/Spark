#pragma once

#include "core/config.h"

namespace spark {

Result<ActiveSelection> ResolveSubAgentSelection(const AppConfig& config, const ActiveSelection& requested);
// Pages contain public model metadata only, capped at 4 KiB per tool result.
nlohmann::json ConfiguredModelsPage(const AppConfig& config, size_t offset, size_t limit);

}  // namespace spark
