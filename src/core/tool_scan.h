#pragma once

#include "core/tools.h"

namespace spark {

// The caller resolves and authorizes path before scanning. Output includes the
// scan footer within max_tool_output_bytes (512-byte minimum, 64 KiB ceiling).
ToolExecutionResult ScanWorkspace(const std::string& path, const std::string& workspace,
                                 const nlohmann::json& args, const ToolContext& ctx, bool search);

}  // namespace spark
