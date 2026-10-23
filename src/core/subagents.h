#pragma once

#include "core/agent_types.h"
#include "core/common.h"
#include "core/config.h"
#include "core/session.h"
#include "core/tools.h"

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace spark {

class McpManager;

enum class SubAgentState {
    Running,
    Completed,
    Failed,
    Cancelled,
};

struct SubAgentInfo {
    std::string id;
    std::string parent_session_key;
    std::string name;
    std::string task;
    std::string session_path;
    std::string output;
    std::string error;
    SubAgentState state = SubAgentState::Running;
    uint64_t started_at = 0;
    uint64_t finished_at = 0;
    std::string provider;
    std::string model;
    std::string reasoning_effort;
    // MonotonicMillis() timestamp; restored values are historical only.
    uint64_t started_tick_ms = 0;
    uint64_t elapsed_ms = 0;
};

// These UI payloads mirror AgentCallbacks. Tool arguments and result output are
// the bounded display previews supplied by Agent, not full execution payloads.
struct ToolResult {
    bool ok = false;
    std::string output;
};

struct TokenUsage {
    int input_tokens = 0;
    int output_tokens = 0;
    int cached_input_tokens = 0;
};

struct SubAgentCallbacks {
    std::function<void(const SubAgentInfo&)> on_created;
    std::function<void(const SubAgentInfo&, AgentPhase, const std::string&)> on_phase;
    std::function<void(const SubAgentInfo&, const std::string&)> on_text_delta;
    std::function<void(const SubAgentInfo&, bool, const std::string&)> on_finished;
    std::function<void(const SubAgentInfo&, const std::string&)> on_error;
    std::function<void(const SubAgentInfo&, const std::string&)> on_reasoning_delta;
    std::function<void(const SubAgentInfo&, const ToolCall&)> on_tool_started;
    std::function<void(const SubAgentInfo&, const ToolCall&, const ToolResult&)> on_tool_finished;
    std::function<void(const SubAgentInfo&, const TokenUsage&)> on_usage;
};

// Runs bounded, non-recursive child agents. A child owns a separate journal and
// worker. Model selection can differ; workspace, MCP and permissions are inherited.
class SubAgentManager {
public:
    explicit SubAgentManager(SubAgentCallbacks callbacks = {});
    ~SubAgentManager();

    SubAgentManager(const SubAgentManager&) = delete;
    SubAgentManager& operator=(const SubAgentManager&) = delete;

    Result<std::string> Spawn(const AppConfig& config, McpManager* mcp, const std::string& data_directory,
                              const std::string& parent_session_key, const std::string& workspace,
                              const std::string& name, const std::string& task, const ApprovalCallback& approval,
                              CancellationToken* parent_cancellation, const ActiveSelection& selection = {});
    Result<std::string> Wait(const std::string& id, CancellationToken* parent_cancellation);
    std::vector<SubAgentInfo> List(const std::string& parent_session_key) const;
    // Reads at most 256 KiB / 64 recent metadata records; never opens journals.
    // Restored ticks are historical only: elapsed_ms is not recomputed on load.
    static std::vector<SubAgentInfo> LoadHistory(const std::string& data_directory,
                                               const std::string& parent_session_key);
    bool HasRunning() const;
    void CancelAll();

private:
    struct Run {
        SubAgentInfo info;
        SessionJournal journal;
        CancellationToken cancellation;
        std::condition_variable condition;
        std::mutex mutex;
        std::mutex worker_mutex;
        std::thread worker;
        bool done = false;
    };

    SubAgentInfo Snapshot(const std::shared_ptr<Run>& run) const;
    void EmitCreated(const std::shared_ptr<Run>& run);
    void EmitPhase(const std::shared_ptr<Run>& run, AgentPhase phase, const std::string& detail);
    void EmitText(const std::shared_ptr<Run>& run, const std::string& delta);
    void EmitFinished(const std::shared_ptr<Run>& run, bool success, const std::string& reason);
    void EmitError(const std::shared_ptr<Run>& run, const std::string& error);

    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Run>> runs_;
    SubAgentCallbacks callbacks_;
};

std::string SubAgentStateToString(SubAgentState state);

}  // namespace spark
