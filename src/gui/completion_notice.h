#pragma once

#include "core/session.h"

#include <optional>
#include <string>

namespace spark {

struct CompletionNotice {
    std::string path;
    std::wstring title;
    std::wstring body;
};

// Uses only the latest turn's final assistant text, never reasoning or tool output.
std::optional<CompletionNotice> MakeCompletionNotice(const std::string& path, const SessionState& state);
}  // namespace spark
