#pragma once

#include "core/common.h"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

namespace spark {

struct ModelCapabilities {
    bool streaming = true;
    bool tool_calls = true;
    bool images = true;
    bool reasoning = true;
    std::vector<std::string> reasoning_efforts;
    std::string default_reasoning_effort;
    int context_window = 0;
    int auto_compact_threshold_percent = 0;
};

struct ModelConfig {
    std::string id;
    std::string display_name;
    ModelCapabilities capabilities;
};

struct ProviderConfig {
    std::string id;
    std::string label;
    std::string protocol = "chat_completions";
    std::string base_url;
    std::string api_key;
    std::string api_key_expression;
    std::map<std::string, std::string> headers;
    std::vector<ModelConfig> models;
    std::string test_model;
};

struct ActiveSelection {
    std::string provider;
    std::string model;
    std::string reasoning_effort;
};

struct AgentLimitsConfig {
    int max_iterations = 100;
    int max_parallel_read_tools = 4;
    int request_timeout_seconds = 180;
    int response_header_timeout_seconds = 600;
    // A command may run for a long time while the user is watching it. The
    // tool can still be cancelled explicitly, and individual calls may
    // override this value.
    int tool_timeout_seconds = 600;
    int max_tool_output_bytes = 16384;
    int max_command_output_bytes = 65536;
};

struct ContextConfig {
    bool auto_compact = true;
    int fallback_context_window = 272000;
    int auto_compact_threshold_percent = 90;
    int reserve_output_tokens = 16384;
    int summary_max_tokens = 8000;
    bool manual_compact_command = true;
};

struct PermissionConfig {
    std::string default_mode = "ask";
    bool workspace_only = true;
    bool allow_network = false;
};

struct SkillConfig {
    bool enabled = true;
    std::vector<std::string> roots;
    // The workspace and per-user skill roots are scanned in addition to roots.
    bool include_workspace_root = true;
    bool include_user_root = true;
    int max_skills = 64;
};

struct WorkspaceConfig {
    std::string path;
    bool allow_outside_reads = false;
    bool allow_outside_writes = false;
    std::string file_write_approval = "ask";
    std::string command_approval = "ask";
    std::string mcp_tool_approval = "ask";
};

struct McpServerConfig {
    std::string id;
    bool enabled = false;
    // A required server turns a startup failure into a visible session error
    // instead of being silently skipped.
    bool required = false;
    std::string transport = "stdio";
    std::string command;
    std::vector<std::string> args;
    // Working directory for stdio servers. Empty keeps the command directory.
    std::string cwd;
    std::map<std::string, std::string> env;
    std::string url;
    std::map<std::string, std::string> headers;
    // Header name -> environment variable name, resolved when the client starts.
    std::map<std::string, std::string> env_http_headers;
    // Environment variable holding a bearer token for the Authorization header.
    std::string bearer_token_env_var;
    std::vector<std::string> enabled_tools;
    std::vector<std::string> disabled_tools;
    int timeout_seconds = 60;
    // Per-phase overrides. Zero falls back to timeout_seconds.
    int startup_timeout_seconds = 0;
    int tool_timeout_seconds = 0;
};

struct StorageConfig {
    std::string data_dir = "./data";
    bool redact_secrets = true;
};

struct UiConfig {
    std::string language = "system";
    std::string theme = "dark";
#ifdef _WIN32
    std::string shell = "cmd.exe";
#else
    std::string shell = "/bin/sh";
#endif
    std::string send_key = "enter";
};

struct AppConfig {
    int schema_version = 2;
    ActiveSelection active;
    std::vector<ProviderConfig> providers;
    AgentLimitsConfig agent;
    ContextConfig context;
    PermissionConfig permissions;
    WorkspaceConfig workspace;
    SkillConfig skills;
    std::vector<McpServerConfig> mcp_servers;
    StorageConfig storage;
    UiConfig ui;
    std::string config_path;
    nlohmann::json raw;
};

Result<AppConfig> LoadConfig(const std::string& path, bool create_from_example = true);
Result<void> SaveConfigAtomically(const AppConfig& config, const std::string& path);
Result<void> SaveConfigAtomically(const AppConfig& config);

std::vector<std::string> ValidateConfig(const AppConfig& config, bool require_credentials = true);
// Applies one approval policy to a session snapshot without changing the
// on-disk config. The GUI and MCP sessions share this helper so the displayed
// permission and the actual tool policy cannot drift apart.
void ApplyPermissionMode(AppConfig* config, const std::string& mode);
const ProviderConfig* FindProvider(const AppConfig& config, const std::string& provider_id);
const ModelConfig* FindModel(const ProviderConfig& provider, const std::string& model_id);
std::vector<std::string> EffectiveReasoningEfforts(const ModelConfig& model);
std::string EffectiveDefaultReasoningEffort(const ModelConfig& model);
std::string FindConfiguredApiKey(const AppConfig& config);
std::vector<std::string> CollectConfiguredSecrets(const AppConfig& config);
std::string PortableConfigPath(const std::string& value, const std::string& config_path, bool workspace_path);
std::string ResolveConfigPath(const std::string& value, const std::string& config_path);
std::string ConfigToPrettyJson(const AppConfig& config);
nlohmann::json ConfigToJson(const AppConfig& config);
AppConfig ConfigFromJson(const nlohmann::json& document, const std::string& path);
nlohmann::json ParseConfigJson(const std::string& text);

std::string DefaultConfigExampleJson();

}  // namespace spark
