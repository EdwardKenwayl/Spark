#pragma once

#include "core/agent_types.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace spark {
struct PromptImage {
    std::string bytes;
    std::string mime;
};

// Prepared copies are shared by all wire protocols; source attachments stay untouched.
std::shared_ptr<const PromptImage> LoadPromptImage(std::string_view path);
std::vector<ChatMessage> PreparePromptImageHistory(const std::vector<ChatMessage>& messages);
}  // namespace spark
