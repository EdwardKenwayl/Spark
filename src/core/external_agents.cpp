#include "core/external_agents.h"

#include "core/agent.h"
#include "core/context_compactor.h"
#include "core/tools.h"
#include "platform/clock.h"

#include <algorithm>
#include <chrono>
#include "platform/filesystem.h"
#include <sstream>
#include <stdexcept>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace spark {
namespace {

constexpr size_t kMaxExternalRuns = 64;
constexpr size_t kMaxConcurrentRuns = 4;
constexpr size_t kMaxTaskBytes = 64000;
constexpr size_t kMaxOutputBytes = 96000;

uint64_t CurrentProcessId() {
#ifdef _WIN32
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(::getpid());
#endif
}

std::string Bounded(std::string value, size_t limit) {
    if (value.size() <= limit) return value;
    return TruncateUtf8(std::move(value), limit) + "\n[truncated]";
}

std::string TitleFromTask(const std::string& task) {
    std::istringstream stream(task);
    std::string line;
    while (std::getline(stream, line)) {
        line = Trim(std::move(line));
        if (line.empty()) continue;
        const std::string lowered = LowerAscii(line);
        if (lowered.rfind("workspace:", 0) == 0 || lowered.rfind("project:", 0) == 0 ||
            line.rfind("工作区：", 0) == 0 || line.rfind("项目路径：", 0) == 0) {
            continue;
        }
        if (lowered.rfind("task:", 0) == 0) line = Trim(line.substr(5));
        if (line.rfind("任务：", 0) == 0) line = Trim(line.substr(std::string("任务：").size()));
        while (!line.empty() && (line.front() == '#' || line.front() == '-' || line.front() == '*')) {
            line = Trim(line.substr(1));
        }
        if (line.empty()) continue;
        size_t sentence_end = line.find_first_of(".!?");
        for (const char* punctuation : {"。", "！", "？"}) {
            const size_t position = line.find(punctuation);
            if (position != std::string::npos && (sentence_end == std::string::npos || position < sentence_end)) {
                sentence_end = position;
            }
        }
        if (sentence_end != std::string::npos && sentence_end > 0) line.resize(sentence_end);
        if (line.size() > 80) line = TruncateUtf8(std::move(line), 80);
        return line;
    }
    return "外部子智能体";
}

std::string NormalizePermissionMode(const std::string& mode) {
    const std::string normalized = LowerAscii(Trim(mode));
    if (normalized == "read_only" || normalized == "full_access" || normalized == "approve_for_me" ||
        normalized == "ask") return normalized;
    return "read_only";
}

std::string ProjectScopeForWorkspace(const AppConfig& config, const std::string& config_path,
                                     const std::string& workspace) {
    if (Trim(workspace).empty()) return "recent";
    const std::string normalized_workspace = NormalizePathForComparison(workspace);
    if (normalized_workspace.empty()) return "recent";

    std::string error;
    const auto contents = ReadFileUtf8(JoinPath(config.storage.data_dir, "projects.json"), &error);
    if (!contents.has_value()) return "recent";

    try {
        const nlohmann::json document = nlohmann::json::parse(*contents);
        if (!document.is_object() || !document.contains("projects") || !document["projects"].is_array()) {
            return "recent";
        }
        const std::string config_directory = config_path.empty() ? GetExecutableDirectory() : ParentPath(config_path);
        const auto matches = [&](const std::string& value) {
            if (Trim(value).empty()) return false;
            const std::string resolved = AbsolutePath(ResolveConfigPath(value, config_path), config_directory);
            return NormalizePathForComparison(resolved) == normalized_workspace;
        };
        for (const auto& item : document["projects"]) {
            if (!item.is_object()) continue;
            if (matches(item.value("workspace_path", std::string{}))) return "project";
            if (!item.contains("folders") || !item["folders"].is_array()) continue;
            for (const auto& folder : item["folders"]) {
                if (folder.is_string() && matches(folder.get<std::string>())) return "project";
            }
        }
    } catch (const nlohmann::json::exception&) {
        return "recent";
    }
    return "recent";
}

}  // namespace

bool IsExternalAgentRegistryFresh(const nlohmann::json& document, uint64_t now) {
    if (!document.is_object()) return false;
    const auto pid = document.find("process_id"), updated = document.find("updated_at");
    if (pid == document.end() || updated == document.end() ||
        !pid->is_number_unsigned() || !updated->is_number_unsigned()) return false;
    const auto process_id = pid->get<uint64_t>(), timestamp = updated->get<uint64_t>();
    if (process_id == 0 || process_id > UINT32_MAX || timestamp == 0) return false;
    return timestamp <= now ? now - timestamp <= 15 : timestamp - now <= 5;
}

std::string ResolveExternalAgentScope(const AppConfig& config, const std::string& config_path,
                                      const std::string& workspace) {
    return ProjectScopeForWorkspace(config, config_path, workspace);
}

ExternalAgentManager::ExternalAgentManager(AppConfig config, std::string config_path)
    : config_(std::move(config)), config_path_(std::move(config_path)) {}

ExternalAgentManager::~ExternalAgentManager() {
    registry_writer_stop_.store(true);
    if (registry_writer_.joinable()) registry_writer_.join();
    std::vector<std::shared_ptr<Run>> runs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : runs_) runs.push_back(entry.second);
    }
    for (const auto& run : runs) {
        run->cancellation.Cancel();
        run->condition.notify_all();
    }
    for (const auto& run : runs) {
        if (run->worker.joinable()) run->worker.join();
    }
    WriteRegistry();
}

Result<void> ExternalAgentManager::Initialize() {
    mcp_ = std::make_unique<McpManager>(config_);
    const auto discovered = mcp_->DiscoverEnabled();
    if (!discovered.ok) return Result<void>::Failure("MCP discovery failed: " + discovered.error);
    if (!registry_writer_.joinable()) {
        registry_writer_ = std::thread([this] {
            try {
                while (!registry_writer_stop_.load()) {
                    ProcessControlRequests();
                    WriteRegistry();
                    for (int tick = 0; tick < 20 && !registry_writer_stop_.load(); ++tick) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                }
            } catch (...) {
            }
        });
    }
    return Result<void>::Success();
}

void ExternalAgentManager::ProcessControlRequests() {
    const std::string directory = JoinPath(config_.storage.data_dir, "external_agents");
    const std::string path = JoinPath(directory, "control.json");
    const auto contents = ReadFileUtf8(path);
    if (!contents.has_value()) return;
    std::string id;
    std::string action;
    try {
        const nlohmann::json request = nlohmann::json::parse(*contents);
        action = LowerAscii(Trim(request.value("action", std::string{})));
        id = Trim(request.value("id", std::string{}));
    } catch (const nlohmann::json::exception&) {
    }
    if (action == "cancel" && !id.empty()) Cancel(id);
    else if ((action == "approve" || action == "deny") && !id.empty()) Approve(id, action == "approve");
    std::error_code error;
    spark::fs::remove(NativePath(path), error);
}

std::string ExternalAgentManager::ModelsJson() const {
    nlohmann::json providers = nlohmann::json::array();
    for (const ProviderConfig& provider : config_.providers) {
        nlohmann::json models = nlohmann::json::array();
        for (const ModelConfig& model : provider.models) {
            models.push_back({{"id", model.id},
                              {"display_name", model.display_name.empty() ? model.id : model.display_name},
                              {"context_window", model.capabilities.context_window},
                              {"supports_tools", model.capabilities.tool_calls},
                              {"supports_images", model.capabilities.images},
                              {"supports_reasoning", model.capabilities.reasoning},
                              {"reasoning_efforts", EffectiveReasoningEfforts(model)},
                              {"default_reasoning_effort", EffectiveDefaultReasoningEffort(model)}});
        }
        providers.push_back({{"id", provider.id}, {"label", provider.label}, {"models", std::move(models)}});
    }
    return nlohmann::json({{"active", {{"provider", config_.active.provider},
                                       {"model", config_.active.model},
                                       {"reasoning_effort", config_.active.reasoning_effort}}},
                           {"providers", std::move(providers)}})
        .dump(2);
}

void ExternalAgentManager::WriteRegistry() const {
    std::lock_guard<std::mutex> registry_lock(registry_write_mutex_);
    std::string directory = JoinPath(config_.storage.data_dir, "external_agents");
    std::error_code directory_error;
    spark::fs::create_directories(NativePath(directory), directory_error);
    if (directory_error) return;

    nlohmann::json runs = nlohmann::json::array();
    std::vector<std::shared_ptr<Run>> active_runs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : runs_) active_runs.push_back(entry.second);
    }
    for (const auto& run : active_runs) {
        ExternalAgentInfo info;
        {
            std::lock_guard<std::mutex> lock(run->mutex);
            info = run->info;
        }
        if (info.id.empty()) continue;
            runs.push_back({
                {"id", info.id},
                {"name", info.name},
                {"state", info.state},
                {"session_path", info.session_path},
                {"workspace", info.workspace},
                {"provider", info.provider},
                {"model", info.model},
                {"reasoning_effort", info.reasoning_effort},
                {"permission_mode", info.permission_mode},
                {"scope", info.scope},
                {"phase", info.phase},
                {"reasoning", info.reasoning},
                {"active_tool_name", info.active_tool_name},
                {"active_tool_arguments", info.active_tool_arguments},
                {"started_at", info.started_at},
                {"finished_at", info.finished_at},
                {"output_complete", info.output_complete},
                {"awaiting_approval", info.awaiting_approval},
                {"approval_id", info.approval_id},
                {"approval_title", info.approval_title},
                {"approval_detail", info.approval_detail},
            });
    }
    const uint64_t process_id = CurrentProcessId();
    const nlohmann::json document = {
        {"schema_version", 1},
        {"process_id", process_id},
        {"updated_at", UnixTimeSeconds()},
        {"runs", std::move(runs)},
    };
    const std::string path =
        JoinPath(directory, "process-" + std::to_string(process_id) + ".json");
    std::string write_error;
    WriteFileUtf8Atomic(path, document.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), &write_error);
}

Result<ActiveSelection> ExternalAgentManager::ResolveSelection(const std::string& provider,
                                                               const std::string& model,
                                                               const std::string& reasoning_effort) const {
    ActiveSelection selection = config_.active;
    const bool explicit_effort = !Trim(reasoning_effort).empty();
    if (!Trim(provider).empty()) selection.provider = Trim(provider);
    if (!Trim(model).empty()) selection.model = Trim(model);
    if (explicit_effort) selection.reasoning_effort = Trim(reasoning_effort);

    if (!selection.provider.empty() && selection.model.empty()) {
        const ProviderConfig* selected_provider = FindProvider(config_, selection.provider);
        if (selected_provider != nullptr && !selected_provider->models.empty()) {
            selection.model = selected_provider->models.front().id;
        }
    }
    if (selection.provider.empty() || selection.model.empty()) {
        for (const ProviderConfig& candidate : config_.providers) {
            if (candidate.models.empty()) continue;
            selection.provider = candidate.id;
            selection.model = candidate.models.front().id;
            break;
        }
    }
    const ProviderConfig* resolved_provider = FindProvider(config_, selection.provider);
    if (resolved_provider == nullptr) return Result<ActiveSelection>::Failure("unknown provider: " + selection.provider);
    const ModelConfig* resolved_model = FindModel(*resolved_provider, selection.model);
    if (resolved_model == nullptr) {
        return Result<ActiveSelection>::Failure("provider " + selection.provider + " has no model " + selection.model);
    }
    const std::vector<std::string> efforts = EffectiveReasoningEfforts(*resolved_model);
    if (!explicit_effort &&
        (selection.provider != config_.active.provider || selection.model != config_.active.model)) {
        selection.reasoning_effort.clear();
    }
    if (!explicit_effort && (selection.reasoning_effort.empty() ||
        std::find(efforts.begin(), efforts.end(), selection.reasoning_effort) == efforts.end())) {
        selection.reasoning_effort = EffectiveDefaultReasoningEffort(*resolved_model);
    }
    if ((explicit_effort || !selection.reasoning_effort.empty()) &&
        std::find(efforts.begin(), efforts.end(), selection.reasoning_effort) == efforts.end()) {
        return Result<ActiveSelection>::Failure("reasoning_effort is not supported by the selected model");
    }
    return Result<ActiveSelection>::Success(std::move(selection));
}

ExternalAgentInfo ExternalAgentManager::Snapshot(const std::shared_ptr<Run>& run) const {
    std::lock_guard<std::mutex> lock(run->mutex);
    return run->info;
}

std::shared_ptr<ExternalAgentManager::Run> ExternalAgentManager::Find(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = runs_.find(id);
    return found == runs_.end() ? nullptr : found->second;
}

Result<void> ExternalAgentManager::StartTask(const std::shared_ptr<Run>& run, const std::string& task) {
    {
        std::lock_guard<std::mutex> lock(run->mutex);
        if (run->running) return Result<void>::Failure("agent is already running");
        if (!run->accepting) return Result<void>::Failure("agent is no longer accepting work");
        run->running = true;
        run->info.state = "running";
        run->info.output.clear();
        run->info.reasoning.clear();
        run->info.phase = "正在启动...";
        run->info.active_tool_name.clear();
        run->info.active_tool_arguments.clear();
        run->info.error.clear();
        run->info.awaiting_approval = false;
        run->info.approval_kind.clear();
        run->info.approval_title.clear();
        run->info.approval_detail.clear();
        run->info.output_complete = false;
        run->info.started_at = UnixTimeSeconds();
        run->info.finished_at = 0;
    }
    // A completed std::thread remains joinable until join() is called. Reassigning
    // a joinable thread would call std::terminate, so reap the previous turn first.
    if (run->worker.joinable()) run->worker.join();
    run->cancellation.Reset();
    run->worker = std::thread([this, run, task] {
        try {
        AppConfig session_config = config_;
        ExternalAgentInfo info;
        {
            std::lock_guard<std::mutex> lock(run->mutex);
            info = run->info;
        }
        session_config.active.provider = info.provider;
        session_config.active.model = info.model;
        session_config.active.reasoning_effort = info.reasoning_effort;
        session_config.workspace.path = info.workspace;
        if (info.permission_mode != session_config.permissions.default_mode) {
            ApplyPermissionMode(&session_config, info.permission_mode);
        }

        const ApprovalCallback approval = [run](const ApprovalRequest& request) {
            std::lock_guard<std::mutex> approval_lock(run->approval_mutex);
            std::unique_lock<std::mutex> lock(run->mutex);
            if (run->cancellation.IsCancelled()) return false;
            run->approval_decided = false;
            run->approval_granted = false;
            run->pending_approval = request;
            run->info.awaiting_approval = true;
            run->info.approval_kind = ApprovalKindToString(request.kind);
            run->info.approval_title = Bounded(request.title, 1000);
            run->info.approval_detail = Bounded(request.detail, 8000);
            run->info.state = "awaiting_approval";
            ++run->info.approval_id;
            lock.unlock();
            run->condition.notify_all();
            lock.lock();
            run->condition.wait(lock, [&] {
                return run->approval_decided || run->cancellation.IsCancelled();
            });
            const bool approved = run->approval_decided && run->approval_granted &&
                                  !run->cancellation.IsCancelled();
            run->info.awaiting_approval = false;
            run->info.approval_kind.clear();
            run->info.approval_title.clear();
            run->info.approval_detail.clear();
            if (!run->cancellation.IsCancelled()) run->info.state = "running";
            lock.unlock();
            run->condition.notify_all();
            return approved;
        };
        McpManager* session_mcp = mcp_.get();
        if (session_config.permissions.allow_network && !config_.permissions.allow_network &&
            std::any_of(session_config.mcp_servers.begin(), session_config.mcp_servers.end(),
                        [](const McpServerConfig& server) { return server.enabled && server.transport == "streamable_http"; })) {
            if (!run->permission_mcp) {
                run->permission_mcp = std::make_unique<McpManager>(session_config);
                const auto discovered = run->permission_mcp->DiscoverEnabled(&run->cancellation);
                if (!discovered.ok) {
                    run->permission_mcp.reset();
                    throw std::runtime_error("MCP discovery failed: " + discovered.error);
                }
            }
            session_mcp = run->permission_mcp.get();
        }
        Agent agent(session_config, &run->journal, session_mcp, nullptr);
        const uint64_t turn_started_tick = MonotonicMillis();
        uint64_t usage_input_tokens = 0;
        uint64_t usage_output_tokens = 0;
        uint64_t usage_cached_input_tokens = 0;
        uint64_t usage_model_calls = 0;
        uint64_t usage_last_input_tokens = 0;
        uint64_t usage_last_output_tokens = 0;
        uint64_t usage_context_tokens = 0;
        uint64_t usage_context_limit_tokens = 0;
        bool usage_context_published = false;
        AgentCallbacks callbacks;
        callbacks.on_phase = [run](AgentPhase phase, const std::string& detail) {
            std::lock_guard<std::mutex> lock(run->mutex);
            switch (phase) {
                case AgentPhase::WaitingModel:
                    run->info.phase = detail == "thinking" ? "正在思考..." : "正在等待模型响应...";
                    break;
                case AgentPhase::Streaming:
                    run->info.phase = "正在输出...";
                    break;
                case AgentPhase::Compacting:
                    run->info.phase = "正在压缩上下文...";
                    break;
                case AgentPhase::RunningTool:
                    run->info.phase = detail.empty() ? "正在运行工具..." : "正在运行 " + detail;
                    break;
                case AgentPhase::WaitingApproval:
                    run->info.phase = "等待审批...";
                    break;
                case AgentPhase::Cancelling:
                    run->info.phase = "正在停止...";
                    break;
                case AgentPhase::Completed:
                    run->info.phase = "已完成";
                    break;
            }
        };
        callbacks.on_text_delta = [run](const std::string& delta) {
            std::lock_guard<std::mutex> lock(run->mutex);
            run->info.output = Bounded(run->info.output + delta, kMaxOutputBytes);
            run->info.phase = "正在输出...";
        };
        callbacks.on_reasoning_delta = [run](const std::string& delta) {
            std::lock_guard<std::mutex> lock(run->mutex);
            run->info.reasoning = Bounded(run->info.reasoning + delta, kMaxOutputBytes);
            run->info.phase = "正在思考...";
        };
        callbacks.on_tool_start = [run](const std::string& call_id, const std::string& name,
                                        const std::string& arguments) {
            std::lock_guard<std::mutex> lock(run->mutex);
            run->info.phase = "正在运行 " + name;
            run->info.active_tool_name = name;
            run->info.active_tool_arguments = arguments;
        };
        callbacks.on_tool_finish = [run](const std::string& call_id, const std::string& name,
                                         bool success, const std::string& output) {
            std::lock_guard<std::mutex> lock(run->mutex);
            run->info.active_tool_name.clear();
            run->info.active_tool_arguments.clear();
            run->info.phase = success ? "继续处理..." : "工具执行失败";
        };
        callbacks.on_model_usage = [&](int input_tokens, int output_tokens, int cached_input_tokens) {
            usage_input_tokens += static_cast<uint64_t>(std::max(0, input_tokens));
            usage_output_tokens += static_cast<uint64_t>(std::max(0, output_tokens));
            usage_cached_input_tokens += static_cast<uint64_t>(std::max(0, cached_input_tokens));
            usage_last_input_tokens = static_cast<uint64_t>(std::max(0, input_tokens));
            usage_last_output_tokens = static_cast<uint64_t>(std::max(0, output_tokens));
            ++usage_model_calls;
        };
        callbacks.on_context_updated = [&](uint64_t used, uint64_t limit) {
            usage_context_tokens = used;
            usage_context_limit_tokens = limit;
            usage_context_published = true;
        };
        callbacks.on_error = [run](const std::string& error) {
            std::lock_guard<std::mutex> lock(run->mutex);
            run->info.error = Bounded(error, 16000);
        };
        const auto result = agent.RunTurn(task, approval, callbacks, &run->cancellation, false);
        {
            std::lock_guard<std::mutex> lock(run->mutex);
            run->running = false;
            run->info.finished_at = UnixTimeSeconds();
            if (run->cancellation.IsCancelled()) {
                run->info.state = "cancelled";
            } else if (result.ok) {
                run->info.state = "completed";
            } else {
                run->info.state = "failed";
                if (run->info.error.empty()) run->info.error = result.error;
            }
            run->info.awaiting_approval = false;
            run->info.approval_kind.clear();
            run->info.approval_title.clear();
            run->info.approval_detail.clear();
            run->info.output_complete = true;
            run->info.phase = "已完成";
            run->info.active_tool_name.clear();
            run->info.active_tool_arguments.clear();
        }
        const uint64_t elapsed_ms = MonotonicMillis() - turn_started_tick;
        const size_t user_turns = static_cast<size_t>(
            std::count_if(run->journal.State().messages.begin(), run->journal.State().messages.end(),
                          [](const ChatMessage& message) { return message.role == MessageRole::User; }));
        const size_t turn_index = user_turns == 0 ? 0 : user_turns - 1;
        run->journal.Append("turn.duration", {
            {"turn_index", turn_index},
            {"elapsed_ms", elapsed_ms},
            {"success", result.ok},
            {"reason", result.ok ? std::string{} : result.error},
        });
        if (usage_model_calls > 0) {
            if (!usage_context_published) {
                const ProviderConfig* provider = FindProvider(session_config, session_config.active.provider);
                const ModelConfig* model = provider == nullptr ? nullptr
                                                               : FindModel(*provider, session_config.active.model);
                usage_context_tokens = EstimateMessagesTokens(ModelContextMessages(run->journal.State()));
                usage_context_limit_tokens =
                    model == nullptr ? static_cast<uint64_t>(session_config.context.fallback_context_window)
                                     : EffectiveContextLimit(session_config.context, *model);
            }
            if (usage_context_tokens == 0) {
                usage_context_tokens = usage_last_input_tokens + usage_last_output_tokens;
            }
            run->journal.Append("turn.usage", {
                {"turn_index", turn_index},
                {"input_tokens", usage_input_tokens},
                {"output_tokens", usage_output_tokens},
                {"cached_input_tokens", usage_cached_input_tokens},
                {"model_calls", usage_model_calls},
                {"context_tokens", usage_context_tokens},
                {"context_limit_tokens", usage_context_limit_tokens},
                {"elapsed_ms", elapsed_ms},
            });
        }
        run->condition.notify_all();
        } catch (const std::exception& exception) {
            {
                std::lock_guard<std::mutex> lock(run->mutex);
                run->running = false;
                run->info.state = "failed";
                run->info.error = std::string("external agent worker failed: ") + exception.what();
                run->info.finished_at = UnixTimeSeconds();
                run->info.output_complete = true;
            }
            run->condition.notify_all();
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(run->mutex);
                run->running = false;
                run->info.state = "failed";
                run->info.error = "external agent worker failed: unknown exception";
                run->info.finished_at = UnixTimeSeconds();
                run->info.output_complete = true;
            }
            run->condition.notify_all();
        }
    });
    return Result<void>::Success();
}

Result<ExternalAgentInfo> ExternalAgentManager::Create(const std::string& task, const std::string& name,
                                                       const std::string& provider, const std::string& model,
                                                       const std::string& reasoning_effort,
                                                       const std::string& workspace,
                                                       const std::string& permission_mode) {
    if (Trim(task).empty()) return Result<ExternalAgentInfo>::Failure("task must not be empty");
    if (task.size() > kMaxTaskBytes) return Result<ExternalAgentInfo>::Failure("task is too large");
    const auto selection = ResolveSelection(provider, model, reasoning_effort);
    if (!selection.ok) return Result<ExternalAgentInfo>::Failure(selection.error);

    std::string target_workspace = Trim(workspace).empty() ? config_.workspace.path : Trim(workspace);
    target_workspace = ResolveConfigPath(target_workspace, config_path_);
    target_workspace = AbsolutePath(target_workspace, config_path_.empty() ? GetExecutableDirectory()
                                                                          : ParentPath(config_path_));
    std::error_code directory_error;
    spark::fs::create_directories(NativePath(target_workspace), directory_error);
    if (directory_error) return Result<ExternalAgentInfo>::Failure("could not create workspace: " + directory_error.message());

    auto run = std::make_shared<Run>();
    const auto opened = run->journal.Open(config_.storage.data_dir, target_workspace);
    if (!opened.ok) return Result<ExternalAgentInfo>::Failure("could not create session: " + opened.error);
    run->info.id = NewSessionId();
    run->info.name = ResolveExternalAgentTitle(name, task);
    run->info.state = "queued";
    run->info.workspace = target_workspace;
    run->info.session_path = run->journal.Path();
    run->info.provider = selection.value.provider;
    run->info.model = selection.value.model;
    run->info.reasoning_effort = selection.value.reasoning_effort;
    run->info.permission_mode = NormalizePermissionMode(permission_mode);
    run->info.scope = ResolveExternalAgentScope(config_, config_path_, target_workspace);
    run->info.started_at = UnixTimeSeconds();
    run->journal.Append("session.title", {{"title", run->info.name}});
    run->journal.Append("session.scope", {{"scope", run->info.scope}});
    run->journal.Append("session.permission", {{"mode", run->info.permission_mode}});

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (runs_.size() >= kMaxExternalRuns) return Result<ExternalAgentInfo>::Failure("too many external agent sessions");
        size_t active = 0;
        for (const auto& entry : runs_) {
            std::lock_guard<std::mutex> run_lock(entry.second->mutex);
            if (entry.second->running) ++active;
        }
        if (active >= kMaxConcurrentRuns) {
            std::error_code cleanup_error;
            spark::fs::remove(NativePath(run->journal.Path()), cleanup_error);
            return Result<ExternalAgentInfo>::Failure("too many external agents are running");
        }
        runs_[run->info.id] = run;
    }
    const auto started = StartTask(run, task);
    if (!started.ok) return Result<ExternalAgentInfo>::Failure(started.error);
    const ExternalAgentInfo snapshot = Snapshot(run);
    return Result<ExternalAgentInfo>::Success(snapshot);
}

Result<void> ExternalAgentManager::Send(const std::string& id, const std::string& message,
                                        const std::string& provider, const std::string& model,
                                        const std::string& reasoning_effort) {
    if (Trim(message).empty()) return Result<void>::Failure("message must not be empty");
    if (message.size() > kMaxTaskBytes) return Result<void>::Failure("message is too large");
    const std::shared_ptr<Run> run = Find(id);
    if (run == nullptr) return Result<void>::Failure("unknown agent id");
    const auto selection = ResolveSelection(provider, model, reasoning_effort);
    if (!selection.ok) return Result<void>::Failure(selection.error);
    {
        std::lock_guard<std::mutex> lock(run->mutex);
        if (run->running) return Result<void>::Failure("agent is already running");
        run->info.provider = selection.value.provider;
        run->info.model = selection.value.model;
        run->info.reasoning_effort = selection.value.reasoning_effort;
    }
    return StartTask(run, message);
}

Result<ExternalAgentInfo> ExternalAgentManager::Wait(const std::string& id, int timeout_ms) {
    const std::shared_ptr<Run> run = Find(id);
    if (run == nullptr) return Result<ExternalAgentInfo>::Failure("unknown agent id");
    std::unique_lock<std::mutex> lock(run->mutex);
    if (timeout_ms <= 0) return Result<ExternalAgentInfo>::Success(run->info);
    run->condition.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
        return run->info.awaiting_approval || !run->running;
    });
    return Result<ExternalAgentInfo>::Success(run->info);
}

Result<ExternalAgentInfo> ExternalAgentManager::Get(const std::string& id) const {
    const std::shared_ptr<Run> run = Find(id);
    if (run == nullptr) return Result<ExternalAgentInfo>::Failure("unknown agent id");
    return Result<ExternalAgentInfo>::Success(Snapshot(run));
}

std::vector<ExternalAgentInfo> ExternalAgentManager::List() const {
    std::vector<std::shared_ptr<Run>> runs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : runs_) runs.push_back(entry.second);
    }
    std::vector<ExternalAgentInfo> output;
    for (const auto& run : runs) output.push_back(Snapshot(run));
    std::sort(output.begin(), output.end(), [](const ExternalAgentInfo& left, const ExternalAgentInfo& right) {
        return left.started_at > right.started_at;
    });
    return output;
}

Result<void> ExternalAgentManager::Cancel(const std::string& id) {
    const std::shared_ptr<Run> run = Find(id);
    if (run == nullptr) return Result<void>::Failure("unknown agent id");
    run->cancellation.Cancel();
    {
        std::lock_guard<std::mutex> lock(run->mutex);
        if (run->running) run->info.state = "cancelling";
    }
    run->condition.notify_all();
    WriteRegistry();
    return Result<void>::Success();
}

Result<void> ExternalAgentManager::Approve(const std::string& id, bool approved) {
    const std::shared_ptr<Run> run = Find(id);
    if (run == nullptr) return Result<void>::Failure("unknown agent id");
    {
        std::lock_guard<std::mutex> lock(run->mutex);
        if (!run->info.awaiting_approval || run->approval_decided) {
            return Result<void>::Failure("agent is not waiting for approval");
        }
        run->approval_granted = approved;
        run->approval_decided = true;
        run->info.awaiting_approval = false;
        run->info.approval_kind.clear();
        run->info.approval_title.clear();
        run->info.approval_detail.clear();
        run->info.state = "running";
    }
    run->condition.notify_all();
    WriteRegistry();
    return Result<void>::Success();
}

std::string ExternalAgentInfoJson(const ExternalAgentInfo& info) {
    return nlohmann::json({{"id", info.id},
                           {"name", info.name},
                           {"state", info.state},
                           {"workspace", info.workspace},
                           {"session_path", info.session_path},
                           {"provider", info.provider},
                           {"model", info.model},
                           {"reasoning_effort", info.reasoning_effort},
                           {"permission_mode", info.permission_mode},
                           {"scope", info.scope},
                           {"output", info.output},
                           {"reasoning", info.reasoning},
                           {"phase", info.phase},
                           {"active_tool_name", info.active_tool_name},
                           {"active_tool_arguments", info.active_tool_arguments},
                           {"error", info.error},
                           {"awaiting_approval", info.awaiting_approval},
                           {"approval_kind", info.approval_kind},
                           {"approval_title", info.approval_title},
                           {"approval_detail", info.approval_detail},
                           {"output_complete", info.output_complete},
                           {"started_at", info.started_at},
                           {"finished_at", info.finished_at}})
        .dump(2);
}

std::string ResolveExternalAgentTitle(const std::string& requested_name, const std::string& task) {
    const std::string name = Trim(requested_name);
    if (!name.empty()) return TruncateUtf8(name, 80);
    return TitleFromTask(task);
}

}  // namespace spark
