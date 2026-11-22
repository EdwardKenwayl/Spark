#include "loopback_http.h"
#include "core/agent.h"
#include "core/context_compactor.h"

#include <algorithm>
#include <iostream>

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
    for (bool streaming : {false, true})
        for (int end_turn : {-1, 0, 1}) TestResponsesContinuation(streaming, end_turn);
    TestLoop("default beyond 100", 105, Ending::Complete, false);
    TestLoop("legacy max_iterations=1", 4, Ending::Complete, true);
    TestLoop("cancel beyond 100", 105, Ending::Cancel, false);
    TestLoop("provider error beyond 100", 105, Ending::ProviderError, false);
    return failures == 0 ? 0 : 1;
}
