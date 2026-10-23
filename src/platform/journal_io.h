#pragma once

#include "core/common.h"

namespace spark {

bool AppendJournalRecord(const std::string& path, const std::string& text, std::string* error);
Result<std::vector<std::string>> RecentJournalFiles(const std::string& directory, int limit);

}  // namespace spark
