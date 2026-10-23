#pragma once

#include "core/agent_types.h"
#include "core/config.h"
#include "platform/child_process.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace spark {

struct McpTool {
    std::string server_id;
    std::string name;
    std::string description;
    nlohmann::json input_schema = nlohmann::json::object();
    std::string exposed_name;
};

class McpClient {
public:
    McpClient(McpServerConfig config, bool allow_network);
    ~McpClient();

    McpClient(const McpClient&) = delete;
    McpClient& operator=(const McpClient&) = delete;

    Result<void> EnsureStarted(CancellationToken* cancellation = nullptr);
    Result<std::vector<McpTool>> ListTools(CancellationToken* cancellation = nullptr);
    Result<std::string> CallTool(const std::string& name, const nlohmann::json& arguments,
                                 CancellationToken* cancellation = nullptr);
    void Stop();

    const McpServerConfig& Config() const { return config_; }
    bool IsRunning() const { return running_; }
    const std::string& LastError() const { return last_error_; }

private:
    Result<nlohmann::json> Request(const std::string& method, nlohmann::json params, int timeout_ms,
                                   CancellationToken* cancellation);
    Result<void> StartStdio(CancellationToken* cancellation);
    Result<nlohmann::json> HttpRequestJson(const nlohmann::json& payload, const std::string& session_id,
                                           std::string* response_session_id, int timeout_ms,
                                           CancellationToken* cancellation);
    // Resolves config headers plus env-sourced and bearer-token headers.
    std::map<std::string, std::string> EffectiveHeaders() const;
    bool ToolAllowed(const std::string& tool_name) const;
    int StartupTimeoutMs() const;
    int ToolTimeoutMs() const;
    bool WriteLine(const std::string& line, int timeout_ms, CancellationToken* cancellation, std::string* error);
    bool ReadLine(std::string& line, int timeout_ms, CancellationToken* cancellation, std::string* error);
    void DrainStderr();
    void Reset();

    McpServerConfig config_;
    bool allow_network_ = false;
    std::atomic<bool> running_{false};
    std::unique_ptr<ChildProcess> process_;
    CancellationToken stderr_stop_;
    std::string pending_output_;
    int next_id_ = 1;
    std::string last_error_;
    std::string http_session_id_;
    std::mutex mutex_;
    std::thread stderr_thread_;
};

class McpManager {
public:
    explicit McpManager(const AppConfig& config);
    ~McpManager();

    Result<std::vector<McpTool>> DiscoverEnabled(CancellationToken* cancellation = nullptr);
    Result<std::string> Execute(const std::string& exposed_name, const nlohmann::json& arguments,
                                CancellationToken* cancellation = nullptr);
    std::vector<ToolDefinition> ToolDefinitions(bool allow_network = true) const;
    bool RequiresNetwork(const std::string& exposed_name) const;
    bool AllowsNetwork() const { return allow_network_; }
    void StopAll();

private:
    std::vector<std::unique_ptr<McpClient>> clients_;
    std::vector<McpTool> tools_;
    bool allow_network_ = false;
};

std::string McpExposedToolName(const std::string& server_id, const std::string& tool_name);

}  // namespace spark
