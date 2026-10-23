#include "gui/native/runtime.h"
#include "platform/clock.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace spark {

struct NativeRuntime::Run {
    SessionJournal journal;
    AppConfig config;
    CancellationToken cancellation;
    std::thread worker;
    std::atomic<bool> done{false};
    uint64_t started = 0, previous_elapsed = 0;
    std::unique_ptr<McpManager> mcp;
    std::unique_ptr<SubAgentManager> children;
    std::mutex approval_mutex;
    std::condition_variable approval_changed;
    Approval approval;
    bool pending = false, presented = false, approved = false, approve_turn = false;
    ~Run() {
        cancellation.Cancel();
        approval_changed.notify_all();
        if (worker.joinable()) worker.join();
    }
};

NativeRuntime::~NativeRuntime() { Stop(); }

void NativeRuntime::Emit(NativeEvent event) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t bytes = event.first.size() + event.second.size() + event.third.size();
    // Merge adjacent deltas rather than allocating one UI event per token.
    if (!events_.empty() && (event.kind == NativeEvent::Kind::Text || event.kind == NativeEvent::Kind::Reasoning)) {
        auto& previous = events_.back();
        if (previous.path == event.path && previous.kind == event.kind && previous.first.size() < 64 * 1024) {
            previous.first += event.first;
            event_bytes_ += bytes;
            return;
        }
    }
    // Full authoritative history is retained in the journal even if a hidden
    // window cannot consume live previews quickly enough.
    if (event_bytes_ > 4 * 1024 * 1024 &&
        (event.kind == NativeEvent::Kind::Text || event.kind == NativeEvent::Kind::Reasoning)) return;
    event_bytes_ += bytes;
    events_.push_back(std::move(event));
}

Result<void> NativeRuntime::Start(const AppConfig& config, const SessionJournal& journal,
                                  const std::string& text, bool resume,
                                  const std::vector<std::string>& images, AgentTurnOptions options) {
    if (Running(journal.Path())) return Result<void>::Failure("This conversation is already running.");
    size_t running = 0;
    for (const auto& item : runs_) if (!item.second->done.load()) ++running;
    if (running >= 4) return Result<void>::Failure("Four conversations are already running.");
    auto run = std::make_shared<Run>();
    run->journal = journal;
    run->config = config;
    run->config.workspace.path = journal.State().workspace;
    run->started = MonotonicMillis();
    const auto& metadata = journal.State().metadata;
    if (resume && metadata.contains("paused_turn") && metadata["paused_turn"].is_object())
        run->previous_elapsed = metadata["paused_turn"].value("elapsed_ms", uint64_t{0});
    const std::string path = journal.Path();
    auto previous = runs_.find(path);
    if (previous != runs_.end() && previous->second->children->HasRunning())
        return Result<void>::Failure("A child agent from this conversation is still running.");
    if (previous != runs_.end()) {
        if (previous->second->worker.joinable()) previous->second->worker.join();
        runs_.erase(previous);
    }
    SubAgentCallbacks children;
    const auto notify_child = [this, path](const SubAgentInfo& info) {
        NativeEvent event;
        event.kind = NativeEvent::Kind::Child;
        event.path = path;
        event.child = info;
        Emit(std::move(event));
    };
    children.on_created = notify_child;
    children.on_finished = [notify_child](const SubAgentInfo& info, bool, const std::string&) { notify_child(info); };
    children.on_phase = [notify_child](const SubAgentInfo& info, AgentPhase, const std::string&) { notify_child(info); };
    run->children = std::make_unique<SubAgentManager>(std::move(children));
    run->mcp = std::make_unique<McpManager>(run->config);
    runs_[path] = run;
    run->worker = std::thread([this, run, path, text, images, options, resume] {
        uint64_t input = 0, output = 0, cached = 0, calls = 0, context = 0, limit = 0;
        const auto event = [&](NativeEvent::Kind kind, std::string first = {}, std::string second = {},
                                std::string third = {}, bool success = false) {
            NativeEvent item;
            item.path = path;
            item.kind = kind;
            item.first = std::move(first);
            item.second = std::move(second);
            item.third = std::move(third);
            item.success = success;
            Emit(std::move(item));
        };
        const ApprovalCallback approve = [this, run, path](const ApprovalRequest& request) {
            uint64_t id;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                id = ++next_approval_;
            }
            std::unique_lock<std::mutex> lock(run->approval_mutex);
            if (run->cancellation.IsCancelled()) return false;
            if (run->approve_turn) return true;
            run->approval.id = id;
            run->approval.path = path;
            run->approval.request = request;
            run->pending = true;
            run->presented = false;
            run->approved = false;
            while (run->pending && !run->cancellation.IsCancelled())
                run->approval_changed.wait_for(lock, std::chrono::milliseconds(25));
            run->pending = false;
            return run->approved && !run->cancellation.IsCancelled();
        };
        Result<void> outcome;
        try {
            run->journal.Append("session.permission", {{"mode", run->config.permissions.default_mode}});
            const auto discovered = run->mcp->DiscoverEnabled(&run->cancellation);
            if (!discovered.ok) {
                event(NativeEvent::Kind::Phase, discovered.error);
                const bool required = std::any_of(run->config.mcp_servers.begin(), run->config.mcp_servers.end(),
                                                  [](const McpServerConfig& server) { return server.enabled && server.required; });
                if (required) throw std::runtime_error(discovered.error);
            }
            AgentCallbacks callbacks;
            callbacks.on_text_delta = [&](const std::string& delta) { event(NativeEvent::Kind::Text, delta); };
            callbacks.on_reasoning_delta = [&](const std::string& delta) { event(NativeEvent::Kind::Reasoning, delta); };
            callbacks.on_tool_start = [&](const std::string& id, const std::string& name, const std::string& arguments) {
                event(NativeEvent::Kind::ToolStart, id, name, arguments);
            };
            callbacks.on_tool_finish = [&](const std::string& id, const std::string& name, bool success,
                                            const std::string& result) {
                event(NativeEvent::Kind::ToolFinish, id, name, result, success);
            };
            callbacks.on_phase = [&](AgentPhase phase, const std::string& detail) {
                NativeEvent item;
                item.kind = NativeEvent::Kind::Phase;
                item.path = path;
                item.first = detail;
                item.phase = phase;
                Emit(std::move(item));
            };
            callbacks.on_context_updated = [&](uint64_t used, uint64_t maximum) {
                context = used; limit = maximum;
                NativeEvent item;
                item.kind = NativeEvent::Kind::Context;
                item.path = path;
                item.first_number = used;
                item.second_number = maximum;
                Emit(std::move(item));
            };
            callbacks.on_context_compacted = [&](uint64_t, uint64_t, const std::string& trigger) {
                event(NativeEvent::Kind::Compact, {}, {}, {}, trigger != "manual");
            };
            callbacks.on_model_usage = [&](int in, int out, int hit) {
                input += std::max(0, in); output += std::max(0, out); cached += std::max(0, hit); ++calls;
            };
            Agent agent(run->config, &run->journal, run->mcp.get(), run->children.get(), options);
            outcome = agent.RunTurn(text, approve, callbacks, &run->cancellation, resume, images);
        } catch (const std::exception& exception) {
            outcome = Result<void>::Failure(exception.what());
        }
        const uint64_t elapsed = MonotonicMillis() - run->started + run->previous_elapsed;
        if (run->cancellation.IsCancelled()) {
            ChatMessage marker;
            marker.role = MessageRole::System;
            marker.content = kInterruptedTurnMarker;
            marker.timestamp = UnixTimeSeconds();
            run->journal.MutableState().messages.push_back(marker);
            if (!run->journal.State().context_messages.empty())
                run->journal.MutableState().context_messages.push_back(marker);
            run->journal.Append("message", marker);
            run->journal.MutableState().metadata["paused_turn"] = {{"elapsed_ms", elapsed}};
        } else {
            run->journal.MutableState().metadata.erase("paused_turn");
        }
        run->journal.Append("turn.duration", {{"elapsed_ms", elapsed}});
        run->journal.Append("turn.usage", {{"input_tokens", input}, {"output_tokens", output},
            {"cached_input_tokens", cached}, {"model_calls", calls}, {"context_tokens", context},
            {"context_limit_tokens", limit}, {"elapsed_ms", elapsed}});
        if (!outcome.ok && !run->cancellation.IsCancelled()) run->journal.Append("turn.error", {{"error", outcome.error}});
        run->journal.SaveSnapshot();
        run->done.store(true, std::memory_order_release);
        event(NativeEvent::Kind::Done, outcome.error, {}, {}, outcome.ok);
    });
    return Result<void>::Success();
}

void NativeRuntime::Pause(const std::string& path) {
    const auto run = runs_.find(path);
    if (run != runs_.end()) {
        run->second->cancellation.Cancel();
        run->second->approval_changed.notify_all();
        run->second->children->CancelAll();
    }
}

bool NativeRuntime::Running(const std::string& path) const {
    const auto run = runs_.find(path);
    return run != runs_.end() && !run->second->done.load(std::memory_order_acquire);
}
bool NativeRuntime::AnyRunning() const {
    for (const auto& run : runs_) if (!run.second->done.load()) return true;
    return false;
}
uint64_t NativeRuntime::Started(const std::string& path) const {
    const auto run = runs_.find(path);
    return run == runs_.end() ? 0 : run->second->started;
}
std::vector<NativeEvent> NativeRuntime::Poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NativeEvent> output;
    while (!events_.empty()) { output.push_back(std::move(events_.front())); events_.pop_front(); }
    event_bytes_ = 0;
    return output;
}
std::vector<NativeRuntime::Approval> NativeRuntime::Approvals() {
    std::vector<Approval> result;
    for (const auto& run : runs_) {
        std::lock_guard<std::mutex> lock(run.second->approval_mutex);
        if (run.second->pending && !run.second->presented) {
            result.push_back(run.second->approval);
            run.second->presented = true;
        }
    }
    return result;
}
void NativeRuntime::Approve(uint64_t id, NativeApprovalDecision decision) {
    for (const auto& run : runs_) {
        std::lock_guard<std::mutex> lock(run.second->approval_mutex);
        if (run.second->pending && run.second->approval.id == id) {
            run.second->approved = decision != NativeApprovalDecision::Deny &&
                                   !run.second->cancellation.IsCancelled();
            if (run.second->approved && decision == NativeApprovalDecision::Turn)
                run.second->approve_turn = true;
            run.second->pending = false;
            run.second->approval_changed.notify_all();
        }
    }
}
Result<SessionJournal> NativeRuntime::FinishedJournal(const std::string& path) {
    const auto run = runs_.find(path);
    if (run == runs_.end() || !run->second->done.load(std::memory_order_acquire))
        return Result<SessionJournal>::Failure("conversation is still running");
    if (run->second->worker.joinable()) run->second->worker.join();
    SessionJournal journal = run->second->journal;
    if (!run->second->children->HasRunning()) runs_.erase(run);
    return Result<SessionJournal>::Success(std::move(journal));
}
std::vector<SubAgentInfo> NativeRuntime::Children(const std::string& path) const {
    const auto run = runs_.find(path);
    return run == runs_.end() ? std::vector<SubAgentInfo>{} :
                               run->second->children->List(NormalizePathForComparison(path));
}
void NativeRuntime::Stop() {
    for (const auto& run : runs_) {
        run.second->cancellation.Cancel();
        run.second->approval_changed.notify_all();
        run.second->children->CancelAll();
    }
    for (const auto& run : runs_) if (run.second->worker.joinable()) run.second->worker.join();
    runs_.clear();
}

}  // namespace spark
