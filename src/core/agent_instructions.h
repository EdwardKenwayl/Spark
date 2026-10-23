#pragma once

#include <string>

namespace spark {

// Returns a bounded, directory-scoped AGENTS.md fragment for the model system prompt.
// The loader supports Spark's own global file plus the nearest Git project root
// and every directory between that root and the active workspace.
std::string BuildAgentInstructionsPrompt(const std::string& workspace);

}  // namespace spark
