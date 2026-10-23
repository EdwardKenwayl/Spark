#include "core/config.h"
#include "core/context_compactor.h"
#include "core/model_catalog.h"
#include "core/session.h"

#include <iostream>

namespace {
int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

nlohmann::json MinimalConfig() {
    return {
        {"providers", {{"test", {
            {"protocol", "openai_responses"}, {"base_url", "https://api.example/v1"},
            {"api_key", "${ENV:SPARK_CONFIG_TEST_KEY}"},
            {"models", {{{"id", "test-model"}}}}
        }}}},
        {"active", {{"provider", "test"}, {"model", "test-model"}}}
    };
}
}  // namespace

int main() {
    {
        spark::AppConfig defaults;
        spark::ModelConfig unknown;
        Check(spark::EffectiveContextLimit(defaults.context, unknown) == 272000 &&
                  !spark::ShouldCompactContext(defaults.context, unknown, 244799) &&
                  spark::ShouldCompactContext(defaults.context, unknown, 244800),
              "an unset model window uses 272K and compacts at 90 percent");
    }
    const auto path = spark::JoinPath(
        spark::WideToUtf8(spark::fs::temp_directory_path().wstring()),
        "spark-config-" + spark::NewSessionId() + ".json");
    const auto minimal = MinimalConfig();
    auto config = spark::ConfigFromJson(minimal, path);
    Check(spark::ValidateConfig(config, false).empty(), "minimal config has valid derived defaults");
    const auto saved = spark::ConfigToJson(config);
    Check(spark::fs::path(spark::Utf8ToWide(config.workspace.path)).filename() == L"Spark" &&
              saved["workspace"]["path"] == "${DOCUMENTS}/Spark",
          "an omitted workspace resolves to the Spark documents folder and saves portably");
    auto existing_paths = minimal;
    existing_paths["workspace"] = {{"path", "${DOCUMENTS}/existing-output"}};
    existing_paths["storage"] = {{"data_dir", "./existing-data"}};
    const auto existing = spark::ConfigFromJson(existing_paths, path);
    const auto round_trip = spark::ConfigToJson(existing);
    Check(round_trip["workspace"]["path"] == existing_paths["workspace"]["path"] &&
              round_trip["storage"]["data_dir"] == existing_paths["storage"]["data_dir"],
          "explicit workspace and data paths survive config load and save");
    Check(!saved["providers"]["test"]["models"][0].contains("capabilities"),
          "default model capabilities stay omitted");
    Check(!saved["providers"]["test"].contains("headers"), "empty headers stay omitted");
    Check(!saved.contains("agent") && !saved.contains("context") && !saved.contains("skills"),
          "default advanced sections stay omitted");
    Check(!saved["storage"].contains("redact_secrets") && !saved["storage"].contains("log_level"),
          "storage keeps only useful nondefault options");
    Check(saved["workspace"].size() == 1 && saved["permissions"].size() == 1,
          "permission details derive from the selected mode");
    Check(saved["providers"]["test"]["api_key"] == "${ENV:SPARK_CONFIG_TEST_KEY}",
          "saving keeps environment key references");
    Check(spark::SaveConfigAtomically(config, path).ok, "sparse configuration saves");
    const auto before_load = spark::ReadFileUtf8(path);
    const auto loaded = spark::LoadConfig(path, false);
    Check(loaded.ok && spark::ConfigToJson(loaded.value) == saved, "sparse config reload is stable");
    Check(spark::LoadConfig(path, true).ok && spark::ReadFileUtf8(path) == before_load,
          "loading an existing configuration never rewrites its bytes");
    spark::fs::remove(spark::Utf8ToWide(path));

    for (const std::string mode : {"ask", "approve_for_me", "full_access"}) {
        auto document = minimal;
        document["permissions"] = {{"default_mode", mode}};
        auto derived = spark::ConfigFromJson(document, path);
        auto expected = config;
        spark::ApplyPermissionMode(&expected, mode);
        Check(derived.workspace.allow_outside_reads == expected.workspace.allow_outside_reads &&
                  derived.workspace.allow_outside_writes == expected.workspace.allow_outside_writes &&
                  derived.workspace.command_approval == expected.workspace.command_approval &&
                  derived.workspace.file_write_approval == expected.workspace.file_write_approval &&
                  derived.workspace.mcp_tool_approval == expected.workspace.mcp_tool_approval &&
                  derived.permissions.allow_network == expected.permissions.allow_network,
              "omitted permission fields follow mode on load");
        Check(spark::ConfigToJson(derived)["permissions"].size() == 1 &&
                  spark::ConfigToJson(derived)["workspace"].size() == 1,
              "saving mode-derived permissions does not expose duplicate switches");
    }

    auto advanced = minimal;
    advanced["providers"]["test"]["headers"] = {{"X-Test", "value"}};
    advanced["providers"]["test"]["models"][0]["capabilities"] = {
        {"streaming", false}, {"tool_calls", false}, {"images", false}, {"reasoning", false},
        {"reasoning_efforts", {"medium", "high"}}, {"default_reasoning_effort", "high"},
        {"context_window", 1000000}, {"auto_compact_threshold_percent", 72}
    };
    advanced["agent"] = {{"tool_timeout_seconds", 60}, {"max_parallel_read_tools", 2}};
    advanced["context"] = {
        {"auto_compact_threshold_percent", 80}, {"fallback_context_window", 128000},
        {"reserve_output_tokens", 8000}, {"summary_max_tokens", 4000}, {"manual_compact_command", false}
    };
    advanced["permissions"] = {{"default_mode", "full_access"}, {"allow_network", false}};
    advanced["workspace"] = {{"command_approval", "deny"}, {"allow_outside_writes", false}};
    advanced["skills"] = {{"roots", {"./skills"}}, {"include_user_root", false}};
    advanced["storage"] = {{"log_level", "warning"}, {"redact_secrets", false}};
    const auto customized = spark::ConfigFromJson(advanced, path);
    Check(customized.active.reasoning_effort.empty() &&
              spark::EffectiveReasoningEfforts(customized.providers.front().models.front()).empty(),
          "disabled reasoning overrides a retained effort list");
    const auto custom_saved = spark::ConfigToJson(customized);
    Check(custom_saved["providers"] == advanced["providers"],
          "explicit capability overrides, headers and key references survive");
    Check(customized.providers.front().models.front().capabilities.auto_compact_threshold_percent == 72 &&
              custom_saved["providers"]["test"]["models"][0]["capabilities"]["auto_compact_threshold_percent"] == 72,
          "model-level automatic compaction percentage round trips");
    Check(custom_saved["agent"] == advanced["agent"] && custom_saved["context"] == advanced["context"],
          "advanced limits remain editable and round trip");
    Check(custom_saved["permissions"] == advanced["permissions"] &&
              custom_saved["workspace"]["command_approval"] == "deny" &&
              !customized.workspace.allow_outside_writes,
          "explicit permission overrides are preserved");
    Check(custom_saved["storage"]["redact_secrets"] == false && !custom_saved["storage"].contains("log_level"),
          "obsolete logging option disappears without losing redaction override");
    Check(spark::ConfigToJson(spark::ConfigFromJson(custom_saved, path)) == custom_saved,
          "advanced configuration reload is stable");

    const auto& large_model = customized.providers.front().models.front();
    Check(spark::EffectiveContextLimit(customized.context, large_model) == 1000000,
          "known large model is not capped by fallback window");
    Check(!spark::ShouldCompactContext(customized.context, large_model, 719999) &&
              spark::ShouldCompactContext(customized.context, large_model, 720000),
          "model-level compaction percentage overrides the global percentage");
    auto global_threshold_model = large_model;
    global_threshold_model.capabilities.auto_compact_threshold_percent = 0;
    Check(!spark::ShouldCompactContext(customized.context, global_threshold_model, 799999) &&
              spark::ShouldCompactContext(customized.context, global_threshold_model, 800000),
          "compaction uses the global percentage when the model inherits it");
    spark::ModelConfig unknown;
    Check(spark::EffectiveContextLimit(customized.context, unknown) == 128000 &&
              !spark::ShouldCompactContext(customized.context, unknown, 102399) &&
              spark::ShouldCompactContext(customized.context, unknown, 102400),
          "unknown model uses configurable fallback window");
    auto small_model = large_model;
    small_model.capabilities.context_window = 20000;
    Check(!spark::ShouldCompactContext(customized.context, small_model, 11999) &&
              spark::ShouldCompactContext(customized.context, small_model, 12000),
          "output reserve still protects small windows");

    auto legacy = minimal;
    legacy["context"] = {
        {"auto_compact", false}, {"auto_compact_token_limit", 128000},
        {"post_turn_compact_threshold_percent", 75}
    };
    const auto legacy_config = spark::ConfigFromJson(legacy, path);
    const auto legacy_saved = spark::ConfigToJson(legacy_config);
    Check(legacy_config.context.auto_compact &&
              legacy_saved["context"] == nlohmann::json{
                  {"fallback_context_window", 128000}, {"auto_compact_threshold_percent", 75}},
          "legacy settings migrate to percentage and fallback without an off switch");
    for (const int percent : {0, 101}) {
        auto invalid = minimal;
        invalid["context"]["auto_compact_threshold_percent"] = percent;
        Check(!spark::ValidateConfig(spark::ConfigFromJson(invalid, path), false).empty(),
              "out of range percentage is rejected");
    }
    auto reasoning = minimal;
    reasoning["providers"]["test"]["models"] = {
        {{"id", "deepseek-v4.1-flash"}, {"capabilities", {
            {"reasoning_efforts", {"low", "medium", "high", "xhigh", "max", "ultra"}},
            {"default_reasoning_effort", "ultra"}}}},
        {{"id", "gpt-5.4"}, {"capabilities", {{"default_reasoning_effort", "high"}}}}
    };
    reasoning["active"]["model"] = "deepseek-v4.1-flash";
    auto reasoning_config = spark::ConfigFromJson(reasoning, path);
    const auto& deepseek = reasoning_config.providers.front().models.front();
    Check(spark::EffectiveReasoningEfforts(deepseek) == std::vector<std::string>{"low", "high", "max"} &&
              reasoning_config.active.reasoning_effort == "high",
          "obsolete configured efforts are filtered and an invalid default falls back to the model default");
    reasoning_config.providers.front().api_key = "fixture";
    const auto selected = spark::ResolveSubAgentSelection(reasoning_config, {"test", "gpt-5.4", ""});
    Check(selected.ok && selected.value.reasoning_effort == "high",
          "switching a subagent model uses that model's configured default");
    reasoning_config.providers.front().models[1].capabilities.default_reasoning_effort.clear();
    const auto automatic = spark::ResolveSubAgentSelection(reasoning_config, {"test", "gpt-5.4", ""});
    Check(automatic.ok && automatic.value.reasoning_effort == "medium" &&
              spark::ConfiguredModelsPage(reasoning_config, 0, 16)["models"][1]["default_reasoning_effort"] == "medium",
          "automatic selection and the exported catalog share the built-in application default");
    Check(!spark::ResolveSubAgentSelection(reasoning_config, {"test", "deepseek-v4.1-flash", "ultra"}).ok,
          "an explicitly unsupported effort is rejected rather than advertised as a real level");
    reasoning["providers"]["test"]["models"] = {{{"id", "gpt-6-sol"}}};
    reasoning["active"] = {{"provider", "test"}, {"model", "gpt-6-sol"}, {"reasoning_effort", ""}};
    auto resolved = spark::ConfigFromJson(reasoning, path);
    Check(resolved.active.reasoning_effort == "medium",
          "an automatic model default resolves to a concrete level when loading the active selection");
    resolved.providers.front().models.front().capabilities.reasoning_efforts = {"high", "xhigh"};
    Check(spark::EffectiveDefaultReasoningEffort(resolved.providers.front().models.front()) == "high",
          "automatic selection remains concrete when the built-in default is excluded");
    const auto source_example = spark::ReadFileUtf8(
        spark::JoinPath(SPARK_TEST_SOURCE_DIR, "config.example.json"));
    Check(source_example.has_value() &&
              spark::ParseConfigJson(*source_example) == spark::ParseConfigJson(spark::DefaultConfigExampleJson()),
          "shipped and embedded default examples agree");
    return failures == 0 ? 0 : 1;
}
