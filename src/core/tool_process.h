#pragma once

#include "core/tools.h"

namespace spark {

ToolExecutionResult ExecuteShellCommand(const std::string& command, const std::string& directory,
                                        int timeout_seconds, const ToolContext& context);

}  // namespace spark
