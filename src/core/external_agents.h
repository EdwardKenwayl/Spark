#pragma once

#include "core/common.h"
#include "core/config.h"
#include "core/mcp.h"
#include "core/session.h"

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace spark {

struct ExternalAgentInfo {
    std::string id;
    std::string name;
    std::string state;
    std::string workspace;
    std::string session_path;
    std::string provider;
    std::string model;
    std::string reasoning_effort;
    std::string permission_mode;
    std::string scope;
    std::string output;
    std::string reasoning;
    std::string phase;
    std::string active_tool_name;
    std::string active_tool_arguments;
    std::string error;
    bool awaiting_approval = false;
    std::string approval_kind;
    std::string approval_title;
    std::string approval_detail;
    bool output_complete = false;
    uint64_t started_at = 0;
    uint64_t finished_at = 0;
    uint64_t approval_id = 0;
};

// Owns independent Spark sessions that can be driven by an external MCP client.
// Each session has its own journal, worker, cancellation token, provider/model selection,
// and permission mode. It never shares mutable conversation state with another session.
class ExternalAgentManager {
public:
    ExternalAgentManager(AppConfig config, std::string config_path);
    ~ExternalAgentManager();

    ExternalAgentManager(const ExternalAgentManager&) = delete;
    ExternalAgentManager& operator=(const ExternalAgentManager&) = delete;

    Result<void> Initialize();
    std::string ModelsJson() const;
    Result<ExternalAgentInfo> Create(const std::string& task, const std::string& name,
                                     const std::string& provider, const std::string& model,
                                     const std::string& reasoning_effort, const std::string& workspace,
                                     const std::string& permission_mode);
    Result<void> Send(const std::string& id, const std::string& message, const std::string& provider,
                      const std::string& model, const std::string& reasoning_effort);
    Result<ExternalAgentInfo> Wait(const std::string& id, int timeout_ms);
    Result<ExternalAgentInfo> Get(const std::string& id) const;
    std::vector<ExternalAgentInfo> List() const;
    Result<void> Approve(const std::string& id, bool approved);
    Result<void> Cancel(const std::string& id);
    void WriteRegistry() const;

private:
    struct Run {
        ExternalAgentInfo info;
        SessionJournal journal;
        CancellationToken cancellation;
        std::thread worker;
        std::unique_ptr<McpManager> permission_mcp;
        std::mutex mutex;
        std::mutex approval_mutex;
        std::condition_variable condition;
        bool running = false;
        bool accepting = true;
        bool approval_decided = false;
        bool approval_granted = false;
        ApprovalRequest pending_approval;
    };

    Result<ActiveSelection> ResolveSelection(const std::string& provider, const std::string& model,
                                             const std::string& reasoning_effort) const;
    Result<void> StartTask(const std::shared_ptr<Run>& run, const std::string& task);
    void ProcessControlRequests();
    ExternalAgentInfo Snapshot(const std::shared_ptr<Run>& run) const;
    std::shared_ptr<Run> Find(const std::string& id) const;

    AppConfig config_;
    std::string config_path_;
    std::unique_ptr<McpManager> mcp_;
    mutable std::mutex registry_write_mutex_;
    std::thread registry_writer_;
    std::atomic<bool> registry_writer_stop_{false};
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Run>> runs_;
};

std::string ExternalAgentInfoJson(const ExternalAgentInfo& info);
// The MCP registry has an independent one-second heartbeat, even during long model calls.
bool IsExternalAgentRegistryFresh(const nlohmann::json& document, uint64_t now);

// Returns "project" when the workspace exactly matches a path registered in the
// Spark project sidebar, otherwise "recent".
std::string ResolveExternalAgentScope(const AppConfig& config, const std::string& config_path,
                                     const std::string& workspace);
std::string ResolveExternalAgentTitle(const std::string& requested_name, const std::string& task);

}  // namespace spark
