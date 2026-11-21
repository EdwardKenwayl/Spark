#include "loopback_http.h"
#include "core/agent.h"

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
    TestLoop("default beyond 100", 105, Ending::Complete, false);
    TestLoop("legacy max_iterations=1", 4, Ending::Complete, true);
    TestLoop("cancel beyond 100", 105, Ending::Cancel, false);
    TestLoop("provider error beyond 100", 105, Ending::ProviderError, false);
    return failures == 0 ? 0 : 1;
}
