#include "core/subagents.h"

#include "core/agent.h"
#include "core/model_catalog.h"
#include "platform/clock.h"

#include <algorithm>
#include <chrono>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace spark {
namespace {

constexpr int kMaxChildAgents = 4;
constexpr size_t kMaxChildTaskBytes = 16000;
constexpr size_t kMaxChildOutputBytes = 48000;
constexpr size_t kMaxChildHistoryRecords = 64;
constexpr size_t kMaxChildHistoryBytes = 256 * 1024;

// Serialize read-modify-write publication across managers in this process.
std::mutex history_mutex;

std::string ChildId() { return "subagent-" + NewSessionId(); }

std::string ChildWorkspaceRoot(const std::string& data_directory, const std::string& id) {
    return JoinPath(JoinPath(data_directory, "subagents"), id);
}

std::string Bounded(std::string value, size_t limit) {
    if (value.size() <= limit) return value;
    return TruncateUtf8(std::move(value), limit) + "\n[truncated]";
}

std::string ChildHistoryPath(const std::string& data_directory, const std::string& parent_session_key) {
    // Stable filename hashing keeps parent journal paths out of path components.
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : parent_session_key) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return JoinPath(JoinPath(data_directory, "subagents"), "history-" + std::to_string(hash) + ".json");
}

nlohmann::json ChildMetadata(const SubAgentInfo& info) {
    return {{"id", info.id},
            {"parent_session_key", info.parent_session_key},
            {"name", TruncateUtf8(info.name, 120)},
            {"task", TruncateUtf8(info.task, 1024)},
            {"session_path", info.session_path},
            {"provider", TruncateUtf8(info.provider, 256)},
            {"model", TruncateUtf8(info.model, 256)},
            {"reasoning_effort", TruncateUtf8(info.reasoning_effort, 64)},
            {"state", SubAgentStateToString(info.state)},
            {"started_at", info.started_at},
            {"finished_at", info.finished_at},
            {"started_tick_ms", info.started_tick_ms},
            {"elapsed_ms", info.elapsed_ms},
            {"error", TruncateUtf8(info.error, 1024)}};
}

void PersistChildMetadata(const std::string& data_directory, const SubAgentInfo& info) noexcept {
    try {
        if (info.parent_session_key.size() > 4096 || info.session_path.size() > 4096) return;
        std::lock_guard<std::mutex> lock(history_mutex);
        auto history = SubAgentManager::LoadHistory(data_directory, info.parent_session_key);
        history.erase(std::remove_if(history.begin(), history.end(), [&](const SubAgentInfo& previous) {
            return previous.id == info.id;
        }), history.end());
        history.push_back(info);
        if (history.size() > kMaxChildHistoryRecords) history.erase(history.begin());
        nlohmann::json document = {{"version", 1}, {"parent_session_key", info.parent_session_key},
                                   {"children", nlohmann::json::array()}};
        for (const auto& child : history) document["children"].push_back(ChildMetadata(child));
        std::string contents = document.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        while (contents.size() > kMaxChildHistoryBytes && document["children"].size() > 1) {
            document["children"].erase(document["children"].begin());
            contents = document.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        }
        if (contents.size() <= kMaxChildHistoryBytes) {
            WriteFileUtf8Atomic(ChildHistoryPath(data_directory, info.parent_session_key), contents);
        }
    } catch (...) {
        // Metadata publication must not prevent a child from finishing.
    }
}

}  // namespace

std::string SubAgentStateToString(SubAgentState state) {
    switch (state) {
        case SubAgentState::Running: return "running";
        case SubAgentState::Completed: return "completed";
        case SubAgentState::Failed: return "failed";
        case SubAgentState::Cancelled: return "cancelled";
    }
    return "running";
}

SubAgentManager::SubAgentManager(SubAgentCallbacks callbacks) : callbacks_(std::move(callbacks)) {}

SubAgentManager::~SubAgentManager() {
    CancelAll();
    std::map<std::string, std::shared_ptr<Run>> runs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        runs = runs_;
    }
    for (const auto& entry : runs) {
        std::lock_guard<std::mutex> lock(entry.second->worker_mutex);
        if (entry.second->worker.joinable()) entry.second->worker.join();
    }
}

SubAgentInfo SubAgentManager::Snapshot(const std::shared_ptr<Run>& run) const {
    std::lock_guard<std::mutex> lock(run->mutex);
    SubAgentInfo info = run->info;
    if (info.state == SubAgentState::Running) info.elapsed_ms = MonotonicMillis() - info.started_tick_ms;
    return info;
}

std::vector<SubAgentInfo> SubAgentManager::LoadHistory(const std::string& data_directory,
                                                     const std::string& parent_session_key) {
    std::vector<SubAgentInfo> history;
    if (parent_session_key.size() > 4096) return history;
    try {
        const auto path = NativePath(ChildHistoryPath(data_directory, parent_session_key));
        std::string contents;
        {
#ifdef _WIN32
            // Share deletion so atomic publication can replace a history being read.
            HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) return history;
            std::unique_ptr<void, decltype(&CloseHandle)> file(handle, &CloseHandle);
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 ||
                size.QuadPart > static_cast<LONGLONG>(kMaxChildHistoryBytes)) {
                return history;
            }
            contents.resize(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            if (!ReadFile(file.get(), contents.data(), static_cast<DWORD>(contents.size()), &read, nullptr) ||
                read != contents.size()) return history;
#else
            const auto retry_interrupted = [](auto operation) {
                auto result = operation();
                for (int attempt = 0; result < 0 && errno == EINTR && attempt < 63; ++attempt) result = operation();
                return result;
            };
            struct HistoryFile {
                int descriptor;
                ~HistoryFile() { if (descriptor >= 0) ::close(descriptor); }
            };
            const HistoryFile file{retry_interrupted([&] {
                return ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
            })};
            if (file.descriptor < 0) return history;
            struct stat size{};
            if (retry_interrupted([&] { return ::fstat(file.descriptor, &size); }) != 0 ||
                !S_ISREG(size.st_mode) || size.st_size <= 0 ||
                static_cast<std::uintmax_t>(size.st_size) > kMaxChildHistoryBytes) return history;
            contents.resize(static_cast<size_t>(size.st_size));
            size_t offset = 0;
            while (offset < contents.size()) {
                const auto read = retry_interrupted([&] {
                    return ::read(file.descriptor, contents.data() + offset, contents.size() - offset);
                });
                if (read <= 0) return history;
                offset += static_cast<size_t>(read);
            }
#endif
        }
        const auto document = nlohmann::json::parse(contents, nullptr, false);
        if (!document.is_object() || document.value("version", 0) != 1 ||
            document.value("parent_session_key", std::string{}) != parent_session_key ||
            !document.contains("children") || !document["children"].is_array()) return history;
        const auto& children = document["children"];
        const size_t begin = children.size() > kMaxChildHistoryRecords
                                 ? children.size() - kMaxChildHistoryRecords : 0;
        for (size_t index = begin; index < children.size(); ++index) {
            try {
                const auto& record = children[index];
                if (!record.is_object()) continue;
                SubAgentInfo info;
                info.id = record.value("id", std::string{});
                info.parent_session_key = record.value("parent_session_key", std::string{});
                info.session_path = record.value("session_path", std::string{});
                if (info.id.empty() || info.id.size() > 128 || info.parent_session_key != parent_session_key ||
                    info.session_path.empty() || info.session_path.size() > 4096) continue;
                const std::string state = record.value("state", std::string{});
                if (state == "running") info.state = SubAgentState::Running;
                else if (state == "completed") info.state = SubAgentState::Completed;
                else if (state == "failed") info.state = SubAgentState::Failed;
                else if (state == "cancelled") info.state = SubAgentState::Cancelled;
                else continue;
                info.name = TruncateUtf8(record.value("name", std::string{}), 120);
                info.task = TruncateUtf8(record.value("task", std::string{}), 1024);
                info.provider = TruncateUtf8(record.value("provider", std::string{}), 256);
                info.model = TruncateUtf8(record.value("model", std::string{}), 256);
                info.reasoning_effort = TruncateUtf8(record.value("reasoning_effort", std::string{}), 64);
                info.error = TruncateUtf8(record.value("error", std::string{}), 1024);
                bool valid_timing = true;
                for (const char* field : {"started_at", "finished_at", "started_tick_ms", "elapsed_ms"}) {
                    const auto value = record.find(field);
                    if (value != record.end() && (!value->is_number_integer() ||
                        (!value->is_number_unsigned() && value->get<int64_t>() < 0))) {
                        valid_timing = false;
                        break;
                    }
                }
                if (!valid_timing) continue;
                info.started_at = record.value("started_at", uint64_t{0});
                info.finished_at = record.value("finished_at", uint64_t{0});
                info.started_tick_ms = record.value("started_tick_ms", uint64_t{0});
                info.elapsed_ms = record.value("elapsed_ms", uint64_t{0});
                history.push_back(std::move(info));
            } catch (const nlohmann::json::exception&) {
                // One malformed record must not hide other children.
            }
        }
    } catch (...) {
        // Missing or malformed history is optional UI metadata.
    }
    return history;
}

void SubAgentManager::EmitCreated(const std::shared_ptr<Run>& run) {
    if (callbacks_.on_created) callbacks_.on_created(Snapshot(run));
}

void SubAgentManager::EmitPhase(const std::shared_ptr<Run>& run, AgentPhase phase, const std::string& detail) {
    if (callbacks_.on_phase) callbacks_.on_phase(Snapshot(run), phase, detail);
}

void SubAgentManager::EmitText(const std::shared_ptr<Run>& run, const std::string& delta) {
    if (callbacks_.on_text_delta) callbacks_.on_text_delta(Snapshot(run), delta);
}

void SubAgentManager::EmitFinished(const std::shared_ptr<Run>& run, bool success, const std::string& reason) {
    if (callbacks_.on_finished) callbacks_.on_finished(Snapshot(run), success, reason);
}

void SubAgentManager::EmitError(const std::shared_ptr<Run>& run, const std::string& error) {
    if (callbacks_.on_error) callbacks_.on_error(Snapshot(run), error);
}

Result<std::string> SubAgentManager::Spawn(const AppConfig& config, McpManager* mcp,
                                           const std::string& data_directory,
                                           const std::string& parent_session_key, const std::string& workspace,
                                           const std::string& name, const std::string& task,
                                           const ApprovalCallback& approval,
                                           CancellationToken* parent_cancellation, const ActiveSelection& selection) {
    if (Trim(task).empty()) return Result<std::string>::Failure("spawn_agent requires a non-empty task");
    if (task.size() > kMaxChildTaskBytes) return Result<std::string>::Failure("spawn_agent task is too long");
    if (parent_cancellation != nullptr && parent_cancellation->IsCancelled()) {
        return Result<std::string>::Failure("cancelled");
    }

    const auto resolved = ResolveSubAgentSelection(config, selection);
    if (!resolved.ok) return Result<std::string>::Failure(resolved.error);
    AppConfig child_config = config;
    child_config.active = resolved.value;
    child_config.workspace.path = workspace;
    auto run = std::make_shared<Run>();
    run->info.id = ChildId();
    run->info.parent_session_key = parent_session_key;
    run->info.name = Trim(name).empty() ? "子智能体" : Bounded(Trim(name), 120);
    run->info.task = task;
    run->info.started_at = UnixTimeSeconds();
    run->info.provider = child_config.active.provider;
    run->info.model = child_config.active.model;
    run->info.reasoning_effort = child_config.active.reasoning_effort;
    run->info.started_tick_ms = MonotonicMillis();
    const std::string root = ChildWorkspaceRoot(data_directory, run->info.id);
    const auto opened = run->journal.Open(root, workspace);
    if (!opened.ok) return Result<std::string>::Failure("could not create child session: " + opened.error);
    run->info.session_path = run->journal.Path();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t active = 0;
        for (const auto& entry : runs_) {
            std::lock_guard<std::mutex> run_lock(entry.second->mutex);
            if (!entry.second->done) ++active;
        }
        if (active >= kMaxChildAgents) return Result<std::string>::Failure("too many child agents are running");
        runs_[run->info.id] = run;
    }
    try {
        PersistChildMetadata(data_directory, run->info);
        EmitCreated(run);
        std::lock_guard<std::mutex> worker_lock(run->worker_mutex);
        run->worker = std::thread([this, run, config = std::move(child_config), mcp, approval, task, data_directory] {
            Result<void> result;
            bool worker_exception = false;
            uint64_t total_input = 0, total_output = 0, total_cached = 0, model_calls = 0;
            uint64_t context_tokens = 0, context_limit = 0;
            try {
                Agent child(config, &run->journal, mcp, nullptr);
                std::map<std::string, ToolCall> active_calls;
                AgentCallbacks callbacks;
                callbacks.on_phase = [this, run](AgentPhase phase, const std::string& detail) {
                    EmitPhase(run, phase, detail);
                };
                callbacks.on_text_delta = [this, run](const std::string& delta) {
                    {
                        std::lock_guard<std::mutex> lock(run->mutex);
                        run->info.output = Bounded(run->info.output + delta, kMaxChildOutputBytes);
                    }
                    EmitText(run, delta);
                };
                callbacks.on_reasoning_delta = [this, run](const std::string& delta) {
                    if (callbacks_.on_reasoning_delta) callbacks_.on_reasoning_delta(Snapshot(run), delta);
                };
                callbacks.on_tool_start = [this, run, &active_calls](const std::string& id, const std::string& name,
                                                                    const std::string& arguments) {
                    ToolCall call{id, name, arguments};
                    active_calls[id] = call;
                    if (callbacks_.on_tool_started) callbacks_.on_tool_started(Snapshot(run), call);
                };
                callbacks.on_tool_finish = [this, run, &active_calls](const std::string& id, const std::string& name,
                                                                     bool ok, const std::string& output) {
                    ToolCall call{id, name, {}};
                    const auto found = active_calls.find(id);
                    if (found != active_calls.end()) {
                        call.arguments_json = std::move(found->second.arguments_json);
                        active_calls.erase(found);
                    }
                    if (callbacks_.on_tool_finished) callbacks_.on_tool_finished(Snapshot(run), call, {ok, output});
                };
                callbacks.on_model_usage = [this, run, &total_input, &total_output, &total_cached, &model_calls]
                                              (int input_tokens, int output_tokens, int cached_input_tokens) {
                    total_input += std::max(0, input_tokens);
                    total_output += std::max(0, output_tokens);
                    total_cached += std::max(0, cached_input_tokens);
                    ++model_calls;
                    if (callbacks_.on_usage) {
                        callbacks_.on_usage(Snapshot(run), {input_tokens, output_tokens, cached_input_tokens});
                    }
                };
                callbacks.on_context_updated = [&](uint64_t used, uint64_t limit) {
                    context_tokens = used;
                    context_limit = limit;
                };
                callbacks.on_error = [this, run](const std::string& error) { EmitError(run, error); };
                result = child.RunTurn(task, approval, callbacks, &run->cancellation, false);
            } catch (const std::exception& exception) {
                result = Result<void>::Failure(std::string("subagent worker failed: ") + exception.what());
                worker_exception = true;
            } catch (...) {
                result = Result<void>::Failure("subagent worker failed: unknown exception");
                worker_exception = true;
            }
            if (worker_exception) {
                result.error = Bounded(RedactSecrets(result.error, CollectConfiguredSecrets(config)), 8000);
                try {
                    run->journal.Append("turn.error", {{"error", result.error}});
                    EmitError(run, result.error);
                } catch (...) {
                    // Publishing a failed callback must not terminate the worker.
                }
            }
            const bool cancelled = run->cancellation.IsCancelled();
            const uint64_t elapsed = MonotonicMillis() - run->info.started_tick_ms;
            try {
                run->journal.Append("turn.duration", {{"elapsed_ms", elapsed}});
                if (model_calls > 0) {
                    run->journal.Append("turn.usage", {
                        {"input_tokens", total_input}, {"output_tokens", total_output},
                        {"cached_input_tokens", total_cached}, {"model_calls", model_calls},
                        {"context_tokens", context_tokens}, {"context_limit_tokens", context_limit},
                        {"elapsed_ms", elapsed}});
                }
            } catch (...) {
                // UI statistics cannot prevent waiters from receiving completion.
            }
            {
                std::lock_guard<std::mutex> lock(run->mutex);
                run->info.error = cancelled ? "cancelled" : result.error;
                run->info.finished_at = UnixTimeSeconds();
                run->info.elapsed_ms = elapsed;
                run->info.state = cancelled ? SubAgentState::Cancelled
                                            : result.ok ? SubAgentState::Completed : SubAgentState::Failed;
                PersistChildMetadata(data_directory, run->info);
                run->done = true;
            }
            run->condition.notify_all();
            try {
                EmitFinished(run, result.ok && !cancelled, cancelled ? "cancelled" : result.error);
            } catch (...) {
                // Waiters can still read the final state if a UI callback fails.
            }
        });
    } catch (const std::exception& exception) {
        {
            std::lock_guard<std::mutex> lock(run->mutex);
            run->info.error = std::string("could not start child agent: ") + exception.what();
            run->info.state = SubAgentState::Failed;
            run->info.finished_at = UnixTimeSeconds();
            run->info.elapsed_ms = MonotonicMillis() - run->info.started_tick_ms;
            PersistChildMetadata(data_directory, run->info);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        runs_.erase(run->info.id);
        return Result<std::string>::Failure(std::string("could not start child agent: ") + exception.what());
    }
    return Result<std::string>::Success(run->info.id);
}

Result<std::string> SubAgentManager::Wait(const std::string& id, CancellationToken* parent_cancellation) {
    std::shared_ptr<Run> run;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = runs_.find(id);
        if (found == runs_.end()) return Result<std::string>::Failure("unknown subagent: " + id);
        run = found->second;
    }
    for (;;) {
        std::unique_lock<std::mutex> lock(run->mutex);
        if (run->done) break;
        if (parent_cancellation != nullptr && parent_cancellation->IsCancelled()) {
            run->cancellation.Cancel();
            return Result<std::string>::Failure("cancelled while waiting for subagent");
        }
        run->condition.wait_for(lock, std::chrono::milliseconds(100));
    }
    {
        std::lock_guard<std::mutex> lock(run->worker_mutex);
        if (run->worker.joinable() && run->worker.get_id() != std::this_thread::get_id()) run->worker.join();
    }
    const SubAgentInfo info = Snapshot(run);
    std::string output = "subagent_id: " + info.id + "\nstate: " + SubAgentStateToString(info.state) +
                         "\nname: " + info.name + "\nprovider: " + info.provider + "\nmodel: " + info.model +
                         "\nreasoning_effort: " + info.reasoning_effort + "\n\n" + info.output;
    if (!info.error.empty()) output += "\nerror: " + info.error;
    return info.state == SubAgentState::Completed ? Result<std::string>::Success(std::move(output))
                                                   : Result<std::string>::Failure(std::move(output));
}

std::vector<SubAgentInfo> SubAgentManager::List(const std::string& parent_session_key) const {
    std::vector<SubAgentInfo> output;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : runs_) {
        const SubAgentInfo info = Snapshot(entry.second);
        if (info.parent_session_key == parent_session_key) output.push_back(info);
    }
    return output;
}

bool SubAgentManager::HasRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : runs_) {
        std::lock_guard<std::mutex> run_lock(entry.second->mutex);
        if (!entry.second->done) return true;
    }
    return false;
}

void SubAgentManager::CancelAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : runs_) entry.second->cancellation.Cancel();
}

}  // namespace spark
