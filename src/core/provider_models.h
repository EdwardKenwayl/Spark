#pragma once

#include "core/config.h"

#include <string>
#include <vector>

namespace spark {

Result<std::vector<std::string>> FetchProviderModelIds(const ProviderConfig& provider,
                                                      CancellationToken* cancellation = nullptr);

}  // namespace spark
