#pragma once

#include "core/common.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace spark {

struct WebSearchResult {
    std::string title;
    std::string url;
    std::string snippet;
};

std::vector<WebSearchResult> ParseDuckDuckGoSearchResults(std::string_view html, size_t max_results);
std::vector<WebSearchResult> ParseYahooSearchResults(std::string_view html, size_t max_results);
Result<std::string> SearchWeb(const std::string& query, size_t max_results, CancellationToken* cancellation);

}  // namespace spark
