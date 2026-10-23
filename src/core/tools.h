#pragma once

#include "core/agent_types.h"
#include "core/common.h"
#include "core/config.h"

#include <functional>
#include <string>
#include <vector>

namespace spark {

using ApprovalCallback = std::function<bool(const ApprovalRequest&)>;

struct ToolContext {
    WorkspaceConfig workspace;
    AgentLimitsConfig limits;
    ApprovalCallback approve;
    CancellationToken* cancellation = nullptr;
    std::vector<std::string> secrets;
    std::string shell = UiConfig{}.shell;
};

struct ToolExecutionResult {
    bool ok = false;
    bool side_effecting = false;
    std::string output;
    std::vector<std::string> image_paths;
};

class ToolRegistry {
public:
    ToolRegistry();

    const std::vector<ToolDefinition>& Definitions() const { return definitions_; }
    ToolExecutionResult Execute(const std::string& name, const std::string& arguments_json, const ToolContext& context) const;

    static std::string WorkspacePath(const ToolContext& context, const std::string& path);
    static bool EnsurePathAllowed(const ToolContext& context, const std::string& path, bool write, std::string* error);

private:
    std::vector<ToolDefinition> definitions_;
};

std::string ToolError(const std::string& message);

}  // namespace spark
