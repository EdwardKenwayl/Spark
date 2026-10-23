#include "core/mcp.h"

#include "platform/http.h"
#include "platform/clock.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <thread>

namespace spark {
namespace {

std::string SanitizeToolPart(std::string value) {
    for (char& character : value) {
        if (!(std::isalnum(static_cast<unsigned char>(character)) || character == '_' || character == '-')) {
            character = '_';
        }
    }
    return value;
}

std::string LookupEnvironmentVariable(const std::string& name) {
    return EnvironmentVariable(name);
}

bool ContainsName(const std::vector<std::string>& names, const std::string& value) {
    return std::find(names.begin(), names.end(), value) != names.end();
}

std::string ExtractFirstText(const nlohmann::json& result) {
    if (!result.is_object() || !result.contains("content") || !result["content"].is_array()) {
        return result.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    std::string output;
    for (const auto& block : result["content"]) {
        if (block.is_object() && block.value("type", std::string{}) == "text") {
            if (!output.empty()) output += "\n";
            output += block.value("text", std::string{});
        }
    }
    return output.empty() ? result.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) : output;
}

bool ParseJsonRpcResponse(const nlohmann::json& message, int expected_id, nlohmann::json* result, std::string* error) {
    if (!message.is_object()) return false;
    if (!message.contains("id") || !message["id"].is_number_integer() || message["id"] != expected_id ||
        (!message.contains("error") && !message.contains("result"))) return false;
    if (message.contains("error")) {
        const auto& value = message["error"];
        if (error != nullptr) {
            *error = value.is_object()
                         ? value.value("message", value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace))
                         : value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        }
        return true;
    }
    if (result != nullptr && message.contains("result")) *result = message["result"];
    return true;
}

nlohmann::json ParseHttpBody(const HttpResponse& response, int expected_id) {
    const auto select = [expected_id](const nlohmann::json& value) -> nlohmann::json {
        const auto matches = [expected_id](const nlohmann::json& item) {
            return item.is_object() && item.contains("id") && item["id"].is_number_integer() &&
                   item["id"] == expected_id && (item.contains("result") || item.contains("error"));
        };
        if (matches(value)) return value;
        if (value.is_array()) for (const auto& item : value) if (matches(item)) return item;
        return nullptr;
    };
    if (response.body.empty()) return nullptr;
    try {
        return select(nlohmann::json::parse(response.body));
    } catch (const nlohmann::json::exception&) {
    }
    std::string data;
    size_t position = 0;
    while (position < response.body.size()) {
        const size_t end = response.body.find('\n', position);
        std::string line = response.body.substr(position, end == std::string::npos ? end : end - position);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        position = end == std::string::npos ? response.body.size() : end + 1;
        if (line.rfind("data:", 0) == 0) {
            size_t start = line.size() > 5 && line[5] == ' ' ? 6 : 5;
            if (!data.empty()) data += '\n';
            data += line.substr(start);
        }
        if ((line.empty() || position == response.body.size()) && !data.empty()) {
            const auto parsed = nlohmann::json::parse(data, nullptr, false);
            data.clear();
            const auto selected = select(parsed);
            if (!selected.is_null()) return selected;
        }
    }
    return nullptr;
}

}  // namespace

McpClient::McpClient(McpServerConfig config, bool allow_network)
    : config_(std::move(config)), allow_network_(allow_network) {}

McpClient::~McpClient() { Stop(); }

int McpClient::StartupTimeoutMs() const {
    const int seconds =
        config_.startup_timeout_seconds > 0 ? config_.startup_timeout_seconds : config_.timeout_seconds;
    return std::clamp(seconds, 1, 86400) * 1000;
}

int McpClient::ToolTimeoutMs() const {
    const int seconds = config_.tool_timeout_seconds > 0 ? config_.tool_timeout_seconds : config_.timeout_seconds;
    return std::clamp(seconds, 1, 86400) * 1000;
}

std::map<std::string, std::string> McpClient::EffectiveHeaders() const {
    std::map<std::string, std::string> headers = config_.headers;
    for (const auto& [name, variable] : config_.env_http_headers) {
        const std::string value = LookupEnvironmentVariable(variable);
        if (!value.empty()) headers[name] = value;
    }
    if (!config_.bearer_token_env_var.empty()) {
        const std::string token = LookupEnvironmentVariable(config_.bearer_token_env_var);
        bool has_authorization = false;
        for (const auto& entry : headers) {
            if (EqualsIgnoreCaseAscii(entry.first, "authorization")) {
                has_authorization = true;
                break;
            }
        }
        if (!token.empty() && !has_authorization) {
            headers["Authorization"] = token.rfind("Bearer ", 0) == 0 ? token : "Bearer " + token;
        }
    }
    return headers;
}

bool McpClient::ToolAllowed(const std::string& tool_name) const {
    if (ContainsName(config_.disabled_tools, tool_name)) return false;
    if (!config_.enabled_tools.empty()) return ContainsName(config_.enabled_tools, tool_name);
    return true;
}

Result<void> McpClient::EnsureStarted(CancellationToken* cancellation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return Result<void>::Success();
    if (!config_.enabled) return Result<void>::Failure("MCP server is disabled: " + config_.id);
    if (config_.transport == "stdio") {
        return StartStdio(cancellation);
    }
    if (!allow_network_) {
        return Result<void>::Failure("streamable_http MCP is disabled by permissions.allow_network");
    }
    http_session_id_.clear();
    running_ = true;
    const nlohmann::json params = {
        {"protocolVersion", "2025-06-18"},
        {"capabilities", nlohmann::json::object()},
        {"clientInfo", {{"name", "Spark"}, {"version", "0.1.0"}}},
    };
    const auto initialized = Request("initialize", params, StartupTimeoutMs(), cancellation);
    if (!initialized.ok) {
        running_ = false;
        return Result<void>::Failure(initialized.error);
    }
    const nlohmann::json notification = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
    std::string response_session;
    const auto ignored =
        HttpRequestJson(notification, http_session_id_, &response_session, StartupTimeoutMs(), cancellation);
    if (ignored.ok && !response_session.empty()) http_session_id_ = response_session;
    return Result<void>::Success();
}

Result<void> McpClient::StartStdio(CancellationToken* cancellation) {
    ProcessOptions options;
    options.executable = config_.command;
    options.arguments = config_.args;
    options.directory = config_.cwd.empty() ? GetExecutableDirectory() : AbsolutePath(config_.cwd);
    options.environment = config_.env;
    auto created = ChildProcess::Start(options);
    if (!created.ok) return Result<void>::Failure(last_error_ = created.error);
    process_ = std::move(created.value);
    pending_output_.clear();
    stderr_stop_.Reset();
    stderr_thread_ = std::thread([this] { DrainStderr(); });
    running_ = true;
    next_id_ = 1;
    const nlohmann::json initialize = {
        {"jsonrpc", "2.0"},
        {"id", next_id_++},
        {"method", "initialize"},
        {"params", {
            {"protocolVersion", "2025-06-18"},
            {"capabilities", nlohmann::json::object()},
            {"clientInfo", {{"name", "Spark"}, {"version", "0.1.0"}}},
        }},
    };
    const auto response = Request("", initialize, StartupTimeoutMs(), cancellation);
    if (!response.ok) {
        Reset();
        return Result<void>::Failure(response.error);
    }
    const nlohmann::json initialized = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
    std::string error;
    if (!WriteLine(initialized.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace),
                   StartupTimeoutMs(), cancellation, &error)) {
        Reset();
        return Result<void>::Failure(error);
    }
    return Result<void>::Success();
}

Result<nlohmann::json> McpClient::Request(const std::string& method, nlohmann::json params,
                                          int timeout_ms, CancellationToken* cancellation) {
    const nlohmann::json payload = method.empty()
                                       ? std::move(params)
                                       : nlohmann::json{{"jsonrpc", "2.0"},
                                                        {"id", next_id_++},
                                                        {"method", method},
                                                        {"params", std::move(params)}};
    if (config_.transport == "streamable_http") {
        std::string session_id;
        const auto response = HttpRequestJson(payload, http_session_id_, &session_id, timeout_ms, cancellation);
        if (response.ok && !session_id.empty()) http_session_id_ = session_id;
        return response;
    }
    std::string error;
    const auto failure = [this](const std::string& message) {
        Reset();
        return Result<nlohmann::json>::Failure(message);
    };
    const uint64_t deadline = MonotonicMillis() + static_cast<uint64_t>(timeout_ms);
    if (!WriteLine(payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace),
                   timeout_ms, cancellation, &error)) {
        return failure(error);
    }
    const int expected_id = payload.value("id", next_id_ - 1);
    nlohmann::json response;
    std::string line;
    for (;;) {
        if (cancellation != nullptr && cancellation->IsCancelled()) return failure("cancelled");
        const uint64_t now = MonotonicMillis();
        if (now >= deadline) return failure("MCP request timed out");
        if (!ReadLine(line, static_cast<int>(deadline - now), cancellation, &error))
            return failure(error);
        try {
            const nlohmann::json message = nlohmann::json::parse(line);
            if (!message.contains("id")) continue;
            std::string rpc_error;
            if (!ParseJsonRpcResponse(message, expected_id, &response, &rpc_error)) continue;
            if (!rpc_error.empty()) return Result<nlohmann::json>::Failure("MCP error: " + rpc_error);
            return Result<nlohmann::json>::Success(std::move(response));
        } catch (const nlohmann::json::exception&) {
            // Ignore non-JSON diagnostic output from a stdio server.
        }
    }
}

Result<nlohmann::json> McpClient::HttpRequestJson(const nlohmann::json& payload, const std::string& session_id,
                                                  std::string* response_session_id, int timeout_ms,
                                                  CancellationToken* cancellation) {
    HttpRequest request;
    request.url = config_.url;
    request.headers = EffectiveHeaders();
    request.headers["Content-Type"] = "application/json";
    request.headers["Accept"] = "application/json, text/event-stream";
    if (!session_id.empty()) request.headers["Mcp-Session-Id"] = session_id;
    request.body = payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    request.timeout_seconds = std::max(1, timeout_ms / 1000);
    request.header_timeout_seconds = request.timeout_seconds;
    // JSON-RPC tool calls can have external side effects. Replaying them after a
    // dropped response may execute the same action twice, so they are attempted once.
    request.max_attempts = payload.value("method", std::string{}) == "tools/call" ? 1 : 3;
    request.stream = false;
    const auto response = HttpRequestStreaming(request, {}, cancellation);
    if (!response.ok) return Result<nlohmann::json>::Failure(response.error);
    if (response.value.status < 200 || response.value.status >= 300) {
        return Result<nlohmann::json>::Failure("MCP HTTP " + std::to_string(response.value.status) + ": " + response.value.body);
    }
    if (response_session_id != nullptr) {
        for (const auto& header : response.value.headers) {
            if (EqualsIgnoreCaseAscii(header.first, "mcp-session-id")) {
                *response_session_id = header.second;
                break;
            }
        }
    }
    try {
        if (!payload.contains("id")) return Result<nlohmann::json>::Success(nlohmann::json::object());
        nlohmann::json message = ParseHttpBody(response.value, payload["id"].get<int>());
        if (message.is_null()) return Result<nlohmann::json>::Failure("MCP HTTP response has no matching request id");
        if (message.contains("error")) {
            return Result<nlohmann::json>::Failure(
                "MCP error: " + message["error"].dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        }
        return Result<nlohmann::json>::Success(message.value("result", nlohmann::json::object()));
    } catch (const nlohmann::json::exception& exception) {
        return Result<nlohmann::json>::Failure(std::string("invalid MCP HTTP JSON: ") + exception.what());
    }
}

bool McpClient::WriteLine(const std::string& line, int timeout_ms, CancellationToken* cancellation,
                          std::string* error) {
    if (!process_) {
        if (error != nullptr) *error = "MCP stdin is not available";
        return false;
    }
    const std::string message = line + "\n";
    const auto written = process_->Write(message, timeout_ms, cancellation);
    if (!written.ok) {
        if (error) *error = "could not write MCP request: " + written.error;
        return false;
    }
    return true;
}

bool McpClient::ReadLine(std::string& line, int timeout_ms, CancellationToken* cancellation, std::string* error) {
    line.clear();
    const uint64_t deadline = MonotonicMillis() + static_cast<uint64_t>(timeout_ms);
    for (;;) {
        if (cancellation != nullptr && cancellation->IsCancelled()) {
            if (error != nullptr) *error = "cancelled";
            return false;
        }
        const size_t newline = pending_output_.find('\n');
        if (newline != std::string::npos) {
            line = pending_output_.substr(0, newline);
            pending_output_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        if (MonotonicMillis() >= deadline) {
            if (error != nullptr) *error = "MCP request timed out";
            return false;
        }
        char buffer[4096];
        const auto read = process_->Read(ProcessStream::Output, buffer, sizeof(buffer), 25, cancellation);
        if (read.state == ProcessIoState::Timeout) {
            if (!process_->Exited()) continue;
            if (error) *error = "MCP server exited";
            return false;
        }
        if (read.state != ProcessIoState::Data) {
            if (error) *error = read.state == ProcessIoState::Cancelled ? "cancelled" :
                                read.state == ProcessIoState::End ? "MCP server exited" : read.error;
            return false;
        }
        pending_output_.append(buffer, read.size);
        if (pending_output_.size() > 16 * 1024 * 1024) {
            if (error != nullptr) *error = "MCP message exceeded 16 MiB limit";
            return false;
        }
    }
}

void McpClient::DrainStderr() {
    char buffer[4096];
    while (!stderr_stop_.IsCancelled()) {
        const auto read = process_->Read(ProcessStream::Error, buffer, sizeof(buffer), 25, &stderr_stop_);
        if (read.state != ProcessIoState::Data && read.state != ProcessIoState::Timeout) break;
        // Stderr is diagnostic only and never becomes model-visible tool output.
    }
}

Result<std::vector<McpTool>> McpClient::ListTools(CancellationToken* cancellation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return Result<std::vector<McpTool>>::Failure("MCP server is not running: " + config_.id);
    const int timeout_ms = ToolTimeoutMs();
    nlohmann::json params = nlohmann::json::object();
    Result<nlohmann::json> response = Request("tools/list", std::move(params), timeout_ms, cancellation);
    if (!response.ok) return Result<std::vector<McpTool>>::Failure(response.error);
    std::vector<McpTool> output;
    if (!response.value.contains("tools") || !response.value["tools"].is_array()) {
        return Result<std::vector<McpTool>>::Failure("MCP tools/list response has no tools array");
    }
    for (const auto& item : response.value["tools"]) {
        McpTool tool;
        tool.server_id = config_.id;
        tool.name = item.value("name", std::string{});
        tool.description = item.value("description", std::string{});
        tool.input_schema = item.value("inputSchema", nlohmann::json::object());
        tool.exposed_name = McpExposedToolName(config_.id, tool.name);
        if (tool.name.empty() || !ToolAllowed(tool.name)) continue;
        output.push_back(std::move(tool));
    }
    return Result<std::vector<McpTool>>::Success(std::move(output));
}

Result<std::string> McpClient::CallTool(const std::string& name, const nlohmann::json& arguments,
                                        CancellationToken* cancellation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return Result<std::string>::Failure("MCP server is not running: " + config_.id);
    const int timeout_ms = ToolTimeoutMs();
    const nlohmann::json params = {{"name", name}, {"arguments", arguments}};
    Result<nlohmann::json> response = Request("tools/call", params, timeout_ms, cancellation);
    if (!response.ok) return Result<std::string>::Failure(response.error);
    if (response.value.value("isError", false)) return Result<std::string>::Failure(ExtractFirstText(response.value));
    return Result<std::string>::Success(ExtractFirstText(response.value));
}

void McpClient::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    Reset();
}

void McpClient::Reset() {
    stderr_stop_.Cancel();
    if (stderr_thread_.joinable()) stderr_thread_.join();
    process_.reset();
    pending_output_.clear();
    running_ = false;
}

McpManager::McpManager(const AppConfig& config) : allow_network_(config.permissions.allow_network) {
    for (const McpServerConfig& server : config.mcp_servers) {
        if (!server.enabled) continue;
        McpServerConfig resolved = server;
        if (resolved.transport == "stdio" && !resolved.command.empty()) {
            if (resolved.command.find_first_of("/\\") != std::string::npos)
                resolved.command = AbsolutePath(resolved.command, ParentPath(config.config_path));
        }
        if (resolved.transport == "stdio" && !resolved.cwd.empty()) {
            resolved.cwd = AbsolutePath(resolved.cwd, ParentPath(config.config_path));
        }
        clients_.push_back(std::make_unique<McpClient>(std::move(resolved), config.permissions.allow_network));
    }
}

McpManager::~McpManager() { StopAll(); }

Result<std::vector<McpTool>> McpManager::DiscoverEnabled(CancellationToken* cancellation) {
    tools_.clear();
    std::string errors;
    std::string required_errors;
    for (auto& client : clients_) {
        if (!allow_network_ && client->Config().transport == "streamable_http" && !client->Config().required) {
            continue;
        }
        const auto started = client->EnsureStarted(cancellation);
        if (!started.ok) {
            const std::string line = client->Config().id + ": " + started.error;
            if (!errors.empty()) errors += "\n";
            errors += line;
            if (client->Config().required) {
                if (!required_errors.empty()) required_errors += "\n";
                required_errors += line;
            }
            continue;
        }
        const auto discovered = client->ListTools(cancellation);
        if (!discovered.ok) {
            const std::string line = client->Config().id + ": " + discovered.error;
            if (!errors.empty()) errors += "\n";
            errors += line;
            if (client->Config().required) {
                if (!required_errors.empty()) required_errors += "\n";
                required_errors += line;
            }
            continue;
        }
        for (const McpTool& tool : discovered.value) tools_.push_back(tool);
    }
    if (!required_errors.empty()) {
        return Result<std::vector<McpTool>>::Failure("required MCP server failed:\n" + required_errors);
    }
    if (tools_.empty() && !errors.empty()) {
        return Result<std::vector<McpTool>>::Failure(errors);
    }
    // Stable ordering keeps the tool list identical between runs so the cached
    // prompt prefix is not invalidated by configuration iteration order.
    std::sort(tools_.begin(), tools_.end(), [](const McpTool& left, const McpTool& right) {
        return left.exposed_name < right.exposed_name;
    });
    return Result<std::vector<McpTool>>::Success(tools_);
}

Result<std::string> McpManager::Execute(const std::string& exposed_name, const nlohmann::json& arguments,
                                        CancellationToken* cancellation) {
    const auto iterator = std::find_if(tools_.begin(), tools_.end(), [&](const McpTool& tool) {
        return tool.exposed_name == exposed_name;
    });
    if (iterator == tools_.end()) return Result<std::string>::Failure("unknown MCP tool: " + exposed_name);
    for (auto& client : clients_) {
        if (client->Config().id == iterator->server_id) return client->CallTool(iterator->name, arguments, cancellation);
    }
    return Result<std::string>::Failure("MCP server is unavailable: " + iterator->server_id);
}

bool McpManager::RequiresNetwork(const std::string& exposed_name) const {
    const auto tool = std::find_if(tools_.begin(), tools_.end(), [&](const McpTool& entry) {
        return entry.exposed_name == exposed_name;
    });
    if (tool == tools_.end()) return false;
    return std::any_of(clients_.begin(), clients_.end(), [&](const auto& client) {
        return client->Config().id == tool->server_id && client->Config().transport == "streamable_http";
    });
}

std::vector<ToolDefinition> McpManager::ToolDefinitions(bool allow_network) const {
    std::vector<std::string> blocked_servers;
    if (!allow_network) {
        for (const auto& client : clients_) {
            if (client->Config().transport == "streamable_http") blocked_servers.push_back(client->Config().id);
        }
    }
    std::vector<ToolDefinition> definitions;
    for (const McpTool& tool : tools_) {
        if (std::find(blocked_servers.begin(), blocked_servers.end(), tool.server_id) != blocked_servers.end()) continue;
        definitions.push_back({tool.exposed_name, tool.description, tool.input_schema});
    }
    return definitions;
}

void McpManager::StopAll() {
    for (auto& client : clients_) client->Stop();
}

std::string McpExposedToolName(const std::string& server_id, const std::string& tool_name) {
    return "mcp__" + SanitizeToolPart(server_id) + "__" + SanitizeToolPart(tool_name);
}

}  // namespace spark
