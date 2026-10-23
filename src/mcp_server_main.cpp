#include "core/config.h"
#include "core/external_agents.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include "platform/filesystem.h"
#include <iostream>
#include <memory>
#include <string>

namespace {

using spark::AppConfig;
using spark::ExternalAgentInfo;
using spark::ExternalAgentManager;

enum class RequestLineState { Complete, End, TooLarge, Failed };

RequestLineState ReadRequestLine(std::istream& input, std::string& line) {
    constexpr size_t kMaxRequestBytes = 16u * 1024u * 1024u;
    line.clear();
    char character = '\0';
    while (input.get(character)) {
        if (character == '\n') return RequestLineState::Complete;
        if (line.size() == kMaxRequestBytes) return RequestLineState::TooLarge;
        line.push_back(character);
    }
    if (input.bad() || !input.eof()) return RequestLineState::Failed;
    return line.empty() ? RequestLineState::End : RequestLineState::Complete;
}

std::string ConfigArgument(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--config" && index + 1 < argc) return argv[index + 1];
        constexpr const char* prefix = "--config=";
        if (argument.rfind(prefix, 0) == 0) return argument.substr(std::char_traits<char>::length(prefix));
    }
    const std::string from_env = spark::EnvironmentVariable("SPARK_CONFIG");
    if (!from_env.empty()) return from_env;
    return spark::JoinPath(spark::GetExecutableDirectory(), "config.json");
}

nlohmann::json ToolResult(const std::string& text, bool is_error = false) {
    return {
        {"content", nlohmann::json::array({{{"type", "text"}, {"text", text}}})},
        {"isError", is_error},
    };
}

nlohmann::json RuntimeTools() {
    return nlohmann::json::array({
        {{"name", "list_models"},
         {"description", "List providers, models, and supported reasoning efforts configured in Spark."},
         {"inputSchema", {{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}}}},
        {{"name", "create_subagent"},
         {"description",
          "Create a new Spark session and run a task asynchronously with a configured model. If workspace "
          "exactly matches a registered Spark project path, the conversation is placed under that project; "
          "otherwise it is placed under recent chats."},
         {"inputSchema",
          {{"type", "object"},
           {"properties",
            {{"task", {{"type", "string"}, {"description", "Task or question for the child agent."}}},
             {"name", {{"type", "string"}, {"description", "Optional display name."}}},
             {"provider", {{"type", "string"}, {"description", "Provider id from list_models."}}},
             {"model", {{"type", "string"}, {"description", "Model id from list_models."}}},
             {"reasoning_effort", {{"type", "string"}, {"description", "Reasoning effort supported by the model."}}},
             {"workspace",
              {{"type", "string"},
               {"description",
                "Optional working directory. Use the exact registered project path to create the child conversation "
                "under that project."}}},
             {"permission_mode",
              {{"type", "string"},
               {"enum", nlohmann::json::array({"read_only", "ask", "approve_for_me", "full_access"})},
               {"description", "Tool permission policy. read_only is the default and rejects writes, commands and MCP tools."}}}}},
           {"required", nlohmann::json::array({"task"})},
           {"additionalProperties", false}}}},
        {{"name", "send_subagent_message"},
         {"description", "Continue a completed Spark child session with another message."},
         {"inputSchema",
          {{"type", "object"},
           {"properties",
            {{"id", {{"type", "string"}}},
             {"message", {{"type", "string"}}},
             {"provider", {{"type", "string"}}},
             {"model", {{"type", "string"}}},
             {"reasoning_effort", {{"type", "string"}}}}},
           {"required", nlohmann::json::array({"id", "message"})},
           {"additionalProperties", false}}}},
        {{"name", "wait_subagent"},
         {"description",
          "Wait up to the timeout for a child agent to finish or request approval. Check output_complete; when "
          "awaiting_approval is true, review the approval details and call approve_subagent_action, then wait again. "
          "If output_complete is false and no approval is pending, the output is partial; call wait_subagent again."},
         {"inputSchema",
          {{"type", "object"},
           {"properties", {{"id", {{"type", "string"}}}, {"timeout_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 600000}}}}},
           {"required", nlohmann::json::array({"id"})},
           {"additionalProperties", false}}}},
        {{"name", "approve_subagent_action"},
         {"description", "Approve or reject the pending tool action for a child agent using ask permission mode."},
         {"inputSchema",
          {{"type", "object"},
           {"properties", {{"id", {{"type", "string"}}}, {"approved", {{"type", "boolean"}}}}},
           {"required", nlohmann::json::array({"id", "approved"})},
           {"additionalProperties", false}}}},
        {{"name", "get_subagent"},
         {"description", "Read the current state and bounded output of one child agent."},
         {"inputSchema", {{"type", "object"}, {"properties", {{"id", {{"type", "string"}}}}}, {"required", nlohmann::json::array({"id"})}}}},
        {{"name", "list_subagents"},
         {"description", "List child agents created by this MCP server process."},
         {"inputSchema", {{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}}}},
        {{"name", "cancel_subagent"},
         {"description", "Request cancellation of a running child agent."},
         {"inputSchema", {{"type", "object"}, {"properties", {{"id", {{"type", "string"}}}}}, {"required", nlohmann::json::array({"id"})}}}},
    });
}

std::string StringArgument(const nlohmann::json& arguments, const char* name, const std::string& fallback = {}) {
    return arguments.is_object() ? arguments.value(name, fallback) : fallback;
}

void Respond(const nlohmann::json& id, nlohmann::json result) {
    nlohmann::json response = {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
    std::cout << response.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << "\n" << std::flush;
}

void RespondError(const nlohmann::json& id, int code, const std::string& message) {
    nlohmann::json response = {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
    std::cout << response.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << "\n" << std::flush;
}

nlohmann::json CallTool(ExternalAgentManager& manager, const std::string& name,
                        const nlohmann::json& arguments) {
    if (name == "list_models") return ToolResult(manager.ModelsJson());
    if (name == "create_subagent") {
        const auto result = manager.Create(StringArgument(arguments, "task"), StringArgument(arguments, "name"),
                                           StringArgument(arguments, "provider"), StringArgument(arguments, "model"),
                                           StringArgument(arguments, "reasoning_effort"),
                                           StringArgument(arguments, "workspace"),
                                           StringArgument(arguments, "permission_mode", "read_only"));
        return result.ok ? ToolResult(spark::ExternalAgentInfoJson(result.value))
                         : ToolResult("create_subagent failed: " + result.error, true);
    }
    if (name == "send_subagent_message") {
        const auto result = manager.Send(StringArgument(arguments, "id"), StringArgument(arguments, "message"),
                                         StringArgument(arguments, "provider"), StringArgument(arguments, "model"),
                                         StringArgument(arguments, "reasoning_effort"));
        return result.ok ? ToolResult("message accepted") : ToolResult("send_subagent_message failed: " + result.error, true);
    }
    if (name == "wait_subagent") {
        int timeout_ms = 120000;
        if (arguments.is_object() && arguments.contains("timeout_ms") && arguments["timeout_ms"].is_number_integer()) {
            timeout_ms = std::max(0, std::min(600000, arguments["timeout_ms"].get<int>()));
        }
        const auto result = manager.Wait(StringArgument(arguments, "id"), timeout_ms);
        return result.ok ? ToolResult(spark::ExternalAgentInfoJson(result.value))
                         : ToolResult("wait_subagent failed: " + result.error, true);
    }
    if (name == "approve_subagent_action") {
        const bool approved = arguments.is_object() && arguments.value("approved", false);
        const std::string id = StringArgument(arguments, "id");
        const auto resolved = manager.Approve(id, approved);
        if (!resolved.ok) return ToolResult("approve_subagent_action failed: " + resolved.error, true);
        const auto result = manager.Get(id);
        return result.ok ? ToolResult(spark::ExternalAgentInfoJson(result.value))
                         : ToolResult("approve_subagent_action failed: " + result.error, true);
    }
    if (name == "get_subagent") {
        const auto result = manager.Get(StringArgument(arguments, "id"));
        return result.ok ? ToolResult(spark::ExternalAgentInfoJson(result.value))
                         : ToolResult("get_subagent failed: " + result.error, true);
    }
    if (name == "list_subagents") {
        nlohmann::json agents = nlohmann::json::array();
        for (const ExternalAgentInfo& info : manager.List()) {
            agents.push_back({{"id", info.id},
                              {"name", info.name},
                              {"state", info.state},
                              {"provider", info.provider},
                              {"model", info.model},
                              {"reasoning_effort", info.reasoning_effort},
                              {"permission_mode", info.permission_mode},
                              {"scope", info.scope},
                              {"awaiting_approval", info.awaiting_approval},
                              {"approval_kind", info.approval_kind},
                              {"approval_title", info.approval_title},
                              {"approval_detail", info.approval_detail},
                              {"output_complete", info.output_complete},
                              {"workspace", info.workspace},
                              {"session_path", info.session_path},
                              {"started_at", info.started_at},
                              {"finished_at", info.finished_at}});
        }
        return ToolResult(nlohmann::json({{"agents", std::move(agents)}}).dump(2));
    }
    if (name == "cancel_subagent") {
        const auto result = manager.Cancel(StringArgument(arguments, "id"));
        return result.ok ? ToolResult("cancellation requested") : ToolResult("cancel_subagent failed: " + result.error, true);
    }
    return ToolResult("unknown tool: " + name, true);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string config_path = ConfigArgument(argc, argv);
    const auto loaded = spark::LoadConfig(config_path, true);
    if (!loaded.ok) {
        std::cerr << "SparkMCP: could not load config: " << loaded.error << "\n";
        return 2;
    }
    AppConfig config = loaded.value;
    config.config_path = config_path;
    config.workspace.path = spark::AbsolutePath(
        spark::ResolveConfigPath(config.workspace.path, config.config_path), spark::ParentPath(config.config_path));
    config.storage.data_dir = spark::AbsolutePath(
        spark::ResolveConfigPath(config.storage.data_dir, config.config_path), spark::ParentPath(config.config_path));

    ExternalAgentManager manager(std::move(config), config_path);
    const auto initialized = manager.Initialize();
    if (!initialized.ok) std::cerr << "SparkMCP: " << initialized.error << "\n";

    std::string line;
    while (std::cout) {
        const RequestLineState state = ReadRequestLine(std::cin, line);
        if (state == RequestLineState::End) break;
        if (state == RequestLineState::Failed) {
            std::cerr << "SparkMCP: could not read stdin\n";
            return 2;
        }
        if (state == RequestLineState::TooLarge) {
            RespondError(nullptr, -32700, "request exceeded 16 MiB limit");
            return 2;
        }
        if (line.empty()) continue;
        nlohmann::json request;
        try {
            request = nlohmann::json::parse(line);
        } catch (const nlohmann::json::exception& error) {
            RespondError(nullptr, -32700, std::string("parse error: ") + error.what());
            continue;
        }
        const std::string method = request.value("method", std::string{});
        if (method.rfind("notifications/", 0) == 0) continue;
        const nlohmann::json id = request.contains("id") ? request["id"] : nlohmann::json(nullptr);
        if (method == "initialize") {
            const std::string protocol = request.value("params", nlohmann::json::object())
                                             .value("protocolVersion", std::string("2025-03-26"));
            Respond(id, {{"protocolVersion", protocol},
                         {"capabilities", {{"tools", {{"listChanged", false}}}}},
                         {"serverInfo", {{"name", "spark-mcp"}, {"version", "0.1.0"}}},
                         {"instructions",
                          "Create and coordinate independent Spark sessions. Call list_models first, then "
                          "create_subagent with an explicit provider/model. Use wait_subagent to collect results. "
                          "If it returns awaiting_approval, inspect the action and call approve_subagent_action with "
                          "approved=true or false. If output_complete is false, wait again for the final result."}});
        } else if (method == "ping") {
            Respond(id, nlohmann::json::object());
        } else if (method == "tools/list") {
            Respond(id, {{"tools", RuntimeTools()}});
        } else if (method == "tools/call") {
            const nlohmann::json params = request.value("params", nlohmann::json::object());
            Respond(id, CallTool(manager, params.value("name", std::string{}),
                                 params.value("arguments", nlohmann::json::object())));
        } else if (method == "shutdown") {
            Respond(id, nullptr);
            break;
        } else {
            RespondError(id, -32601, "method not found: " + method);
        }
    }
    if (!std::cout) {
        std::cerr << "SparkMCP: could not write stdout\n";
        return 2;
    }
    return 0;
}
