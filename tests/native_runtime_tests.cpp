#include "loopback_http.h"
#include "gui/native/runtime.h"
#include "platform/clock.h"

#include <iostream>

namespace {
using namespace spark;
using Server = test::LoopbackHttpServer;
int failures = 0;
void Check(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << message << '\n'; }
}
AppConfig Configuration(const std::string& root, const std::string& url) {
    AppConfig config;
    config.storage.data_dir = root;
    config.workspace.path = root;
    config.config_path = JoinPath(root, "config.json");
    config.mcp_servers.clear();
    ProviderConfig provider;
    provider.id = "fixture"; provider.protocol = "chat_completions";
    provider.base_url = url; provider.api_key = "synthetic-fixture";
    ModelConfig model; model.id = "fixture-model";
    provider.models.push_back(model);
    config.providers = {provider};
    config.active.provider = provider.id; config.active.model = model.id;
    config.agent.request_timeout_seconds = 2;
    return config;
}

void TestApprovalScope(const fs::path& root) {
    Server server([](Server::Connection& connection, size_t attempt) {
        nlohmann::json delta;
        const bool tool = attempt == 0 || attempt == 1 || attempt == 3 || attempt == 4 || attempt == 6;
        if (tool) {
            const nlohmann::json arguments = {{"path", "approval-" + std::to_string(attempt) + ".txt"},
                                               {"content", "approved"}};
            delta["tool_calls"] = nlohmann::json::array({
                {{"index", 0}, {"id", "approval-" + std::to_string(attempt)}, {"type", "function"},
                 {"function", {{"name", "write_file"}, {"arguments", arguments.dump()}}}}
            });
        } else delta["content"] = "done";
        const nlohmann::json chunk = {{"choices", nlohmann::json::array({
            {{"delta", delta}, {"finish_reason", tool ? "tool_calls" : "stop"}}})}};
        const std::string body = "data: " + chunk.dump() + "\n\ndata: [DONE]\n\n";
        connection.Send(Server::Headers(200, body.size(), "Content-Type: text/event-stream\r\n") + body);
    });
    auto config = Configuration(PathToUtf8(root), server.Url("/v1"));
    SessionJournal first, second;
    Check(first.Open(config.storage.data_dir, config.workspace.path).ok, "approval session opens");
    Check(second.Open(config.storage.data_dir, config.workspace.path).ok, "separate approval session opens");
    NativeRuntime runtime;
    for (int round = 0; round < 4; ++round) {
        auto& journal = round < 2 ? first : second;
        Check(runtime.Start(config, journal, "write fixture", false, {}).ok, "approval turn starts");
        int prompts = 0;
        const auto deadline = MonotonicMillis() + 5000;
        while (runtime.AnyRunning() && MonotonicMillis() < deadline) {
            for (const auto& approval : runtime.Approvals()) {
                ++prompts;
                Check(approval.path == journal.Path(), "approval stays with its requesting conversation");
                if (round == 3) runtime.Pause(journal.Path());
                runtime.Approve(approval.id, round == 0 ? NativeApprovalDecision::Turn :
                    round == 2 ? NativeApprovalDecision::Once :
                    round == 3 ? NativeApprovalDecision::Turn : NativeApprovalDecision::Deny);
            }
            runtime.Poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Check(!runtime.AnyRunning(), "approval fixture turn terminates");
        if (runtime.AnyRunning()) { runtime.Stop(); return; }
        Check(prompts == 1, "each new turn/conversation requires its own approval");
        const auto finished = runtime.FinishedJournal(journal.Path());
        Check(finished.ok, "approval fixture journal remains readable");
        if (finished.ok) journal = finished.value;
    }
    Check(fs::exists(root / "approval-0.txt") && fs::exists(root / "approval-1.txt"),
          "explicit allow-turn permits both writes in its turn");
    Check(!fs::exists(root / "approval-3.txt"), "a new turn cannot inherit the previous turn's approval");
    Check(fs::exists(root / "approval-4.txt"), "allow-once permits its approved write in another session");
    Check(!fs::exists(root / "approval-6.txt"), "a cancelled run cannot reuse a turn approval");
    runtime.Stop();
}
}  // namespace
int main() {
    using namespace spark;
    const auto root = fs::temp_directory_path() / NativePath("spark-runtime-" + NewSessionId());
    std::error_code error;
    fs::create_directories(root, error);
    std::atomic<bool> release{false};
    const std::string prefix = "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"Checking\",\"content\":\"Hello\"}}]}\n\n";
    const std::string suffix = "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":2}}\n\ndata: [DONE]\n\n";
    Server server([&](Server::Connection& connection, size_t) {
        connection.Send(Server::Headers(200, prefix.size() + suffix.size(), "Content-Type: text/event-stream\r\n") + prefix);
        while (!release.load()) if (!connection.Pause(std::chrono::milliseconds(10))) return;
        connection.Send(suffix);
    });
    AppConfig config = Configuration(PathToUtf8(root), server.Url("/v1"));
    SessionJournal first, second;
    Check(first.Open(config.storage.data_dir, config.workspace.path).ok, "first session opens");
    Check(second.Open(config.storage.data_dir, config.workspace.path).ok, "second session opens");
    NativeRuntime runtime;
    Check(runtime.Start(config, first, "hello", false, {}).ok, "first turn starts");
    Check(server.WaitForRequests(1), "fixture receives first turn");
    bool saw_delta = false;
    const auto deadline = MonotonicMillis() + 3000;
    while (!saw_delta && MonotonicMillis() < deadline) {
        for (const auto& event : runtime.Poll())
            if (event.kind == NativeEvent::Kind::Text) {
                saw_delta = true;
                Check(event.path == first.Path() && event.first == "Hello", "stream belongs to first session");
            }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    SessionJournal live;
    Check(live.Resume(first.Path()).ok && live.State().metadata.contains("live_turn"),
          "running text can be replayed by an external UI before completion");
    Check(saw_delta, "text is delivered before completion");
    Check(runtime.Start(config, second, "interrupt", false, {}).ok, "parallel session starts");
    runtime.Pause(second.Path());
    release.store(true);
    const auto finish_deadline = MonotonicMillis() + 5000;
    while (runtime.AnyRunning() && MonotonicMillis() < finish_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    Check(!runtime.AnyRunning(), "parallel turns terminate");
    const auto done = runtime.FinishedJournal(first.Path());
    const auto paused = runtime.FinishedJournal(second.Path());
    Check(done.ok && !done.value.State().metadata.contains("paused_turn"), "first turn completes independently");
    Check(paused.ok && paused.value.State().metadata.contains("paused_turn"), "second turn retains interruption");
    if (done.ok) {
        Check(done.value.State().metadata.contains("turn_durations_ms"), "completed elapsed time persists");
        Check(!done.value.State().metadata.contains("live_turn"), "completed turn clears live preview");
        SessionJournal replay;
        Check(replay.Resume(done.value.Path()).ok && replay.State().messages.size() == done.value.State().messages.size(),
              "reloaded transcript matches completed state");
        runtime.Poll();
        SessionJournal compacted = done.value;
        for (const bool automatic : {false, true}) {
            AppConfig compact_config = config;
            if (automatic) {
                compact_config.context.auto_compact_threshold_percent = 1;
                compact_config.providers.front().models.front().capabilities.context_window = 10000;
            }
            Check(runtime.Start(compact_config, compacted, automatic ? "continue" : "/compact", false, {}).ok,
                  "compaction turn starts");
            const auto compact_deadline = MonotonicMillis() + 5000;
            while (runtime.AnyRunning() && MonotonicMillis() < compact_deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            Check(!runtime.AnyRunning(), "compaction turn completes");
            bool notice = false;
            for (const auto& event : runtime.Poll()) {
                if (event.kind != NativeEvent::Kind::Compact) continue;
                notice = true;
                Check(event.path == compacted.Path() && event.success == automatic,
                      "live compaction notice retains its session and trigger type");
            }
            Check(notice, "compaction publishes a live notice");
            const auto saved = runtime.FinishedJournal(compacted.Path());
            Check(saved.ok, "compacted session remains available");
            if (saved.ok) compacted = saved.value;
        }
    }
    runtime.Stop();
    fs::create_directories(root / "approvals", error);
    TestApprovalScope(root / "approvals");
    fs::remove_all(root, error);
    return failures ? 1 : 0;
}
