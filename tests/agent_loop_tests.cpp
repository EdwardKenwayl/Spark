#include "loopback_http.h"
#include "core/agent.h"
#include "core/context_compactor.h"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <sstream>

namespace {
using namespace spark;
using Server = test::LoopbackHttpServer;
int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

enum class Ending { Complete, Cancel, ProviderError };

void TestPromptCachePrefix(std::string protocol) {
    const bool responses = protocol == "responses";
    const auto root = fs::temp_directory_path() / Utf8ToWide("spark-cache-prefix-" + NewSessionId());
    const std::string workspace = PathToUtf8(root);
    const std::string skill_path = JoinPath(workspace, "fixture-skill/SKILL.md");
    fs::create_directories(root / "fixture-skill");
    Check(WriteFileUtf8Atomic(skill_path,
          "---\nname: fixture-skill\ndescription: A fixture skill.\n---\nUnique skill body.\n"),
          "cache fixture skill is created");

    Server server([&](Server::Connection& connection, size_t) {
        const std::string body = responses
            ? R"({"status":"completed","output":[{"type":"message","content":[{"type":"output_text","text":"Done."}]}]})"
            : R"({"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"Done."}}]})";
        connection.Send(Server::Headers(200, body.size(), "Content-Type: application/json\r\n") + body);
    }, 4);
    AppConfig config;
    config.workspace.path = workspace;
    config.storage.data_dir = JoinPath(workspace, "data");
    config.skills.enabled = true;
    config.skills.include_workspace_root = false;
    config.skills.include_user_root = false;
    config.skills.roots = {JoinPath(workspace, "fixture-skill")};
    ProviderConfig provider;
    provider.id = "fixture";
    provider.protocol = protocol;
    provider.base_url = server.Url("/v1");
    provider.api_key = "synthetic-fixture";
    ModelConfig model;
    model.id = "fixture-model";
    model.capabilities.streaming = false;
    provider.models.push_back(model);
    config.providers.push_back(provider);
    config.active = {provider.id, model.id, ""};

    SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, workspace).ok, "cache fixture session opens");
    Agent agent(config, &journal, nullptr);
    Check(agent.RunTurn("First request.", {}, {}, nullptr).ok, "cache fixture initial request completes");
    Check(agent.RunTurn("Use $fixture-skill.", {}, {}, nullptr).ok, "cache fixture skill request completes");

    SessionJournal restored;
    Check(restored.Resume(journal.Path()).ok, "cache fixture session reloads");
    Agent resumed(config, &restored, nullptr);
    Check(resumed.RunTurn("", {}, {}, nullptr, true).ok, "cache fixture continuation completes");
    Check(resumed.RunTurn("Next request.", {}, {}, nullptr).ok, "cache fixture next user request completes");

    const auto requests = server.Requests();
    Check(requests.size() == 4 && server.Error().empty(), "all cache fixture requests reached the loopback server");
    if (requests.size() == 4) {
        const char* items_key = responses ? "input" : "messages";
        auto previous = nlohmann::json::parse(requests.front().body);
        for (size_t index = 1; index < requests.size(); ++index) {
            const auto current = nlohmann::json::parse(requests[index].body);
            if (responses) {
                Check(current["instructions"] == previous["instructions"] &&
                      current["prompt_cache_key"] == previous["prompt_cache_key"],
                      "Responses instructions and routing key are stable across turns and restart");
            }
            Check(current["tools"] == previous["tools"], "tool definitions are stable");
            Check(current[items_key].size() > previous[items_key].size() &&
                  std::equal(previous[items_key].begin(), previous[items_key].end(), current[items_key].begin()),
                  "every previously sent item remains an exact prefix after skill activation and resume");
            Check(requests[index].body.find("Unique skill body.") != std::string::npos,
                  "invoked skill context remains at its original position after reload");
            previous = current;
        }
    }
    Check(journal.State().messages.size() > 2 && journal.State().messages[2].content == "Use $fixture-skill.",
          "skill expansion does not rewrite the user's visible message");
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

void TestCompactionUserBudget() {
    std::vector<ChatMessage> history;
    for (size_t index = 0; index < 40; ++index) {
        ChatMessage user;
        user.content = std::to_string(index) + ":" + std::string(2000, 'a');
        history.push_back(std::move(user));
    }
    ChatMessage call;
    call.role = MessageRole::Assistant;
    call.tool_calls.push_back({"budget-call", "read_file", R"({"path":"fixture.txt"})"});
    ChatMessage output;
    output.role = MessageRole::Tool;
    output.name = "read_file";
    output.tool_call_id = "budget-call";
    output.content = "fixture";
    history.push_back(call);
    history.push_back(output);
    const auto compacted = BuildCompactedHistory(history, "Work remains.");
    size_t bytes = 0, count = 0;
    bool newest = false;
    for (const auto& message : compacted) {
        if (message.role != MessageRole::User) continue;
        bytes += message.content.size();
        ++count;
        newest = newest || message.content == history[39].content;
    }
    Check(newest && count > 1 && count <= 16 && bytes <= 16000,
          "compaction keeps recent user instructions within a shared byte and message budget");
    Check(nlohmann::json(BuildCompactedHistory(compacted, "Work remains.")) == nlohmann::json(compacted),
          "repeated compaction does not accumulate duplicate user messages");
    history[39].content = std::string(32000, 'z');
    const auto oversized = BuildCompactedHistory(history, "Work remains.");
    const auto user = std::find_if(oversized.begin(), oversized.end(), [](const ChatMessage& item) {
        return item.role == MessageRole::User;
    });
    Check(user != oversized.end() && user->content == std::string(16000, 'z'),
          "a large latest instruction is retained with a bounded UTF-8-safe prefix");
}

void TestCompactionReplayAndContinue() {
    const auto root = fs::temp_directory_path() / Utf8ToWide("spark-compact-replay-" + NewSessionId());
    fs::create_directories(root);
    Check(WriteFileUtf8Atomic(JoinPath(PathToUtf8(root), "fixture.txt"), "fixture"),
          "compaction continuation input is created");
    std::atomic<bool> replaying{false};
    size_t replay_requests = 0;
    Server server([&](Server::Connection& connection, size_t) {
        nlohmann::json output = nlohmann::json::array();
        const bool resume = replaying.load();
        if (resume && replay_requests++ == 0) {
            output.push_back({{"type", "function_call"}, {"call_id", "resumed-read"}, {"name", "read_file"},
                              {"arguments", R"({"path":"fixture.txt"})"}});
        } else {
            output.push_back({{"type", "message"}, {"role", "assistant"},
                {"content", nlohmann::json::array({{{"type", "output_text"},
                    {"text", resume ? "Done: fixture." : "Work remains. Continue the file read."}}})}});
        }
        const std::string body = nlohmann::json{{"status", "completed"}, {"output", output}}.dump();
        connection.Send(Server::Headers(200, body.size(), "Content-Type: application/json\r\n") + body);
    }, 8);
    AppConfig config;
    config.workspace.path = PathToUtf8(root);
    config.storage.data_dir = JoinPath(config.workspace.path, "data");
    config.skills.enabled = false;
    config.agent.request_timeout_seconds = 3;
    ProviderConfig provider;
    provider.id = "fixture";
    provider.protocol = "responses";
    provider.base_url = server.Url("/v1");
    provider.api_key = "synthetic-fixture";
    ModelConfig model;
    model.id = "fixture-model";
    model.capabilities.context_window = 4096;
    model.capabilities.streaming = false;
    provider.models.push_back(model);
    config.providers.push_back(provider);
    config.active = {provider.id, model.id, ""};
    SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, config.workspace.path).ok, "compaction session opens");
    ChatMessage earlier;
    for (size_t index = 0; index < 4096; ++index) earlier.content += "\xE4\xB8\xAD";
    ChatMessage goal;
    goal.content = "Read fixture.txt and finish the entire task.";
    ChatMessage steering;
    steering.content = "Continue until complete; a progress report does not finish this task.";
    ChatMessage call;
    call.role = MessageRole::Assistant;
    call.tool_calls.push_back({"seed-read", "read_file", R"({"path":"fixture.txt"})"});
    call.reasoning_items.push_back(
        R"({"type":"reasoning","id":"seed-reasoning","encrypted_content":"fixture"})");
    ChatMessage output;
    output.role = MessageRole::Tool;
    output.name = "read_file";
    output.tool_call_id = "seed-read";
    output.content = "fixture";
    for (const auto& message : {earlier, goal, steering, call, output}) {
        journal.MutableState().messages.push_back(message);
        Check(journal.Append("message", message).ok, "seed message persists");
    }
    Agent agent(config, &journal, nullptr);
    const auto approve = [](const ApprovalRequest&) { return true; };
    Check(agent.RunTurn("/compact", approve, {}, nullptr).ok, "first real compaction succeeds");
    const auto first = journal.State().context_messages;
    Check(first.size() == 6 && !first[1].content.empty() &&
              first[1].content.size() < earlier.content.size() &&
              earlier.content.find(first[1].content) == 0 && EstimateMessagesTokens(first) < 4096,
          "small-window compaction bounds retained user text while preserving a UTF-8 prefix");
    Check(agent.RunTurn("/compact", approve, {}, nullptr).ok, "second real compaction succeeds");
    const nlohmann::json expected = std::vector<ChatMessage>{
        journal.State().context_messages.front(), first[1], goal, steering, call, output};
    Check(nlohmann::json(first) == expected && nlohmann::json(journal.State().context_messages) == expected,
          "both compactions preserve original instructions and the complete reasoning/tool group once");
    const auto contents = ReadFileUtf8(journal.Path());
    std::istringstream records(contents.value_or(""));
    std::string line, without_snapshots;
    while (std::getline(records, line)) {
        const auto event = nlohmann::json::parse(line);
        if (event.value("type", "") != "snapshot") without_snapshots += line + "\n";
    }
    const auto replay_path = JoinPath(config.storage.data_dir, "sessions/replayed.jsonl");
    Check(WriteFileUtf8Atomic(replay_path, without_snapshots), "journal-only replay fixture is created");
    SessionJournal replayed;
    Check(replayed.Resume(replay_path).ok, "journal without snapshots resumes");
    Check(nlohmann::json(replayed.State().context_messages) == expected,
          "compaction event alone restores the exact replacement context");
    // The small window above tests retention; use a normal window for the tool-loop replay.
    config.providers.front().models.front().capabilities.context_window = 272000;
    Agent resumed(config, &replayed, nullptr);
    const size_t summary_requests = server.Requests().size();
    replaying.store(true);
    size_t tools = 0;
    AgentCallbacks callbacks;
    callbacks.on_tool_finish = [&](const auto&, const auto&, bool ok, const auto&) {
        Check(ok, "tool after journal-only restore succeeds"); ++tools;
    };
    const auto result = resumed.RunTurn("Continue.", approve, callbacks, nullptr);
    const auto requests = server.Requests();
    Check(result.ok && tools == 1 && requests.size() == summary_requests + 2 && server.Error().empty(),
          "a twice-compacted journal resumes the actual provider/tool loop");
    if (requests.size() == summary_requests + 2) {
        const auto body = nlohmann::json::parse(requests[summary_requests].body);
        Check(requests[summary_requests].body.find(goal.content) != std::string::npos &&
              requests[summary_requests].body.find(steering.content) != std::string::npos &&
              requests[summary_requests].body.find("seed-reasoning") != std::string::npos &&
              !body["tools"].empty() && replayed.State().messages.back().content == "Done: fixture.",
              "the continued request contains retained instructions, reasoning and tools");
    }
    std::error_code error;
    fs::remove_all(root, error);
}

void TestResponsesContinuation(bool streaming, int end_turn) {
    const bool follow_up = end_turn == 0;
    const auto root = fs::temp_directory_path() / Utf8ToWide("spark-follow-up-" + NewSessionId());
    fs::create_directories(root);
    Check(WriteFileUtf8Atomic(JoinPath(PathToUtf8(root), "fixture.txt"), "fixture"),
          "continuation tool input is created");
    Server server([&](Server::Connection& connection, size_t attempt) {
        nlohmann::json output = nlohmann::json::array();
        auto message = [](const char* phase, const char* text) {
            return nlohmann::json{{"type", "message"}, {"role", "assistant"}, {"phase", phase},
                {"content", nlohmann::json::array({{{"type", "output_text"}, {"text", text}}})}};
        };
        if (attempt == 0) {
            output.push_back(message("commentary", "Checking the requested file."));
            output.push_back(message("commentary", "The task is still in progress."));
        } else if (attempt == 1) {
            output.push_back({{"type", "reasoning"}, {"id", "reasoning-fixture"},
                {"encrypted_content", "fixture-encrypted-reasoning"}});
            output.push_back(message("commentary", "I will read the file now."));
            output.push_back(message("commentary", "Reading fixture.txt."));
            output.push_back({{"type", "function_call"}, {"call_id", "read-fixture"},
                {"name", "read_file"}, {"arguments", R"({"path":"fixture.txt"})"}});
        } else {
            output.push_back(message("final_answer", "Done. The file says fixture."));
        }
        nlohmann::json response = {{"status", "completed"}, {"output", output}};
        if (attempt != 0 || end_turn >= 0) response["end_turn"] = attempt == 0 ? !follow_up : attempt > 1;
        std::string body;
        if (streaming) {
            for (size_t index = 0; index < output.size(); ++index) {
                body += "data: " + nlohmann::json{{"type", "response.output_item.done"},
                    {"output_index", index}, {"item", output[index]}}.dump() + "\n\n";
            }
            body += "data: " + nlohmann::json{{"type", "response.completed"},
                {"response", response}}.dump() + "\n\n";
        } else {
            body = response.dump();
        }
        connection.Send(Server::Headers(200, body.size(), streaming
            ? "Content-Type: text/event-stream\r\n" : "Content-Type: application/json\r\n") + body);
    }, follow_up ? 3 : 1);
    AppConfig config;
    config.workspace.path = PathToUtf8(root);
    config.storage.data_dir = JoinPath(config.workspace.path, "data");
    config.skills.enabled = false;
    ProviderConfig provider;
    provider.id = "fixture";
    provider.protocol = "responses";
    provider.base_url = server.Url("/v1");
    provider.api_key = "synthetic-fixture";
    ModelConfig model;
    model.id = "fixture-model";
    model.capabilities.streaming = streaming;
    provider.models.push_back(model);
    config.providers.push_back(provider);
    config.active = {provider.id, model.id, ""};
    SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, config.workspace.path).ok, "continuation session opens");
    Agent agent(config, &journal, nullptr);
    int finished = 0, tools = 0;
    std::string visible;
    AgentCallbacks callbacks;
    callbacks.on_text_delta = [&](const std::string& delta) { visible += delta; };
    callbacks.on_tool_finish = [&](const auto&, const auto&, bool ok, const auto&) {
        Check(ok, "continuation tool succeeds"); ++tools;
    };
    callbacks.on_turn_finished = [&] { ++finished; };
    const auto result = agent.RunTurn("Read fixture.txt and report its content.",
        [](const ApprovalRequest&) { return true; }, callbacks, nullptr);
    if (!result.ok) std::cerr << "continuation: " << result.error << '\n';
    const auto requests = server.Requests();
    Check(result.ok && finished == 1 && tools == (follow_up ? 1 : 0),
          "explicit end_turn=false continues; true or absent completes even with commentary");
    Check(requests.size() == (follow_up ? 3 : 1), "continuation makes only the required requests");
    Check(visible.find("Checking the requested file.") != std::string::npos &&
              visible.find("The task is still in progress.") != std::string::npos,
          "all intermediate messages remain visible");
    if (follow_up && requests.size() == 3) {
        const auto payload = nlohmann::json::parse(requests[1].body);
        Check(payload["input"].size() >= 3 &&
                  payload["input"][1].value("phase", "") == "commentary" &&
                  payload["input"][2].value("phase", "") == "commentary",
              "separate commentary messages retain their phase on the next request");
        Check(journal.State().messages.back().content == "Done. The file says fixture.",
              "only the completed work becomes the final response");
        const auto compacted = BuildCompactedHistory(journal.State().messages, "Read fixture.txt.");
        const auto calls = std::find_if(compacted.begin(), compacted.end(), [](const ChatMessage& item) {
            return !item.tool_calls.empty();
        });
        Check(calls != compacted.end() && calls->reasoning_items.size() == 1 &&
                  nlohmann::json::parse(calls->reasoning_items.front()).value("id", "") == "reasoning-fixture",
              "compaction retains reasoning required by a multi-message response's tool calls");
        SessionJournal restored;
        Check(restored.Resume(journal.Path()).ok, "continuation journal reloads");
        const nlohmann::json original = journal.State().messages;
        const nlohmann::json replayed = restored.State().messages;
        Check(replayed == original, "reloading preserves message boundaries, phases and tool results");
    }
    std::error_code error;
    fs::remove_all(root, error);
}

void TestLoop(const std::string& label, size_t tool_rounds, Ending ending, bool legacy_config) {
    const auto root = fs::temp_directory_path() / Utf8ToWide("spark-loop-" + NewSessionId());
    fs::create_directories(root);
    std::string write_error;
    Check(WriteFileUtf8Atomic(JoinPath(PathToUtf8(root), "fixture.txt"), "fixture", &write_error),
          "tool input is created");
    Server server([&](Server::Connection& connection, size_t attempt) {
        if (attempt == tool_rounds && ending == Ending::ProviderError) {
            const std::string body = R"({"error":{"message":"fixture provider rejected request"}})";
            connection.Send(Server::Headers(400, body.size(), "Content-Type: application/json\r\n") + body);
            return;
        }
        const bool tool = attempt < tool_rounds;
        nlohmann::json delta;
        if (tool) {
            delta["tool_calls"] = nlohmann::json::array({
                {{"index", 0}, {"id", "call-" + std::to_string(attempt)}, {"type", "function"},
                 {"function", {{"name", "read_file"}, {"arguments", R"({"path":"fixture.txt"})"}}}}
            });
        } else {
            delta["content"] = "finished all requested tools";
        }
        const nlohmann::json event = {{"choices", nlohmann::json::array({
            {{"delta", delta}, {"finish_reason", tool ? "tool_calls" : "stop"}}
        })}};
        const std::string body = "data: " + event.dump() + "\n\ndata: [DONE]\n\n";
        connection.Send(Server::Headers(200, body.size(), "Content-Type: text/event-stream\r\n") + body);
    }, tool_rounds + 1);

    AppConfig config;
    config.workspace.path = PathToUtf8(root);
    config.storage.data_dir = JoinPath(config.workspace.path, "data");
    config.skills.enabled = false;
    config.agent.request_timeout_seconds = 3;
    ProviderConfig provider;
    provider.id = "fixture";
    provider.protocol = "chat_completions";
    provider.base_url = server.Url("/v1");
    provider.api_key = "synthetic-fixture";
    ModelConfig model;
    model.id = "loop-model";
    model.capabilities.context_window = 1000000;
    provider.models.push_back(model);
    config.providers.push_back(provider);
    config.active = {provider.id, model.id, ""};
    if (legacy_config) {
        auto document = ConfigToJson(config);
        document["agent"]["max_iterations"] = 1;
        config = ConfigFromJson(document, JoinPath(config.workspace.path, "config.json"));
        Check(ValidateConfig(config, false).empty(), "legacy configuration remains valid");
    }
    SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, config.workspace.path).ok, "session opens");
    Agent agent(config, &journal, nullptr);
    CancellationToken cancellation;
    size_t tools = 0;
    int finished = 0;
    int errors = 0;
    AgentCallbacks callbacks;
    callbacks.on_tool_finish = [&](const std::string&, const std::string&, bool ok, const std::string&) {
        Check(ok, "tool succeeds");
        if (++tools == tool_rounds && ending == Ending::Cancel) cancellation.Cancel();
    };
    callbacks.on_turn_finished = [&] { ++finished; };
    callbacks.on_error = [&](const std::string&) { ++errors; };
    const auto result = agent.RunTurn("read the fixture for every requested tool round",
        [](const ApprovalRequest&) { return true; }, callbacks, &cancellation);
    const auto requests = server.Requests();
    if (!server.Error().empty()) std::cerr << "server: " << server.Error() << '\n';
    if (!result.ok) std::cerr << label << ": " << result.error << '\n';
    size_t results = 0;
    for (const auto& message : journal.State().messages) {
        if (message.role == MessageRole::Tool) ++results;
    }
    Check(server.Error().empty(), "fixture server reports no error");
    Check(tools == tool_rounds && results == tool_rounds, "every requested tool has a persisted result");
    if (ending == Ending::Complete) {
        Check(result.ok && finished == 1 && errors == 0, "only the final answer completes the turn");
        Check(!journal.State().messages.empty() &&
                  journal.State().messages.back().content == "finished all requested tools",
              "final response is preserved");
    } else if (ending == Ending::Cancel) {
        Check(!result.ok && result.error == "cancelled" && finished == 0,
              "cancel still interrupts a long tool loop");
    } else {
        Check(!result.ok && errors == 1 && finished == 0,
              "provider failure still stops a long tool loop as an error");
    }
    Check(requests.size() == tool_rounds + (ending == Ending::Cancel ? 0 : 1),
          "requests stop at completion, cancellation or provider error");
    for (const auto& request : requests) {
        const auto body = nlohmann::json::parse(request.body);
        Check(body.contains("tools") && !body["tools"].empty(), "no forced no-tools finalization is inserted");
    }
    std::cout << label << ": tools=" << tools << " results=" << results
              << " requests=" << requests.size() << " completed=" << finished
              << " errors=" << errors << " ok=" << result.ok << '\n';
    std::error_code error;
    fs::remove_all(root, error);
}
}  // namespace

int main() {
    TestPromptCachePrefix("responses");
    TestPromptCachePrefix("chat_completions");
    TestCompactionUserBudget();
    TestCompactionReplayAndContinue();
    for (bool streaming : {false, true})
        for (int end_turn : {-1, 0, 1}) TestResponsesContinuation(streaming, end_turn);
    TestLoop("default beyond 100", 105, Ending::Complete, false);
    TestLoop("legacy max_iterations=1", 4, Ending::Complete, true);
    TestLoop("cancel beyond 100", 105, Ending::Cancel, false);
    TestLoop("provider error beyond 100", 105, Ending::ProviderError, false);
    return failures == 0 ? 0 : 1;
}
