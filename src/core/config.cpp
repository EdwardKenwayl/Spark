#include "core/config.h"

#include "platform/user_directories.h"

#include <algorithm>
#include <cstdlib>
#include "platform/filesystem.h"
#include <fstream>
#include <set>
#include <string_view>

namespace spark {
namespace {

std::string StringOr(const nlohmann::json& object, const char* key, const std::string& fallback = {}) {
    if (!object.is_object()) return fallback;
    const auto iterator = object.find(key);
    if (iterator == object.end() || !iterator->is_string()) return fallback;
    return iterator->get<std::string>();
}

bool BoolOr(const nlohmann::json& object, const char* key, bool fallback) {
    if (!object.is_object()) return fallback;
    const auto iterator = object.find(key);
    if (iterator == object.end() || !iterator->is_boolean()) return fallback;
    return iterator->get<bool>();
}

int IntOr(const nlohmann::json& object, const char* key, int fallback) {
    if (!object.is_object()) return fallback;
    const auto iterator = object.find(key);
    if (iterator == object.end() || !iterator->is_number_integer()) return fallback;
    return iterator->get<int>();
}

std::string ResolveEnvironmentReference(const std::string& value) {
    constexpr const char* prefix = "${ENV:";
    if (value.rfind(prefix, 0) != 0 || value.size() <= std::char_traits<char>::length(prefix) || value.back() != '}') {
        return value;
    }
    const std::string name = value.substr(std::char_traits<char>::length(prefix), value.size() - std::char_traits<char>::length(prefix) - 1);
    return EnvironmentVariable(name);
}

std::string DocumentsDirectory() {
    return UserDirectory(UserDirectoryKind::Documents);
}

std::string AppDataDirectory() { return UserDirectory(UserDirectoryKind::AppData); }

std::string LocalAppDataDirectory() { return UserDirectory(UserDirectoryKind::LocalAppData); }

std::string StripLeadingPathSeparators(std::string value) {
    while (!value.empty() && (value.front() == '\\' || value.front() == '/')) value.erase(value.begin());
    return value;
}

std::string ExpandConfigPath(const std::string& value, const std::string& config_path) {
    constexpr const char* documents_prefix = "${DOCUMENTS}";
    constexpr const char* executable_prefix = "${EXE_DIR}";
    constexpr const char* config_prefix = "${CONFIG_DIR}";
    constexpr const char* appdata_prefix = "${APPDATA}";
    constexpr const char* local_appdata_prefix = "${LOCALAPPDATA}";
    if (value.rfind(documents_prefix, 0) == 0) {
        return JoinPath(DocumentsDirectory(), StripLeadingPathSeparators(value.substr(std::char_traits<char>::length(documents_prefix))));
    }
    if (value.rfind(executable_prefix, 0) == 0) {
        return JoinPath(GetExecutableDirectory(),
                        StripLeadingPathSeparators(value.substr(std::char_traits<char>::length(executable_prefix))));
    }
    if (value.rfind(config_prefix, 0) == 0) {
        return JoinPath(ParentPath(config_path),
                        StripLeadingPathSeparators(value.substr(std::char_traits<char>::length(config_prefix))));
    }
    if (value.rfind(appdata_prefix, 0) == 0) {
        const std::string base = AppDataDirectory();
        if (base.empty()) return value;
        return JoinPath(base, StripLeadingPathSeparators(value.substr(std::char_traits<char>::length(appdata_prefix))));
    }
    if (value.rfind(local_appdata_prefix, 0) == 0) {
        const std::string base = LocalAppDataDirectory();
        if (base.empty()) return value;
        return JoinPath(base, StripLeadingPathSeparators(value.substr(std::char_traits<char>::length(local_appdata_prefix))));
    }
    return value;
}

// Rewrites an absolute path below a known folder back into its portable token,
// so saving the config never bakes the current user name into the file.
std::string ReplaceKnownPrefix(const std::string& value, const std::string& absolute, const char* token) {
    if (value.empty() || absolute.empty()) return value;
    const std::string normalized_value = NormalizePathForComparison(value);
    const std::string normalized_absolute = NormalizePathForComparison(absolute);
    if (normalized_value == normalized_absolute) return token;
    if (normalized_value.size() <= normalized_absolute.size()) return value;
    if (normalized_value.compare(0, normalized_absolute.size(), normalized_absolute) != 0) return value;
    const char separator = normalized_value[normalized_absolute.size()];
    if (separator != '\\' && separator != '/') return value;
    return std::string(token) + "/" + value.substr(absolute.size() + 1);
}

std::string RelativePathForConfig(const std::string& value, const std::string& base) {
    if (value.empty()) return {};
    std::error_code error;
    const spark::fs::path absolute = NativePath(value);
    const spark::fs::path relative = spark::fs::relative(absolute, NativePath(base), error);
    if (error || relative.empty() || *relative.begin() == "..") return value;
    const std::string portable = relative.generic_u8string();
    return portable == "." ? std::string{"."} : "./" + portable;
}

std::string PortablePathForConfig(const std::string& value, const std::string& config_path, bool workspace_path) {
    if (value.empty()) return {};
    const std::string base = config_path.empty() ? GetExecutableDirectory() : ParentPath(config_path);
    const std::string executable_relative = RelativePathForConfig(value, base);
    if (executable_relative.rfind("./", 0) == 0 || executable_relative == ".") return executable_relative;
    const std::string documents_token = ReplaceKnownPrefix(executable_relative, DocumentsDirectory(), "${DOCUMENTS}");
    if (documents_token != executable_relative) return documents_token;
    const std::string appdata_token = ReplaceKnownPrefix(executable_relative, AppDataDirectory(), "${APPDATA}");
    if (appdata_token != executable_relative) return appdata_token;
    const std::string local_appdata_token =
        ReplaceKnownPrefix(executable_relative, LocalAppDataDirectory(), "${LOCALAPPDATA}");
    if (local_appdata_token != executable_relative) return local_appdata_token;
    if (workspace_path) {
        const std::string default_workspace = JoinPath(DocumentsDirectory(), "Spark");
        if (NormalizePathForComparison(value) == NormalizePathForComparison(default_workspace)) {
            return "${DOCUMENTS}/Spark";
        }
    }
    return value;
}

ModelCapabilities ParseCapabilities(const nlohmann::json& object) {
    ModelCapabilities capabilities;
    capabilities.streaming = BoolOr(object, "streaming", capabilities.streaming);
    capabilities.tool_calls = BoolOr(object, "tool_calls", capabilities.tool_calls);
    capabilities.images = BoolOr(object, "images", capabilities.images);
    capabilities.reasoning = BoolOr(object, "reasoning", capabilities.reasoning);
    if (object.contains("reasoning_efforts") && object["reasoning_efforts"].is_array()) {
        for (const auto& effort : object["reasoning_efforts"]) {
            if (effort.is_string() && !effort.get<std::string>().empty()) {
                capabilities.reasoning_efforts.push_back(effort.get<std::string>());
            }
        }
    }
    capabilities.default_reasoning_effort = StringOr(object, "default_reasoning_effort");
    capabilities.context_window = IntOr(object, "context_window", IntOr(object, "context_tokens", capabilities.context_window));
    capabilities.auto_compact_threshold_percent =
        IntOr(object, "auto_compact_threshold_percent", capabilities.auto_compact_threshold_percent);
    return capabilities;
}

template <typename T>
void WriteOverride(nlohmann::json& object, const char* key, const T& value, const T& default_value) {
    if (value != default_value) object[key] = value;
}

nlohmann::json CapabilitiesToJson(const ModelCapabilities& capabilities) {
    const ModelCapabilities defaults;
    nlohmann::json result = nlohmann::json::object();
    WriteOverride(result, "streaming", capabilities.streaming, defaults.streaming);
    WriteOverride(result, "tool_calls", capabilities.tool_calls, defaults.tool_calls);
    WriteOverride(result, "images", capabilities.images, defaults.images);
    WriteOverride(result, "reasoning", capabilities.reasoning, defaults.reasoning);
    WriteOverride(result, "reasoning_efforts", capabilities.reasoning_efforts, defaults.reasoning_efforts);
    WriteOverride(result, "default_reasoning_effort", capabilities.default_reasoning_effort, defaults.default_reasoning_effort);
    WriteOverride(result, "context_window", capabilities.context_window, defaults.context_window);
    WriteOverride(result, "auto_compact_threshold_percent", capabilities.auto_compact_threshold_percent,
                  defaults.auto_compact_threshold_percent);
    return result;
}

std::map<std::string, std::string> ParseStringMap(const nlohmann::json& object) {
    std::map<std::string, std::string> result;
    if (!object.is_object()) return result;
    for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
        if (iterator->is_string()) result[iterator.key()] = iterator->get<std::string>();
    }
    return result;
}

void ParseModels(const nlohmann::json& owner, ProviderConfig& provider) {
    const auto models = owner.find("models");
    if (models == owner.end() || !models->is_array()) return;
    for (const auto& model : *models) {
        ModelConfig entry;
        entry.id = StringOr(model, "id");
        entry.display_name = StringOr(model, "display_name", entry.id);
        entry.capabilities = ParseCapabilities(model.value("capabilities", nlohmann::json::object()));
        provider.models.push_back(std::move(entry));
    }
}

std::string ExampleJson() {
    return R"JSON({
  // Common settings; see CONFIGURATION.md for advanced options and defaults.
  "schema_version": 2,

  // Current default selection; switch provider, model and reasoning effort in the composer.
  "active": { "provider": "provider-a", "model": "YOUR_MODEL_ID", "reasoning_effort": "" },

  // Duplicate provider-a to add providers, or add them from Settings.
  "providers": {
    "provider-a": {
      "label": "Provider A",
      // Supports openai_responses / responses, openai_chat_completions / chat_completions, and anthropic_messages.
      "protocol": "openai_responses",
      // API root; the application adds the protocol endpoint path as needed.
      "base_url": "https://api.openai.com/v1",
      // Enter your API key, or use ${ENV:VARIABLE_NAME}.
      "api_key": "",
      "models": [
        {
          // Actual model ID; it must be supported by the provider.
          "id": "YOUR_MODEL_ID",
          "display_name": "Your Model",
          "capabilities": {
            // Use the provider's documented model window; this is an example, not a model specification.
            // Streaming, tool calls, images and reasoning default to true and can be omitted.
            "context_window": 272000,
            // Optional per-model override of the global compaction percentage below.
            "auto_compact_threshold_percent": 90
          }
        }
      ]
    }
  },

  // Automatic compaction is always on; it uses a percentage of the model window and reserves output headroom.
  "context": {
    "auto_compact_threshold_percent": 90
  },

  // ask: request approval; approve_for_me: auto-approve within the workspace; full_access: unrestricted access.
  "permissions": { "default_mode": "ask" },

  // Output root for no-project conversations; ${DOCUMENTS} is the current user's Documents folder.
  "workspace": { "path": "${DOCUMENTS}/Spark" },

  // Extra skill roots; <workspace>/.spark/skills and ${APPDATA}/Spark/skills are also scanned.
  "skills": { "roots": ["./skills"] },

  // Optional MCP servers; see CONFIGURATION.md for stdio and streamable_http examples.
  "mcp_servers": {},

  // Session data directory; relative paths resolve against the directory containing config.json.
  "storage": { "data_dir": "./data" },

  // Interface language: system (follow the OS), zh-CN, en-US, zh-TW,
  // ja-JP, ko-KR, de-DE, fr-FR, es-ES, pt-BR, ru-RU.
  // Omitted shell uses cmd.exe on Windows or /bin/sh on Linux/macOS.
  "ui": { "language": "system", "theme": "dark", "send_key": "enter" }
}
)JSON";
}

bool IsSupportedProtocol(const std::string& protocol) {
    static const std::set<std::string> supported = {
        "openai_chat_completions",
        "openai_responses",
        "anthropic_messages",
        "chat_completions",
        "responses",
    };
    return supported.find(protocol) != supported.end();
}

}  // namespace

std::string PortableConfigPath(const std::string& value, const std::string& config_path, bool workspace_path) {
    return PortablePathForConfig(value, config_path, workspace_path);
}

std::string ResolveConfigPath(const std::string& value, const std::string& config_path) {
    return ExpandConfigPath(value, config_path);
}

AppConfig ConfigFromJson(const nlohmann::json& document, const std::string& path) {
    AppConfig config;
    config.config_path = path;
    config.raw = document;
    config.schema_version = IntOr(document, "schema_version", 2);

    const auto active = document.value("active", nlohmann::json::object());
    config.active.provider = StringOr(active, "provider");
    config.active.model = StringOr(active, "model");
    config.active.reasoning_effort = StringOr(active, "reasoning_effort");

    const auto providers = document.find("providers");
    if (providers != document.end() && providers->is_object()) {
        for (auto iterator = providers->begin(); iterator != providers->end(); ++iterator) {
            if (!iterator->is_object()) continue;
            ProviderConfig provider;
            provider.id = iterator.key();
            provider.label = StringOr(*iterator, "label", provider.id);
            provider.protocol = LowerAscii(Trim(StringOr(*iterator, "protocol", provider.protocol)));
            provider.base_url = StringOr(*iterator, "base_url");
            provider.test_model = StringOr(*iterator, "test_model");
            provider.api_key_expression = StringOr(*iterator, "api_key");
            provider.headers = ParseStringMap(iterator->value("headers", nlohmann::json::object()));

            if (provider.api_key_expression.empty()) {
                const auto keys = iterator->find("keys");
                if (keys != iterator->end() && keys->is_array() && !keys->empty()) {
                    const auto& first = keys->front();
                    provider.api_key_expression = StringOr(first, "api_key");
                }
            }
            provider.api_key = ResolveEnvironmentReference(provider.api_key_expression);
            ParseModels(*iterator, provider);
            config.providers.push_back(std::move(provider));
        }
    }

    const auto provider_order = document.find("provider_order");
    if (provider_order != document.end() && provider_order->is_array()) {
        std::map<std::string, size_t> positions;
        for (const auto& id : *provider_order) {
            if (id.is_string()) positions.emplace(id.get<std::string>(), positions.size());
        }
        // Unlisted providers keep their legacy iteration order after the listed IDs.
        std::stable_sort(config.providers.begin(), config.providers.end(), [&](const auto& left, const auto& right) {
            const auto left_position = positions.find(left.id);
            const auto right_position = positions.find(right.id);
            const size_t left_rank = left_position == positions.end() ? positions.size() : left_position->second;
            const size_t right_rank = right_position == positions.end() ? positions.size() : right_position->second;
            return left_rank < right_rank;
        });
    }

    const ProviderConfig* active_provider = FindProvider(config, config.active.provider);
    const ModelConfig* active_model = active_provider == nullptr ? nullptr : FindModel(*active_provider, config.active.model);
    const std::vector<std::string> active_efforts = active_model == nullptr ? std::vector<std::string>{}
                                                                            : EffectiveReasoningEfforts(*active_model);
    if (active_efforts.empty()) {
        config.active.reasoning_effort.clear();
    } else if (std::find(active_efforts.begin(), active_efforts.end(), config.active.reasoning_effort) == active_efforts.end()) {
        config.active.reasoning_effort = EffectiveDefaultReasoningEffort(*active_model);
    }

    const auto context = document.value("context", nlohmann::json::object());
    config.context.auto_compact = BoolOr(context, "auto_compact", config.context.auto_compact);
    config.context.fallback_context_window =
        IntOr(context, "fallback_context_window",
              IntOr(context, "auto_compact_token_limit", config.context.fallback_context_window));
    config.context.auto_compact_threshold_percent =
        IntOr(context, "auto_compact_threshold_percent",
              IntOr(context, "post_turn_compact_threshold_percent", config.context.auto_compact_threshold_percent));
    config.context.reserve_output_tokens = IntOr(context, "reserve_output_tokens", config.context.reserve_output_tokens);
    config.context.summary_max_tokens = IntOr(context, "summary_max_tokens", config.context.summary_max_tokens);
    config.context.manual_compact_command = BoolOr(context, "manual_compact_command", config.context.manual_compact_command);
    // Automatic compaction is always enabled in Spark. The legacy field is
    // still parsed so older config files remain readable, but it cannot disable
    // context protection at runtime.
    config.context.auto_compact = true;

    const auto agent = document.value("agent", nlohmann::json::object());
    config.agent.max_iterations = IntOr(agent, "max_iterations", config.agent.max_iterations);
    config.agent.max_parallel_read_tools =
        IntOr(agent, "max_parallel_read_tools", IntOr(agent, "max_parallel_tools", config.agent.max_parallel_read_tools));
    config.agent.request_timeout_seconds = IntOr(agent, "request_timeout_seconds", config.agent.request_timeout_seconds);
    config.agent.response_header_timeout_seconds =
        IntOr(agent, "response_header_timeout_seconds", config.agent.response_header_timeout_seconds);
    config.agent.tool_timeout_seconds = IntOr(agent, "tool_timeout_seconds", config.agent.tool_timeout_seconds);
    config.agent.max_tool_output_bytes = IntOr(agent, "max_tool_output_bytes", config.agent.max_tool_output_bytes);
    config.agent.max_command_output_bytes = IntOr(agent, "max_command_output_bytes", config.agent.max_command_output_bytes);

    const auto permissions = document.value("permissions", nlohmann::json::object());
    config.permissions.default_mode = StringOr(permissions, "default_mode", config.permissions.default_mode);
    const std::string permission_mode = config.permissions.default_mode;
    ApplyPermissionMode(&config, permission_mode);
    // Preserve invalid modes for validation.
    config.permissions.default_mode = permission_mode;
    config.permissions.workspace_only = BoolOr(permissions, "workspace_only", config.permissions.workspace_only);
    config.permissions.allow_network = BoolOr(permissions, "allow_network", config.permissions.allow_network);

    const auto workspace = document.value("workspace", nlohmann::json::object());
    config.workspace.path = ExpandConfigPath(StringOr(workspace, "path", "${DOCUMENTS}/Spark"), path);
    config.workspace.allow_outside_reads = BoolOr(workspace, "allow_outside_reads", config.workspace.allow_outside_reads);
    config.workspace.allow_outside_writes = BoolOr(workspace, "allow_outside_writes", config.workspace.allow_outside_writes);
    config.workspace.file_write_approval =
        LowerAscii(Trim(StringOr(workspace, "file_write_approval", config.workspace.file_write_approval)));
    config.workspace.command_approval = LowerAscii(Trim(StringOr(workspace, "command_approval", config.workspace.command_approval)));
    config.workspace.mcp_tool_approval = LowerAscii(Trim(StringOr(workspace, "mcp_tool_approval", config.workspace.mcp_tool_approval)));

    const auto skills = document.value("skills", nlohmann::json::object());
    config.skills.enabled = BoolOr(skills, "enabled", config.skills.enabled);
    config.skills.include_workspace_root = BoolOr(skills, "include_workspace_root", config.skills.include_workspace_root);
    config.skills.include_user_root = BoolOr(skills, "include_user_root", config.skills.include_user_root);
    config.skills.max_skills = IntOr(skills, "max_skills", config.skills.max_skills);
    if (skills.contains("roots") && skills["roots"].is_array()) {
        for (const auto& root : skills["roots"]) {
            if (root.is_string()) config.skills.roots.push_back(ExpandConfigPath(root.get<std::string>(), path));
        }
    }

    const auto mcp_servers = document.find("mcp_servers");
    if (mcp_servers != document.end() && mcp_servers->is_object()) {
        for (auto iterator = mcp_servers->begin(); iterator != mcp_servers->end(); ++iterator) {
            if (!iterator->is_object()) continue;
            McpServerConfig server;
            server.id = iterator.key();
            server.enabled = BoolOr(*iterator, "enabled", false);
            server.required = BoolOr(*iterator, "required", false);
            server.transport = LowerAscii(Trim(StringOr(*iterator, "transport", "stdio")));
            server.command = ExpandConfigPath(StringOr(*iterator, "command"), path);
            server.cwd = ExpandConfigPath(StringOr(*iterator, "cwd"), path);
            server.url = StringOr(*iterator, "url");
            server.timeout_seconds = IntOr(*iterator, "timeout_seconds", 60);
            if (server.timeout_seconds <= 0) server.timeout_seconds = 60;
            server.startup_timeout_seconds = IntOr(*iterator, "startup_timeout_sec", 0);
            server.tool_timeout_seconds = IntOr(*iterator, "tool_timeout_sec", 0);
            server.bearer_token_env_var = StringOr(*iterator, "bearer_token_env_var");
            if (iterator->contains("args") && (*iterator)["args"].is_array()) {
                for (const auto& argument : (*iterator)["args"]) {
                    if (argument.is_string()) server.args.push_back(argument.get<std::string>());
                }
            }
            for (const char* list_key : {"enabled_tools", "disabled_tools"}) {
                const auto list = iterator->find(list_key);
                if (list == iterator->end() || !list->is_array()) continue;
                std::vector<std::string>& target =
                    std::string(list_key) == "enabled_tools" ? server.enabled_tools : server.disabled_tools;
                for (const auto& entry : *list) {
                    if (entry.is_string() && !entry.get<std::string>().empty()) target.push_back(entry.get<std::string>());
                }
            }
            server.env = ParseStringMap(iterator->value("env", nlohmann::json::object()));
            server.headers = ParseStringMap(iterator->value("headers", nlohmann::json::object()));
            if (server.headers.empty()) {
                server.headers = ParseStringMap(iterator->value("http_headers", nlohmann::json::object()));
            }
            server.env_http_headers = ParseStringMap(iterator->value("env_http_headers", nlohmann::json::object()));
            config.mcp_servers.push_back(std::move(server));
        }
    }

    const auto storage = document.value("storage", nlohmann::json::object());
    config.storage.data_dir = ExpandConfigPath(StringOr(storage, "data_dir", config.storage.data_dir), path);
    config.storage.redact_secrets = BoolOr(storage, "redact_secrets", true);

    const auto ui = document.value("ui", nlohmann::json::object());
    config.ui.language = StringOr(ui, "language", config.ui.language);
    config.ui.theme = StringOr(ui, "theme", config.ui.theme);
    config.ui.shell = StringOr(ui, "shell", config.ui.shell);
    config.ui.send_key = StringOr(ui, "send_key", config.ui.send_key);
    return config;
}

nlohmann::json ConfigToJson(const AppConfig& config) {
    nlohmann::json document = config.raw.is_object() ? config.raw : nlohmann::json::object();
    document["schema_version"] = 2;
    document["active"] = {{"provider", config.active.provider},
                          {"model", config.active.model},
                          {"reasoning_effort", config.active.reasoning_effort}};

    nlohmann::json providers = nlohmann::json::object();
    nlohmann::json provider_order = nlohmann::json::array();
    for (const ProviderConfig& provider : config.providers) {
        provider_order.push_back(provider.id);
        nlohmann::json models = nlohmann::json::array();
        for (const ModelConfig& model : provider.models) {
            nlohmann::json entry = {{"id", model.id}};
            if (!model.display_name.empty()) WriteOverride(entry, "display_name", model.display_name, model.id);
            auto capabilities = CapabilitiesToJson(model.capabilities);
            if (!capabilities.empty()) entry["capabilities"] = std::move(capabilities);
            models.push_back(std::move(entry));
        }
        providers[provider.id] = {
            {"protocol", provider.protocol},
            {"base_url", provider.base_url},
            {"api_key", provider.api_key_expression.empty() ? provider.api_key : provider.api_key_expression},
            {"models", std::move(models)},
        };
        if (!provider.label.empty()) WriteOverride(providers[provider.id], "label", provider.label, provider.id);
        if (!provider.headers.empty()) providers[provider.id]["headers"] = provider.headers;
        if (!provider.test_model.empty()) providers[provider.id]["test_model"] = provider.test_model;
    }
    document["providers"] = std::move(providers);
    document["provider_order"] = std::move(provider_order);
    document.erase("routing");
    const AppConfig defaults;
    document["context"] = nlohmann::json::object();
    auto& context = document["context"];
    WriteOverride(context, "fallback_context_window", config.context.fallback_context_window, defaults.context.fallback_context_window);
    WriteOverride(context, "auto_compact_threshold_percent", config.context.auto_compact_threshold_percent, defaults.context.auto_compact_threshold_percent);
    WriteOverride(context, "reserve_output_tokens", config.context.reserve_output_tokens, defaults.context.reserve_output_tokens);
    WriteOverride(context, "summary_max_tokens", config.context.summary_max_tokens, defaults.context.summary_max_tokens);
    WriteOverride(context, "manual_compact_command", config.context.manual_compact_command, defaults.context.manual_compact_command);
    document["agent"] = nlohmann::json::object();
    auto& agent = document["agent"];
    WriteOverride(agent, "max_iterations", config.agent.max_iterations, defaults.agent.max_iterations);
    WriteOverride(agent, "max_parallel_read_tools", config.agent.max_parallel_read_tools, defaults.agent.max_parallel_read_tools);
    WriteOverride(agent, "request_timeout_seconds", config.agent.request_timeout_seconds, defaults.agent.request_timeout_seconds);
    WriteOverride(agent, "response_header_timeout_seconds", config.agent.response_header_timeout_seconds, defaults.agent.response_header_timeout_seconds);
    WriteOverride(agent, "tool_timeout_seconds", config.agent.tool_timeout_seconds, defaults.agent.tool_timeout_seconds);
    WriteOverride(agent, "max_tool_output_bytes", config.agent.max_tool_output_bytes, defaults.agent.max_tool_output_bytes);
    WriteOverride(agent, "max_command_output_bytes", config.agent.max_command_output_bytes, defaults.agent.max_command_output_bytes);

    AppConfig policy_defaults;
    ApplyPermissionMode(&policy_defaults, config.permissions.default_mode);
    document["permissions"] = {{"default_mode", config.permissions.default_mode}};
    auto& permissions = document["permissions"];
    WriteOverride(permissions, "workspace_only", config.permissions.workspace_only, policy_defaults.permissions.workspace_only);
    WriteOverride(permissions, "allow_network", config.permissions.allow_network, policy_defaults.permissions.allow_network);
    document["workspace"] = {
        {"path", PortablePathForConfig(config.workspace.path, config.config_path, true)},
    };
    auto& workspace = document["workspace"];
    WriteOverride(workspace, "allow_outside_reads", config.workspace.allow_outside_reads, policy_defaults.workspace.allow_outside_reads);
    WriteOverride(workspace, "allow_outside_writes", config.workspace.allow_outside_writes, policy_defaults.workspace.allow_outside_writes);
    WriteOverride(workspace, "file_write_approval", config.workspace.file_write_approval, policy_defaults.workspace.file_write_approval);
    WriteOverride(workspace, "command_approval", config.workspace.command_approval, policy_defaults.workspace.command_approval);
    WriteOverride(workspace, "mcp_tool_approval", config.workspace.mcp_tool_approval, policy_defaults.workspace.mcp_tool_approval);
    nlohmann::json skill_roots = nlohmann::json::array();
    for (const std::string& root : config.skills.roots) {
        skill_roots.push_back(PortablePathForConfig(root, config.config_path, false));
    }
    document["skills"] = nlohmann::json::object();
    auto& skills = document["skills"];
    WriteOverride(skills, "enabled", config.skills.enabled, defaults.skills.enabled);
    WriteOverride(skills, "include_workspace_root", config.skills.include_workspace_root, defaults.skills.include_workspace_root);
    WriteOverride(skills, "include_user_root", config.skills.include_user_root, defaults.skills.include_user_root);
    WriteOverride(skills, "max_skills", config.skills.max_skills, defaults.skills.max_skills);
    if (!skill_roots.empty()) skills["roots"] = std::move(skill_roots);

    nlohmann::json servers = nlohmann::json::object();
    for (const McpServerConfig& server : config.mcp_servers) {
        nlohmann::json entry = {
            {"enabled", server.enabled},
            {"required", server.required},
            {"transport", server.transport},
            {"timeout_seconds", server.timeout_seconds},
        };
        if (server.startup_timeout_seconds > 0) entry["startup_timeout_sec"] = server.startup_timeout_seconds;
        if (server.tool_timeout_seconds > 0) entry["tool_timeout_sec"] = server.tool_timeout_seconds;
        if (!server.enabled_tools.empty()) entry["enabled_tools"] = server.enabled_tools;
        if (!server.disabled_tools.empty()) entry["disabled_tools"] = server.disabled_tools;
        if (server.transport == "stdio") {
            entry["command"] = PortablePathForConfig(server.command, config.config_path, false);
            if (!server.cwd.empty()) entry["cwd"] = PortablePathForConfig(server.cwd, config.config_path, false);
            entry["args"] = server.args;
            entry["env"] = server.env;
        } else {
            entry["url"] = server.url;
            entry["headers"] = server.headers;
            if (!server.env_http_headers.empty()) entry["env_http_headers"] = server.env_http_headers;
            if (!server.bearer_token_env_var.empty()) entry["bearer_token_env_var"] = server.bearer_token_env_var;
        }
        servers[server.id] = std::move(entry);
    }
    document["mcp_servers"] = std::move(servers);
    document["storage"] = {
        {"data_dir", PortablePathForConfig(config.storage.data_dir, config.config_path, false)},
    };
    WriteOverride(document["storage"], "redact_secrets", config.storage.redact_secrets, defaults.storage.redact_secrets);
    document["ui"] = {
        {"language", config.ui.language},
        {"theme", config.ui.theme},
        {"shell", config.ui.shell},
        {"send_key", config.ui.send_key},
    };
    for (const char* section : {"context", "agent", "skills"}) {
        if (document[section].empty()) document.erase(section);
    }
    return document;
}

Result<AppConfig> LoadConfig(const std::string& path, bool create_from_example) {
    std::string error;
    const auto contents = ReadFileUtf8(path, &error);
    if (!contents.has_value()) {
        if (!create_from_example) return Result<AppConfig>::Failure("could not read config: " + error);
        const std::string example = DefaultConfigExampleJson();
        if (!WriteWindowsTextFileAtomic(path, example, &error)) return Result<AppConfig>::Failure("could not create config: " + error);
        return LoadConfig(path, false);
    }
    try {
        const nlohmann::json document = ParseConfigJson(*contents);
        return Result<AppConfig>::Success(ConfigFromJson(document, path));
    } catch (const nlohmann::json::exception& exception) {
        return Result<AppConfig>::Failure(std::string("invalid config JSON: ") + exception.what());
    }
}

Result<void> SaveConfigAtomically(const AppConfig& config, const std::string& path) {
    std::string error;
    if (!WriteWindowsTextFileAtomic(path, ConfigToPrettyJson(config), &error)) {
        return Result<void>::Failure("could not save config: " + error);
    }
    return Result<void>::Success();
}

nlohmann::json ParseConfigJson(const std::string& text) {
    return nlohmann::json::parse(text, nullptr, true, true);
}

Result<void> SaveConfigAtomically(const AppConfig& config) { return SaveConfigAtomically(config, config.config_path); }

std::vector<std::string> ValidateConfig(const AppConfig& config, bool require_credentials) {
    std::vector<std::string> errors;
    if (config.providers.empty()) errors.push_back("providers: at least one provider is required");
    std::vector<std::string> provider_ids;
    for (const ProviderConfig& provider : config.providers) {
        const std::string prefix = "providers." + provider.id;
        if (provider.id.empty()) errors.push_back("providers: provider id cannot be empty");
        const std::string protocol = LowerAscii(Trim(provider.protocol));
        if (protocol.empty() || !IsSupportedProtocol(protocol)) errors.push_back(prefix + ".protocol: unsupported protocol");
        if (provider.base_url.empty()) errors.push_back(prefix + ".base_url: empty");
        if (provider.models.empty()) errors.push_back(prefix + ".models: at least one model is required");
        for (const ModelConfig& model : provider.models) {
            if (model.id.empty()) errors.push_back(prefix + ".models: model id cannot be empty");
            if (model.capabilities.auto_compact_threshold_percent < 0 ||
                model.capabilities.auto_compact_threshold_percent > 100) {
                errors.push_back(prefix + ".models." + model.id +
                                  ".capabilities.auto_compact_threshold_percent: expected 1-100 or 0 to inherit");
            }
        }
        provider_ids.push_back(provider.id);
    }
    if (std::find(provider_ids.begin(), provider_ids.end(), config.active.provider) == provider_ids.end()) {
        errors.push_back("active.provider: provider does not exist");
    }
    const ProviderConfig* provider = FindProvider(config, config.active.provider);
    if (provider != nullptr) {
        if (FindModel(*provider, config.active.model) == nullptr) {
            errors.push_back("active.model: model does not exist for the active provider");
        }
        if (require_credentials && provider->api_key.empty()) {
            errors.push_back("providers." + provider->id + ".api_key: empty");
        }
    }
    if (config.agent.max_iterations <= 0 || config.agent.max_parallel_read_tools <= 0 ||
        config.agent.request_timeout_seconds <= 0 || config.agent.tool_timeout_seconds <= 0 ||
        config.agent.response_header_timeout_seconds <= 0 ||
        config.agent.max_tool_output_bytes <= 0 || config.agent.max_command_output_bytes <= 0) {
        errors.push_back("agent: limits must be positive");
    }
    if (config.context.fallback_context_window <= 0 || config.context.reserve_output_tokens <= 0 ||
        config.context.summary_max_tokens <= 0 || config.context.auto_compact_threshold_percent < 1 ||
        config.context.auto_compact_threshold_percent > 100) {
        errors.push_back("context: invalid compaction limits");
    }
    const std::string permission_mode = LowerAscii(Trim(config.permissions.default_mode));
    if (permission_mode != "ask" && permission_mode != "approve_for_me" && permission_mode != "full_access") {
        errors.push_back("permissions.default_mode: invalid permission mode");
    }
    const auto valid_approval_policy = [](const std::string& value) {
        const std::string policy = LowerAscii(Trim(value));
        return policy == "deny" || policy == "ask" || policy == "allow";
    };
    if (!valid_approval_policy(config.workspace.file_write_approval)) {
        errors.push_back("workspace.file_write_approval: expected deny, ask, or allow");
    }
    if (!valid_approval_policy(config.workspace.command_approval)) {
        errors.push_back("workspace.command_approval: expected deny, ask, or allow");
    }
    if (!valid_approval_policy(config.workspace.mcp_tool_approval)) {
        errors.push_back("workspace.mcp_tool_approval: expected deny, ask, or allow");
    }
    if (config.workspace.path.empty()) errors.push_back("workspace.path: choose a workspace directory");
    for (const McpServerConfig& server : config.mcp_servers) {
        // Disabled entries are templates; only enforce shape for servers that run.
        if (!server.enabled) continue;
        const std::string prefix = "mcp_servers." + server.id;
        if (server.transport != "stdio" && server.transport != "streamable_http") {
            errors.push_back(prefix + ".transport: expected stdio or streamable_http");
            continue;
        }
        if (server.transport == "stdio" && server.command.empty()) {
            errors.push_back(prefix + ".command: empty");
        }
        if (server.transport == "streamable_http" && server.url.empty()) {
            errors.push_back(prefix + ".url: empty");
        }
    }
    return errors;
}

void ApplyPermissionMode(AppConfig* config, const std::string& mode) {
    if (config == nullptr) return;
    const std::string normalized = LowerAscii(Trim(mode));
    if (normalized == "read_only") {
        config->permissions.default_mode = "ask";
        config->permissions.workspace_only = true;
        config->permissions.allow_network = false;
        config->workspace.allow_outside_reads = false;
        config->workspace.allow_outside_writes = false;
        config->workspace.file_write_approval = "deny";
        config->workspace.command_approval = "deny";
        config->workspace.mcp_tool_approval = "deny";
        return;
    }
    if (normalized == "approve_for_me") {
        config->permissions.default_mode = "approve_for_me";
        config->permissions.workspace_only = true;
        config->permissions.allow_network = false;
        config->workspace.allow_outside_reads = false;
        config->workspace.allow_outside_writes = false;
        config->workspace.file_write_approval = "allow";
        config->workspace.command_approval = "allow";
        config->workspace.mcp_tool_approval = "allow";
        return;
    }
    if (normalized == "full_access") {
        config->permissions.default_mode = "full_access";
        config->permissions.workspace_only = false;
        config->permissions.allow_network = true;
        config->workspace.allow_outside_reads = true;
        config->workspace.allow_outside_writes = true;
        config->workspace.file_write_approval = "allow";
        config->workspace.command_approval = "allow";
        config->workspace.mcp_tool_approval = "allow";
        return;
    }
    config->permissions.default_mode = "ask";
    config->permissions.workspace_only = true;
    config->permissions.allow_network = false;
    config->workspace.allow_outside_reads = false;
    config->workspace.allow_outside_writes = false;
    config->workspace.file_write_approval = "ask";
    config->workspace.command_approval = "ask";
    config->workspace.mcp_tool_approval = "ask";
}

const ProviderConfig* FindProvider(const AppConfig& config, const std::string& provider_id) {
    for (const ProviderConfig& provider : config.providers) {
        if (provider.id == provider_id) return &provider;
    }
    return nullptr;
}

const ModelConfig* FindModel(const ProviderConfig& provider, const std::string& model_id) {
    for (const ModelConfig& model : provider.models) {
        if (model.id == model_id) return &model;
    }
    return nullptr;
}

std::vector<std::string> EffectiveReasoningEfforts(const ModelConfig& model) {
    if (!model.capabilities.reasoning) return {};
    const std::string model_name = LowerAscii(model.id);
    using Profile = std::pair<std::string_view, std::vector<std::string>>;
    static const std::vector<Profile> profiles = {
        {"deepseek-v4", {"low", "high", "max"}},
        {"deepseek-flash", {"low", "high", "max"}},
        {"gpt-5.1-codex-max", {"none", "low", "medium", "high", "xhigh"}},
        {"gpt-5.1-codex", {"low", "medium", "high"}},
        {"gpt-5.1", {"none", "low", "medium", "high"}},
        {"gpt-5-pro", {"high"}},
        {"gpt-5.2-pro", {"medium", "high", "xhigh"}},
        {"gpt-5.3-codex-spark", {"low", "medium", "high", "xhigh"}},
        {"gpt-5.3-codex", {"low", "medium", "high", "xhigh"}},
        {"gpt-5.4-mini", {"low", "medium", "high", "xhigh"}},
        {"gpt-5.4", {"low", "medium", "high", "xhigh"}},
        {"gpt-5.2", {"none", "low", "medium", "high", "xhigh"}},
        {"gpt-5.5", {"low", "medium", "high", "xhigh"}},
        {"gpt-5.6-sol", {"low", "medium", "high", "xhigh", "max", "ultra"}},
        {"gpt-5.6-terra", {"low", "medium", "high", "xhigh", "max", "ultra"}},
        {"gpt-5.6-luna", {"low", "medium", "high", "xhigh", "max"}},
        {"gpt-5.6", {"low", "medium", "high", "xhigh", "max"}},
        {"gpt-5-codex", {"low", "medium", "high"}},
        {"gpt-5", {"minimal", "low", "medium", "high"}},
        {"gpt-6-astra", {"low", "medium", "high", "xhigh", "max", "ultra"}},
        {"gpt-6.1-sol", {"low", "medium", "high", "xhigh", "max", "ultra"}},
        {"gpt-6-sol", {"none", "low", "medium", "high", "xhigh", "max", "ultra"}},
        {"gpt-6-luna", {"none", "low", "medium", "high", "xhigh", "max"}},
        {"gpt-oss", {"low", "medium", "high"}},
        {"doubao-seed-2.0-mini", {"low", "medium", "high"}},
        {"doubao-seed-2.0-lite", {"minimal", "low", "medium", "high"}},
        {"doubao-seed-2.1", {"low", "medium", "high"}},
        {"doubao-seed-evolving", {"low", "medium", "high"}},
        {"gemini-3.1-pro", {"low", "high"}},
        {"gemini-3.5-flash-lite", {"low", "medium", "high"}},
        {"gemini-3.7-flash", {"low", "medium", "high"}},
        {"gemini-3.8-flash", {"low", "medium", "high"}},
        {"glm-5.3", {"low", "high", "max"}},
        {"glm-5-turbo", {"high"}},
        {"glm-5.2", {"high", "max"}},
        {"glm-5.1", {"high"}},
        {"glm-5v-turbo", {"low", "high", "max"}},
        {"grok-4.5", {"low", "medium", "high"}},
        {"grok-4.6", {"low", "medium", "high", "xhigh"}},
        {"grok-4.7", {"low", "medium", "high", "xhigh"}},
        {"kimi-k2.5", {"low", "medium", "high", "xhigh"}},
        {"kimi-k2.6", {"low", "medium", "high", "xhigh"}},
        {"kimi-k2.7-code", {"low", "medium", "high", "xhigh"}},
        {"kimi-k2.8", {"low", "high", "max"}},
        {"kimi-k3", {"low", "high", "max"}},
        {"mimo-v2.5", {"none", "high"}},
        {"mimo-v2.6", {"none", "high"}},
        {"minimax-m3", {"none", "high"}},
        {"minimax-m2.7", {"low", "medium", "high", "xhigh"}},
        {"minimax-m2.5", {"low", "medium", "high", "xhigh"}},
        {"mistral-medium-3.5", {"low", "medium", "high"}},
        {"muse-spark-1.3", {"low", "high", "max"}},
        {"muse-glimmer", {"low", "high"}},
        {"nemotron-3-ultra", {"low", "medium", "high"}},
        {"qwen3.8", {"low", "medium", "xhigh"}},
        {"qwen3.7", {"low", "medium", "high", "xhigh"}},
        {"qwen3.6", {"low", "medium", "high", "xhigh"}},
        {"qwen3-coder", {"low", "medium", "high", "xhigh"}},
        {"step-3.7-flash", {"low", "medium", "high"}},
        {"step-5-preview", {"low", "medium", "high"}},
        {"inkling", {"low", "medium", "high", "max"}},
    };
    for (const auto& [pattern, efforts] : profiles) {
        if (model_name.find(pattern) == std::string::npos) continue;
        if (model.capabilities.reasoning_efforts.empty()) return efforts;

        std::vector<std::string> filtered;
        for (const std::string& configured : model.capabilities.reasoning_efforts) {
            if (std::find(efforts.begin(), efforts.end(), configured) != efforts.end() &&
                std::find(filtered.begin(), filtered.end(), configured) == filtered.end()) {
                filtered.push_back(configured);
            }
        }
        return filtered.empty() ? efforts : filtered;
    }
    return model.capabilities.reasoning_efforts;
}

std::string EffectiveDefaultReasoningEffort(const ModelConfig& model) {
    if (!model.capabilities.reasoning) return {};
    const auto efforts = EffectiveReasoningEfforts(model);
    if (efforts.empty()) return {};

    const std::string configured = Trim(model.capabilities.default_reasoning_effort);
    if (std::find(efforts.begin(), efforts.end(), configured) != efforts.end()) return configured;

    const std::string model_name = LowerAscii(model.id);
    using DefaultProfile = std::pair<std::string_view, std::string_view>;
    static const std::vector<DefaultProfile> defaults = {
        {"deepseek-v4", "high"}, {"deepseek-flash", "high"},
        {"gpt-5.1-codex-max", "medium"}, {"gpt-5.1", "medium"},
        {"gpt-5-pro", "high"}, {"gpt-5.2-pro", "medium"}, {"gpt-5.2", "medium"},
        {"gpt-5.3-codex-spark", "low"}, {"gpt-5.3-codex", "medium"},
        {"gpt-5.4-mini", "medium"}, {"gpt-5.4", "medium"},
        {"gpt-5.5", "medium"}, {"gpt-5.6-sol", "low"}, {"gpt-5.6-terra", "medium"},
        {"gpt-5.6-luna", "medium"}, {"gpt-5.6", "medium"},
        {"gpt-6.1-sol", "medium"}, {"gpt-6-sol", "medium"}, {"gpt-6-luna", "medium"},
        {"gpt-6-astra", "medium"}, {"gpt-oss", "high"},
        {"gpt-5", "medium"},
        {"doubao-seed-2.0-mini", "medium"}, {"doubao-seed-2.0-lite", "medium"},
        {"doubao-seed-2.1-pro", "high"}, {"doubao-seed-2.1", "medium"},
        {"doubao-seed-evolving", "high"},
        {"gemini-3.1-pro", "high"}, {"gemini-3.5-flash-lite", "medium"},
        {"gemini-3.7-flash", "medium"}, {"gemini-3.8-flash", "medium"},
        {"glm-5.3", "max"}, {"glm-5-turbo", "high"}, {"glm-5.2", "max"},
        {"glm-5.1", "high"}, {"glm-5v-turbo", "max"},
        {"grok-4.5", "high"}, {"grok-4.6", "high"}, {"grok-4.7", "high"},
        {"kimi-k2.5", "medium"}, {"kimi-k2.6", "medium"}, {"kimi-k2.7-code", "medium"},
        {"kimi-k2.8", "max"}, {"kimi-k3", "high"},
        {"mimo-v2.5", "high"}, {"mimo-v2.6", "high"}, {"minimax-m3", "high"},
        {"minimax-m2.7", "medium"}, {"minimax-m2.5", "medium"},
        {"mistral-medium-3.5", "medium"}, {"muse-spark-1.3", "high"}, {"muse-glimmer", "high"},
        {"nemotron-3-ultra", "high"}, {"qwen3.8-27b", "medium"}, {"qwen3.8", "xhigh"},
        {"qwen3.7", "medium"}, {"qwen3.6", "medium"}, {"qwen3-coder", "medium"},
        {"step-3.7-flash", "high"}, {"step-5-preview", "high"}, {"inkling", "high"},
    };
    for (const auto& [pattern, effort] : defaults) {
        if (model_name.find(pattern) != std::string::npos &&
            std::find(efforts.begin(), efforts.end(), effort) != efforts.end()) {
            return std::string(effort);
        }
    }
    // A restricted or custom list still needs a concrete, enabled application default.
    const auto medium = std::find(efforts.begin(), efforts.end(), "medium");
    return medium == efforts.end() ? efforts.front() : *medium;
}

std::string FindConfiguredApiKey(const AppConfig& config) {
    const ProviderConfig* provider = FindProvider(config, config.active.provider);
    return provider == nullptr ? std::string{} : provider->api_key;
}

std::vector<std::string> CollectConfiguredSecrets(const AppConfig& config) {
    std::vector<std::string> secrets;
    for (const ProviderConfig& provider : config.providers) {
        if (!provider.api_key.empty()) secrets.push_back(provider.api_key);
        for (const auto& [name, value] : provider.headers) {
            if (!value.empty() && (EqualsIgnoreCaseAscii(name, "authorization") || LowerAscii(name).find("key") != std::string::npos)) {
                secrets.push_back(value);
            }
        }
    }
    for (const McpServerConfig& server : config.mcp_servers) {
        for (const auto& [name, value] : server.headers) {
            if (!value.empty() && (EqualsIgnoreCaseAscii(name, "authorization") || LowerAscii(name).find("key") != std::string::npos)) {
                secrets.push_back(value);
            }
        }
        for (const auto& [name, value] : server.env) {
            if (!value.empty() && (LowerAscii(name).find("key") != std::string::npos || LowerAscii(name).find("token") != std::string::npos)) {
                secrets.push_back(value);
            }
        }
    }
    return secrets;
}

std::string ConfigToPrettyJson(const AppConfig& config) {
    return ConfigToJson(config).dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

std::string DefaultConfigExampleJson() { return ExampleJson(); }

}  // namespace spark
