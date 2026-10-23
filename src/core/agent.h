#pragma once

#include "core/agent_types.h"
#include "core/config.h"
#include "core/mcp.h"
#include "core/provider.h"
#include "core/session.h"
#include "core/skills.h"
#include "core/subagents.h"
#include "core/tools.h"

#include <functional>
#include <string>
#include <vector>

namespace spark {

enum class CollaborationMode { Execute, Plan };

struct AgentTurnOptions {
    CollaborationMode mode = CollaborationMode::Execute;
    std::string goal;
};

struct AgentCallbacks {
    std::function<void(const std::string&)> on_status;
    std::function<void(AgentPhase, const std::string&)> on_phase;
    std::function<void()> on_user_message_appended;
    std::function<void(const std::string&)> on_text_delta;
    std::function<void(const std::string&)> on_reasoning_delta;
    std::function<void(const std::string&, const std::string&, const std::string&)> on_tool_start;
    std::function<void(const std::string&, const std::string&, bool, const std::string&)> on_tool_finish;
    std::function<void(int, int, int)> on_model_usage;
    // Reports the live context size after every provider call and after every compaction so the
    // UI can show one number instead of guessing from local estimates.
    std::function<void(uint64_t, uint64_t)> on_context_updated;
    // Reports a completed compaction so the transcript can show the same marker as Codex.
    std::function<void(uint64_t, uint64_t, const std::string&)> on_context_compacted;
    std::function<void()> on_turn_finished;
    std::function<void(const std::string&)> on_error;
};

class Agent {
public:
    Agent(const AppConfig& config, SessionJournal* journal, McpManager* mcp, SubAgentManager* subagents,
          AgentTurnOptions options = {});
    Agent(const AppConfig& config, SessionJournal* journal, McpManager* mcp);

    Result<void> RunTurn(const std::string& user_text, const ApprovalCallback& approval, AgentCallbacks callbacks,
                         CancellationToken* cancellation, bool continue_interrupted = false,
                         const std::vector<std::string>& image_paths = {});

private:
    std::string BuildSystemPrompt(const ProviderConfig& provider, const ModelConfig& model) const;
    Result<ToolExecutionResult> ExecuteReadSkill(const std::string& arguments_json) const;
    Result<ToolExecutionResult> ExecuteTool(const ToolCall& call, const ApprovalCallback& approval,
                                            AgentCallbacks& callbacks, CancellationToken* cancellation,
                                            bool notify_callbacks = true) const;

    const AppConfig& config_;
    SessionJournal* journal_ = nullptr;
    McpManager* mcp_ = nullptr;
    SubAgentManager* subagents_ = nullptr;
    ToolRegistry tools_;
    std::vector<SkillInfo> skills_;
    AgentTurnOptions options_;
};

}  // namespace spark
