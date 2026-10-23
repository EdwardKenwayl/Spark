#include "core/agent.h"
#include "core/config.h"
#include "core/context_compactor.h"
#include "core/external_agents.h"
#include "core/model_catalog.h"
#include "core/provider.h"
#include "core/session.h"
#include "core/subagents.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <thread>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

struct FakeResponse {
    int status = 200;
    std::string content_type = "text/event-stream";
    std::string body;
    // Milliseconds the fake provider stays silent before sending the response
    // line, used to emulate an endpoint that needs time before its first byte.
    int header_delay_ms = 0;
    std::map<std::string, std::string> headers;
};

class FakeServer {
public:
    FakeServer() {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        bind(listen_socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        listen(listen_socket_, 8);
        int length = sizeof(address);
        getsockname(listen_socket_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { AcceptLoop(); });
    }

    ~FakeServer() {
        stop_ = true;
        if (listen_socket_ != INVALID_SOCKET) closesocket(listen_socket_);
        condition_.notify_all();
        if (thread_.joinable()) thread_.join();
        WSACleanup();
    }

    int Port() const { return port_; }
    void Enqueue(FakeResponse response) {
        std::lock_guard<std::mutex> lock(mutex_);
        responses_.push(std::move(response));
        condition_.notify_all();
    }

    bool WaitForRequests(size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(8), [&] { return requests_.size() >= count || stop_; }) &&
               requests_.size() >= count;
    }

    std::vector<std::string> Requests() {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    void AcceptLoop() {
        for (;;) {
            FakeResponse response;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [&] { return stop_ || !responses_.empty(); });
                if (stop_) return;
                response = std::move(responses_.front());
                responses_.pop();
            }
            sockaddr_in client_address{};
            int length = sizeof(client_address);
            SOCKET client = accept(listen_socket_, reinterpret_cast<sockaddr*>(&client_address), &length);
            if (client == INVALID_SOCKET) {
                if (stop_) return;
                continue;
            }
            std::string request;
            char buffer[8192];
            size_t header_end = std::string::npos;
            size_t content_length = 0;
            for (;;) {
                const int read = recv(client, buffer, sizeof(buffer), 0);
                if (read <= 0) break;
                request.append(buffer, read);
                if (header_end == std::string::npos) {
                    header_end = request.find("\r\n\r\n");
                    if (header_end != std::string::npos) {
                        const std::string headers = request.substr(0, header_end);
                        const std::string marker = "Content-Length:";
                        size_t position = headers.find(marker);
                        if (position != std::string::npos) {
                            position += marker.size();
                            content_length = static_cast<size_t>(std::stoul(headers.substr(position)));
                        }
                    }
                }
                if (header_end != std::string::npos && request.size() >= header_end + 4 + content_length) break;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                requests_.push_back(request);
                condition_.notify_all();
            }
            for (int waited = 0; waited < response.header_delay_ms && !stop_; waited += 25) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            if (stop_) {
                closesocket(client);
                return;
            }
            const std::string status_text = response.status == 200 ? "OK" : (response.status == 401 ? "Unauthorized" : "Error");
            std::string extra_headers;
            for (const auto& [name, value] : response.headers) extra_headers += name + ": " + value + "\r\n";
            const std::string headers = "HTTP/1.1 " + std::to_string(response.status) + " " + status_text +
                                        "\r\nContent-Type: " + response.content_type + "\r\nContent-Length: " +
                                        std::to_string(response.body.size()) + "\r\n" + extra_headers +
                                        "Connection: close\r\n\r\n";
            send(client, headers.data(), static_cast<int>(headers.size()), 0);
            send(client, response.body.data(), static_cast<int>(response.body.size()), 0);
            shutdown(client, SD_SEND);
            closesocket(client);
        }
    }

    SOCKET listen_socket_ = INVALID_SOCKET;
    int port_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<FakeResponse> responses_;
    std::vector<std::string> requests_;
    bool stop_ = false;
};

std::string Sse(const nlohmann::json& value) { return "data: " + value.dump() + "\n\n"; }

std::string TextSse(const std::string& text) {
    nlohmann::json event;
    event["choices"] = nlohmann::json::array();
    event["choices"].push_back({{"delta", {{"content", text}}}, {"finish_reason", "stop"}});
    return Sse(event) + "data: [DONE]\n\n";
}

std::string RequestBody(const std::string& request) {
    const size_t position = request.find("\r\n\r\n");
    return position == std::string::npos ? std::string{} : request.substr(position + 4);
}

spark::AppConfig MakeConfig(int port, const std::string& workspace) {
    spark::AppConfig config;
    config.config_path = spark::JoinPath(workspace, "config.json");
    config.active = {"local", "test-model", ""};
    spark::ProviderConfig provider;
    provider.id = "local";
    provider.label = "Local fake provider";
    provider.protocol = "chat_completions";
    provider.base_url = "http://127.0.0.1:" + std::to_string(port) + "/v1";
    provider.api_key = "primary-secret";
    provider.api_key_expression = "primary-secret";
    provider.models = {{"test-model", "Test Model", {true, true, false, false, {}, "", 8192}}};
    config.providers.push_back(provider);
    config.workspace.path = workspace;
    config.storage.data_dir = spark::JoinPath(workspace, "data");
    config.agent.max_iterations = 8;
    config.agent.request_timeout_seconds = 10;
    config.agent.tool_timeout_seconds = 10;
    return config;
}

std::string TempDirectory(const std::string& suffix) {
    const std::string path = spark::JoinPath(spark::CurrentDirectoryUtf8(), "spark_agent_test_" + suffix);
    std::error_code error;
    spark::fs::remove_all(spark::Utf8ToWide(path), error);
    spark::fs::create_directories(spark::Utf8ToWide(path), error);
    return path;
}

void RemoveDirectory(const std::string& path) {
    std::error_code error;
    spark::fs::remove_all(spark::Utf8ToWide(path), error);
}

void AppendSyntheticMessage(spark::SessionJournal& journal, spark::MessageRole role, const std::string& content,
                            const std::string& name = {}) {
    spark::ChatMessage message;
    message.role = role;
    message.name = name;
    message.content = content;
    journal.MutableState().messages.push_back(message);
    if (!journal.MutableState().context_messages.empty()) {
        journal.MutableState().context_messages.push_back(message);
    }
    Check(journal.Append("message", message).ok, "synthetic context message is journaled");
}

void AppendSyntheticContext(spark::SessionJournal& journal, uint64_t extra_tokens,
                            const std::string& marker, const std::string& first_marker = {}) {
    const std::string filler(4000, 'x');
    uint64_t added = 0;
    int index = 0;
    while (added < extra_tokens) {
        const uint64_t before = spark::EstimateMessagesTokens(journal.State().messages);
        std::string user_prefix = index == 0 && !first_marker.empty() ? first_marker + " " : "";
        AppendSyntheticMessage(journal, spark::MessageRole::User,
                               user_prefix + marker + " user chunk " + std::to_string(index) + "\n" + filler);
        AppendSyntheticMessage(journal, spark::MessageRole::Assistant,
                               marker + " assistant result " + std::to_string(index) + "\n" + filler);
        const uint64_t after = spark::EstimateMessagesTokens(journal.State().messages);
        if (after <= before) break;
        added += after - before;
        ++index;
    }
}

std::vector<nlohmann::json> CompactionEvents(const std::string& journal_path) {
    const auto contents = spark::ReadFileUtf8(journal_path);
    std::vector<nlohmann::json> events;
    if (!contents.has_value()) return events;
    std::istringstream stream(*contents);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        try {
            const nlohmann::json event = nlohmann::json::parse(line);
            if (event.value("type", std::string{}) == "context.compacted") {
                events.push_back(event.value("payload", nlohmann::json::object()));
            }
        } catch (const nlohmann::json::exception&) {
        }
    }
    return events;
}

void TestProviderToolCallStreaming() {
    FakeServer server;
    const std::string workspace = TempDirectory("provider");
    const auto config = MakeConfig(server.Port(), workspace);
    const auto* provider = spark::FindProvider(config, "local");
    const auto* model = spark::FindModel(*provider, "test-model");
    std::string body;
    body += std::string(R"(data: {"id":"resp-1","choices":[{"delta":{"tool_calls":[{"index":0,"id":"call-1","function":{"name":"read_","arguments":"{\"path\":"}}]},"finish_reason":null}]})") + "\n\n";
    body += std::string(R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"name":"file","arguments":"\"README.md\"}"}}]},"finish_reason":"tool_calls"}]})") + "\n\n";
    body += "data: [DONE]\n\n";
    server.Enqueue({200, "text/event-stream", body});
    auto adapter = spark::CreateProviderAdapter("chat_completions");
    spark::ProviderRequest request;
    request.provider = provider;
    request.model = model;
    request.messages = {{{spark::MessageRole::User, "read a file", {}, {}, {}}}};
    const auto response = adapter->Complete(request, {}, nullptr);
    Check(response.ok, "provider streaming tool call succeeds");
    if (response.ok) {
        Check(response.value.tool_calls.size() == 1, "provider accumulates one tool call");
        Check(response.value.tool_calls[0].name == "read_file", "provider accumulates tool name fragments");
        Check(response.value.tool_calls[0].arguments_json == "{\"path\":\"README.md\"}", "provider accumulates tool arguments");
    }
    const auto requests = server.Requests();
    Check(requests.size() == 1 && requests[0].find("Authorization: Bearer primary-secret") != std::string::npos,
          "provider sends named key");
    RemoveDirectory(workspace);
}

void TestResponsesToolCallStreaming() {
    FakeServer server;
    const std::string workspace = TempDirectory("responses_provider");
    const auto config = MakeConfig(server.Port(), workspace);
    const auto* provider = spark::FindProvider(config, "local");
    const auto* model = spark::FindModel(*provider, "test-model");

    nlohmann::json added = {
        {"type", "response.output_item.added"},
        {"item", {{"type", "function_call"}, {"id", "item-1"}, {"call_id", "call-1"}, {"name", "read_file"}, {"arguments", ""}}},
    };
    nlohmann::json arguments_delta = {
        {"type", "response.function_call_arguments.delta"},
        {"item_id", "item-1"},
        {"delta", R"({"path":"README.md"})"},
    };
    nlohmann::json arguments_done = {
        {"type", "response.function_call_arguments.done"},
        {"item_id", "item-1"},
        {"arguments", R"({"path":"README.md"})"},
    };
    nlohmann::json completed = {
        {"type", "response.completed"},
        {"response", {{"id", "resp-1"}, {"status", "completed"}}},
    };
    std::string body;
    for (const nlohmann::json& event : {added, arguments_delta, arguments_done, completed}) {
        body += Sse(event);
    }
    server.Enqueue({200, "text/event-stream", body});

    auto adapter = spark::CreateProviderAdapter("responses");
    spark::ProviderRequest request;
    request.provider = provider;
    request.model = model;
    request.messages = {{{spark::MessageRole::User, "read a file", {}, {}, {}}}};
    request.prompt_cache_key = "session-cache-key";
    const auto response = adapter->Complete(request, {}, nullptr);
    Check(response.ok, "Responses API streaming tool call succeeds");
    if (response.ok) {
        Check(response.value.tool_calls.size() == 1, "Responses API maps item_id to one call_id");
        if (response.value.tool_calls.size() == 1) {
            Check(response.value.tool_calls[0].id == "call-1", "Responses API preserves call_id");
            Check(response.value.tool_calls[0].name == "read_file", "Responses API preserves tool name");
            Check(response.value.tool_calls[0].arguments_json == R"({"path":"README.md"})",
                  "Responses API maps argument deltas to the function call");
        }
    }
    const auto requests = server.Requests();
    Check(requests.size() == 1 && requests[0].find("\"prompt_cache_key\":\"session-cache-key\"") != std::string::npos,
          "Responses API sends the stable prompt cache key");
    RemoveDirectory(workspace);
}

// Providers differ in where they publish a finished answer: some stream
// response.output_text.delta events, others attach the text to the finished
// output item, and some only include it in response.completed. All of them have
// to end up as a usable reply instead of an empty turn.
void TestResponsesFinalOutputFallback() {
    FakeServer server;
    const std::string workspace = TempDirectory("responses_final_output");
    const auto config = MakeConfig(server.Port(), workspace);
    const auto* provider = spark::FindProvider(config, "local");
    const auto* model = spark::FindModel(*provider, "test-model");
    spark::ProviderRequest request;
    request.provider = provider;
    request.model = model;
    request.messages = {{{spark::MessageRole::User, "hello", {}, {}, {}}}};
    auto adapter = spark::CreateProviderAdapter("responses");

    const nlohmann::json reasoning = {{"type", "response.reasoning_text.delta"}, {"delta", "thinking"}};
    const nlohmann::json completed_plain = {
        {"type", "response.completed"},
        {"response", {{"id", "resp-1"}, {"status", "completed"}}},
    };

    nlohmann::json item_content = nlohmann::json::array();
    item_content.push_back({{"type", "output_text"}, {"text", "answer from item"}});
    nlohmann::json item_done;
    item_done["type"] = "response.output_item.done";
    item_done["item"] = {{"type", "message"}, {"id", "msg-1"}, {"content", item_content}};
    std::string item_body;
    for (const nlohmann::json& event : {reasoning, item_done, completed_plain}) item_body += Sse(event);
    server.Enqueue({200, "text/event-stream", item_body});

    const auto item_reply = adapter->Complete(request, {}, nullptr);
    Check(item_reply.ok, "Responses API output item message is accepted");
    if (item_reply.ok) {
        Check(item_reply.value.text == "answer from item",
              "finished output item text is used when no text deltas arrive");
        Check(!item_reply.value.diagnostics.empty(), "successful replies carry HTTP diagnostics");
    }

    nlohmann::json completed_content = nlohmann::json::array();
    completed_content.push_back({{"type", "output_text"}, {"text", "answer from completed"}});
    nlohmann::json completed_output = nlohmann::json::array();
    completed_output.push_back({{"type", "message"}, {"id", "msg-2"}, {"content", completed_content}});
    nlohmann::json completed_with_output;
    completed_with_output["type"] = "response.completed";
    completed_with_output["response"] = {{"id", "resp-2"}, {"status", "completed"}, {"output", completed_output}};
    std::string completed_body;
    for (const nlohmann::json& event : {reasoning, completed_with_output}) completed_body += Sse(event);
    server.Enqueue({200, "text/event-stream", completed_body});

    const auto completed_reply = adapter->Complete(request, {}, nullptr);
    Check(completed_reply.ok, "Responses API completed output array is accepted");
    if (completed_reply.ok) {
        Check(completed_reply.value.text == "answer from completed",
              "finished output array text is used when no text deltas arrive");
    }
    RemoveDirectory(workspace);
}

// A 2xx answer without a usable payload used to close the turn silently with an
// empty assistant message; it has to surface a diagnostic instead.
void TestResponsesEmptyPayloadDiagnostics() {
    FakeServer server;
    const std::string workspace = TempDirectory("responses_empty_payload");
    const auto config = MakeConfig(server.Port(), workspace);
    const auto* provider = spark::FindProvider(config, "local");
    const auto* model = spark::FindModel(*provider, "test-model");
    spark::ProviderRequest request;
    request.provider = provider;
    request.model = model;
    request.messages = {{{spark::MessageRole::User, "hello", {}, {}, {}}}};
    auto adapter = spark::CreateProviderAdapter("responses");

    server.Enqueue({200, "text/event-stream", "", 0, {{"X-Request-Id", "empty-stream-123"}}});
    const auto zero_bytes = adapter->Complete(request, {}, nullptr);
    Check(!zero_bytes.ok && zero_bytes.error.find("空响应体（0 字节）") != std::string::npos &&
              zero_bytes.error.find("HTTP 200") != std::string::npos &&
              zero_bytes.error.find("请求编号 empty-stream-123") != std::string::npos,
          "empty HTTP 200 stream reports no body and preserves the upstream request ID");

    server.Enqueue({200, "application/json", R"({"object":"error","message":"stream unsupported"})"});
    const auto not_streamed = adapter->Complete(request, {}, nullptr);
    Check(!not_streamed.ok, "non-SSE 2xx body is rejected");
    if (!not_streamed.ok) {
        Check(not_streamed.error.find("SSE") != std::string::npos,
              "non-SSE 2xx body reports a stream diagnostic");
        Check(not_streamed.error.find("stream unsupported") != std::string::npos,
              "non-SSE 2xx body includes the raw response prefix");
    }

    const nlohmann::json reasoning = {{"type", "response.reasoning_text.delta"}, {"delta", "still thinking"}};
    server.Enqueue({200, "text/event-stream", Sse(reasoning)});
    const auto truncated = adapter->Complete(request, {}, nullptr);
    Check(!truncated.ok, "stream without a completion event is rejected");
    if (!truncated.ok) {
        Check(truncated.error.find("流式响应意外中断") != std::string::npos,
              "truncated stream reports an interrupted stream");
        Check(truncated.error.find("推理片段 1 个") != std::string::npos,
              "truncated stream counts reasoning fragments");
    }

    const nlohmann::json completed = {
        {"type", "response.completed"},
        {"response", {{"id", "resp-3"}, {"status", "completed"}}},
    };
    server.Enqueue({200, "text/event-stream", Sse(completed)});
    const auto empty = adapter->Complete(request, {}, nullptr);
    Check(!empty.ok, "empty completion is rejected");
    if (!empty.ok) {
        Check(empty.error.find("空响应") != std::string::npos, "empty completion reports an empty response");
    }
    RemoveDirectory(workspace);
}

// Thinking models reject the follow-up request unless the reasoning of the previous
// assistant turn is handed back to the API, so a turn with tool calls has to replay it.
void TestReasoningReplayAcrossToolCalls() {
    FakeServer server;
    const std::string workspace = TempDirectory("reasoning_replay");
    auto config = MakeConfig(server.Port(), workspace);
    config.providers[0].protocol = "responses";

    const nlohmann::json reasoning_delta = {{"type", "response.reasoning_text.delta"}, {"delta", "step one "}};
    const nlohmann::json reasoning_done = {{"type", "response.reasoning_text.done"}, {"text", "step one step two"}};
    const nlohmann::json function_call = {
        {"type", "response.output_item.done"},
        {"item",
         {{"type", "function_call"},
          {"id", "item-1"},
          {"call_id", "call-1"},
          {"name", "read_file"},
          {"arguments", R"({"path":"README.md"})"}}},
    };
    const nlohmann::json completed_first = {
        {"type", "response.completed"},
        {"response", {{"id", "resp-1"}, {"status", "completed"}}},
    };
    std::string first_body;
    for (const nlohmann::json& event : {reasoning_delta, reasoning_done, function_call, completed_first}) {
        first_body += Sse(event);
    }
    server.Enqueue({200, "text/event-stream", first_body});

    const nlohmann::json text_delta = {{"type", "response.output_text.delta"}, {"delta", "all done"}};
    const nlohmann::json completed_second = {
        {"type", "response.completed"},
        {"response", {{"id", "resp-2"}, {"status", "completed"}}},
    };
    server.Enqueue({200, "text/event-stream", Sse(text_delta) + Sse(completed_second)});

    spark::SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, workspace).ok, "reasoning replay session opens");
    spark::Agent agent(config, &journal, nullptr);
    const auto result = agent.RunTurn("clean the cache", [](const spark::ApprovalRequest&) { return true; }, {},
                                      nullptr, /*continue_interrupted=*/false);
    Check(result.ok, "tool turn with reasoning completes");

    const auto requests = server.Requests();
    Check(requests.size() == 2, "reasoning turn issues two provider requests");
    if (requests.size() == 2) {
        const std::string second = RequestBody(requests[1]);
        const size_t reasoning_position = second.find("step one step two");
        const size_t tool_output_position = second.find("function_call_output");
        Check(second.find("\"reasoning\"") != std::string::npos, "second request carries a reasoning item");
        Check(reasoning_position != std::string::npos, "second request replays the reasoning text");
        Check(reasoning_position != std::string::npos && tool_output_position != std::string::npos &&
                  reasoning_position < tool_output_position,
              "reasoning item is sent before the tool output");
    }
    RemoveDirectory(workspace);
}

// Codex replays its whole history, so reasoning that belongs to a turn which already finished has
// to be handed back too. Dropping it is what produced DeepSeek's "The `reasoning_text` in the
// thinking mode must be passed back to the API" error.
void TestReasoningReplayForFinishedTurn() {
    FakeServer server;
    const std::string workspace = TempDirectory("reasoning_replay_turn_two");
    auto config = MakeConfig(server.Port(), workspace);
    config.providers[0].protocol = "responses";

    const nlohmann::json call_reasoning = {{"type", "response.reasoning_text.delta"}, {"delta", "call reasoning"}};
    const nlohmann::json call_reasoning_done = {{"type", "response.reasoning_text.done"}, {"text", "call reasoning"}};
    const nlohmann::json function_call = {
        {"type", "response.output_item.done"},
        {"item",
         {{"type", "function_call"},
          {"id", "item-1"},
          {"call_id", "call-1"},
          {"name", "read_file"},
          {"arguments", R"({"path":"README.md"})"}}},
    };
    const nlohmann::json completed_first = {
        {"type", "response.completed"},
        {"response", {{"id", "resp-1"}, {"status", "completed"}}},
    };
    std::string first_body;
    for (const nlohmann::json& event : {call_reasoning, call_reasoning_done, function_call, completed_first}) {
        first_body += Sse(event);
    }
    server.Enqueue({200, "text/event-stream", first_body});

    const nlohmann::json answer_reasoning = {{"type", "response.reasoning_text.delta"},
                                             {"delta", "final answer reasoning"}};
    const nlohmann::json answer_reasoning_done = {{"type", "response.reasoning_text.done"},
                                                  {"text", "final answer reasoning"}};
    const nlohmann::json answer_text = {{"type", "response.output_text.delta"}, {"delta", "all done"}};
    const nlohmann::json completed_second = {
        {"type", "response.completed"},
        {"response", {{"id", "resp-2"}, {"status", "completed"}}},
    };
    std::string second_body;
    for (const nlohmann::json& event : {answer_reasoning, answer_reasoning_done, answer_text, completed_second}) {
        second_body += Sse(event);
    }
    server.Enqueue({200, "text/event-stream", second_body});

    const nlohmann::json follow_up_text = {{"type", "response.output_text.delta"}, {"delta", "ok"}};
    const nlohmann::json completed_third = {
        {"type", "response.completed"},
        {"response", {{"id", "resp-3"}, {"status", "completed"}}},
    };
    server.Enqueue({200, "text/event-stream", Sse(follow_up_text) + Sse(completed_third)});

    spark::SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, workspace).ok, "finished-turn reasoning session opens");
    spark::Agent agent(config, &journal, nullptr);
    Check(agent.RunTurn("first task", [](const spark::ApprovalRequest&) { return true; }, {}, nullptr, false).ok,
          "first turn with a tool call completes");
    Check(agent.RunTurn("follow up", [](const spark::ApprovalRequest&) { return true; }, {}, nullptr, false).ok,
          "second turn completes");

    const auto requests = server.Requests();
    Check(requests.size() == 3, "two turns issue three provider requests");
    if (requests.size() == 3) {
        const std::string third = RequestBody(requests[2]);
        Check(third.find("final answer reasoning") != std::string::npos,
              "a finished assistant turn keeps replaying its reasoning");
        Check(third.find("call reasoning") != std::string::npos,
              "the earlier tool-call turn keeps replaying its reasoning");
    }
    RemoveDirectory(workspace);
}

// Parallel tool calls can lose an argument delta, so the finished item has to win over the
// accumulated string: a truncated value only surfaces later as "invalid tool arguments".
void TestFunctionCallArgumentsPreferFinishedItem() {
    FakeServer server;
    const std::string workspace = TempDirectory("arguments_final");
    const auto config = MakeConfig(server.Port(), workspace);
    const auto* provider = spark::FindProvider(config, "local");
    const auto* model = spark::FindModel(*provider, "test-model");
    spark::ProviderRequest request;
    request.provider = provider;
    request.model = model;
    request.messages = {{{spark::MessageRole::User, "hello", {}, {}, {}}}};
    auto adapter = spark::CreateProviderAdapter("responses");

    const nlohmann::json added = {
        {"type", "response.output_item.added"},
        {"item", {{"type", "function_call"}, {"id", "item-9"}, {"call_id", "call-9"}, {"name", "read_file"}, {"arguments", ""}}},
    };
    const nlohmann::json partial = {
        {"type", "response.function_call_arguments.delta"},
        {"item_id", "item-9"},
        {"delta", R"({"path":"READM)"},
    };
    const nlohmann::json finished = {
        {"type", "response.output_item.done"},
        {"item", {{"type", "function_call"}, {"id", "item-9"}, {"call_id", "call-9"}, {"name", "read_file"},
                  {"arguments", R"({"path":"README.md"})"}}},
    };
    const nlohmann::json completed = {
        {"type", "response.completed"},
        {"response", {{"id", "resp-9"}, {"status", "completed"}}},
    };
    std::string body;
    for (const nlohmann::json& event : {added, partial, finished, completed}) body += Sse(event);
    server.Enqueue({200, "text/event-stream", body});

    const auto reply = adapter->Complete(request, {}, nullptr);
    Check(reply.ok, "finished function_call item is accepted");
    if (reply.ok) {
        Check(reply.value.tool_calls.size() == 1, "finished item yields one tool call");
        if (reply.value.tool_calls.size() == 1) {
            Check(reply.value.tool_calls[0].arguments_json == R"({"path":"README.md"})",
                  "finished item arguments replace the truncated delta value");
        }
    }
    RemoveDirectory(workspace);
}

void TestProviderErrors() {
    FakeServer server;
    const std::string workspace = TempDirectory("errors");
    const auto config = MakeConfig(server.Port(), workspace);
    const auto* provider = spark::FindProvider(config, "local");
    const auto* model = spark::FindModel(*provider, "test-model");
    spark::ProviderRequest request{provider, model, {{{spark::MessageRole::User, "hello", {}, {}, {}}}}, {}};
    auto adapter = spark::CreateProviderAdapter("chat_completions");

    server.Enqueue({200, "text/event-stream", "data: {not-json}\n\n"});
    auto malformed = adapter->Complete(request, {}, nullptr);
    Check(!malformed.ok && malformed.error.find("malformed") != std::string::npos, "malformed SSE is rejected");

    server.Enqueue({401, "application/json", "{\"error\":{\"message\":\"bad key\"}}"});
    auto unauthorized = adapter->Complete(request, {}, nullptr);
    Check(!unauthorized.ok && unauthorized.error.find("bad key") != std::string::npos, "auth error is surfaced");
    RemoveDirectory(workspace);
}

void TestSlowProviderStartup() {
    FakeServer server;
    const std::string workspace = TempDirectory("slow_start");
    const auto config = MakeConfig(server.Port(), workspace);
    const auto* provider = spark::FindProvider(config, "local");
    const auto* model = spark::FindModel(*provider, "test-model");
    auto adapter = spark::CreateProviderAdapter("chat_completions");

    spark::ProviderRequest request;
    request.provider = provider;
    request.model = model;
    request.messages = {{{spark::MessageRole::User, "hello", {}, {}, {}}}};
    request.request_timeout_seconds = 10;
    request.response_header_timeout_seconds = 10;

    // The provider stays silent for longer than one WinHTTP receive poll slice
    // before it answers. Waiting for the response headers has to keep polling
    // instead of failing the request with a receive timeout (WinHTTP 12002).
    server.Enqueue({200, "text/event-stream", TextSse("slow but alive"), 1500});
    const uint64_t started = GetTickCount64();
    const auto slow = adapter->Complete(request, {}, nullptr);
    const uint64_t elapsed = GetTickCount64() - started;
    Check(slow.ok, "provider that starts slowly still succeeds");
    if (slow.ok) {
        Check(slow.value.text == "slow but alive", "slow provider reply is returned");
    }
    Check(elapsed >= 1200, "the slow provider was actually waited for");

    // A provider that never starts answering must fail close to the configured
    // header deadline and only once, so retries cannot multiply the wait.
    request.request_timeout_seconds = 1;
    request.response_header_timeout_seconds = 1;
    server.Enqueue({200, "text/event-stream", TextSse("too late"), 5000});
    const uint64_t failure_started = GetTickCount64();
    const auto stalled = adapter->Complete(request, {}, nullptr);
    const uint64_t failure_elapsed = GetTickCount64() - failure_started;
    Check(!stalled.ok, "provider that never answers fails");
    Check(stalled.error.find("12002") != std::string::npos,
          ("header timeout keeps the WinHTTP error code, got: " + stalled.error).c_str());
    Check(failure_elapsed < 2500, "header deadline is not multiplied by automatic retries");

    // Cancelling a turn while the provider is still silent has to end the wait
    // promptly instead of blocking until the header deadline expires.
    request.request_timeout_seconds = 30;
    request.response_header_timeout_seconds = 30;
    server.Enqueue({200, "text/event-stream", TextSse("never"), 5000});
    spark::CancellationToken cancellation;
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        cancellation.Cancel();
    });
    const uint64_t cancel_started = GetTickCount64();
    const auto cancelled = adapter->Complete(request, {}, &cancellation);
    const uint64_t cancel_elapsed = GetTickCount64() - cancel_started;
    canceller.join();
    Check(!cancelled.ok && cancelled.error.find("cancel") != std::string::npos,
          "cancelling during the header wait stops the request");
    Check(cancel_elapsed < 5000, "cancelling during the header wait returns promptly");
    RemoveDirectory(workspace);
}

void TestMidStreamCancellation() {
    FakeServer server;
    const std::string workspace = TempDirectory("cancel_stream");
    const auto config = MakeConfig(server.Port(), workspace);
    const auto* provider = spark::FindProvider(config, "local");
    const auto* model = spark::FindModel(*provider, "test-model");
    server.Enqueue({200, "text/event-stream", std::string(R"(data: {"choices":[{"delta":{"content":"partial"}}]})") + "\n\n"});
    spark::ProviderRequest request{provider, model, {{{spark::MessageRole::User, "hello", {}, {}, {}}}}, {}};
    auto adapter = spark::CreateProviderAdapter("chat_completions");
    spark::CancellationToken cancellation;
    spark::ProviderCallbacks callbacks;
    callbacks.on_text_delta = [&](const std::string&) { cancellation.Cancel(); };
    const auto response = adapter->Complete(request, callbacks, &cancellation);
    Check(!response.ok && response.error.find("cancel") != std::string::npos, "mid-stream cancellation stops the request");
    RemoveDirectory(workspace);
}

void TestMaxIterationsFinalizesGracefully() {
    FakeServer server;
    const std::string workspace = TempDirectory("max_iterations");
    auto config = MakeConfig(server.Port(), workspace);
    config.agent.max_iterations = 3;

    for (int index = 0; index < config.agent.max_iterations; ++index) {
        nlohmann::json event;
        event["choices"] = nlohmann::json::array();
        event["choices"].push_back({
            {"delta",
             {{"tool_calls",
               nlohmann::json::array({nlohmann::json{
                   {"index", 0},
                   {"id", "call-" + std::to_string(index)},
                   {"type", "function"},
                   {"function", {{"name", "list_files"}, {"arguments", R"({"path":"."})"}}},
               }})}}},
            {"finish_reason", "tool_calls"},
        });
        server.Enqueue({200, "text/event-stream", Sse(event) + "data: [DONE]\n\n"});
    }
    server.Enqueue({200, "text/event-stream", TextSse("final summary after tool limit")});

    spark::SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, workspace).ok, "max iteration test session opens");
    spark::Agent agent(config, &journal, nullptr);
    spark::AgentCallbacks callbacks;
    std::string streamed;
    std::string error;
    callbacks.on_text_delta = [&](const std::string& text) { streamed += text; };
    callbacks.on_error = [&](const std::string& text) { error = text; };
    const auto result = agent.RunTurn("keep working", [](const spark::ApprovalRequest&) { return true; },
                                      callbacks, nullptr);

    Check(result.ok, "max iteration limit finishes successfully");
    Check(error.empty(), "max iteration limit does not paint a red error");
    Check(streamed.find("final summary after tool limit") != std::string::npos,
          "finalization response is streamed");
    Check(server.Requests().size() == static_cast<size_t>(config.agent.max_iterations) + 1,
          "one final no-tools request follows the tool limit");
    Check(!journal.State().messages.empty() && journal.State().messages.back().role == spark::MessageRole::Assistant &&
              journal.State().messages.back().content == "final summary after tool limit",
          "finalization response is persisted in the transcript");
    RemoveDirectory(workspace);
}

void TestAgentToolRoundTripAndContext() {
    FakeServer server;
    const std::string workspace = TempDirectory("agent");
    std::string write_error;
    spark::WriteFileUtf8Atomic(spark::JoinPath(workspace, "hello.txt"), "hello file\n", &write_error);
    auto config = MakeConfig(server.Port(), workspace);
    spark::SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, workspace).ok, "agent test session opens");

    std::string first;
    first += std::string(R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call-1","function":{"name":"read_file","arguments":"{\"path\":\"hello.txt\"}"}}]},"finish_reason":"tool_calls"}]})") + "\n\n";
    first += "data: [DONE]\n\n";
    server.Enqueue({200, "text/event-stream", first});
    server.Enqueue({200, "text/event-stream", std::string(R"(data: {"choices":[{"delta":{"content":"file read"},"finish_reason":"stop"}]})") + "\n\ndata: [DONE]\n\n"});

    spark::Agent agent(config, &journal, nullptr);
    spark::AgentCallbacks callbacks;
    std::string streamed;
    callbacks.on_text_delta = [&](const std::string& text) { streamed += text; };
    const auto result = agent.RunTurn("read hello.txt", [](const spark::ApprovalRequest&) { return true; }, callbacks, nullptr);
    Check(result.ok, "agent tool round trip succeeds");
    Check(streamed == "file read", "final text streamed");
    Check(journal.State().messages.size() == 4, "agent persists user, assistant, tool, assistant messages");
    const auto requests = server.Requests();
    Check(requests.size() == 2, "agent sends two provider requests");
    if (requests.size() == 2) {
        Check(requests[1].find("hello file") != std::string::npos, "tool result is sent back to provider");
        Check(requests[1].find("call-1") != std::string::npos, "tool call id is preserved in context");
    }
    RemoveDirectory(workspace);
}

void TestParallelToolCallIDs() {
    FakeServer server;
    const std::string workspace = TempDirectory("parallel_tool_ids");
    std::string error;
    spark::WriteFileUtf8Atomic(spark::JoinPath(workspace, "a.txt"), "alpha\n", &error);
    spark::WriteFileUtf8Atomic(spark::JoinPath(workspace, "b.txt"), "beta\n", &error);
    auto config = MakeConfig(server.Port(), workspace);
    spark::SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, workspace).ok, "parallel tool session opens");

    nlohmann::json tools;
    tools["choices"] = nlohmann::json::array();
    tools["choices"].push_back({
        {"delta", {{"tool_calls", nlohmann::json::array({
                       {{"index", 0}, {"id", "call-a"},
                        {"function", {{"name", "read_file"}, {"arguments", "{\"path\":\"a.txt\"}"}}}},
                       {{"index", 1}, {"id", "call-b"},
                        {"function", {{"name", "read_file"}, {"arguments", "{\"path\":\"b.txt\"}"}}}},
                   })}}},
        {"finish_reason", "tool_calls"},
    });
    server.Enqueue({200, "text/event-stream", Sse(tools) + "data: [DONE]\n\n"});
    server.Enqueue({200, "text/event-stream", TextSse("both files read")});

    std::vector<std::pair<std::string, std::string>> starts;
    std::vector<std::pair<std::string, std::string>> finishes;
    spark::AgentCallbacks callbacks;
    callbacks.on_tool_start = [&](const std::string& call_id, const std::string& name, const std::string&) {
        starts.emplace_back(call_id, name);
    };
    callbacks.on_tool_finish = [&](const std::string& call_id, const std::string& name, bool,
                                   const std::string&) { finishes.emplace_back(call_id, name); };
    spark::Agent agent(config, &journal, nullptr);
    const auto result = agent.RunTurn("read both files", [](const spark::ApprovalRequest&) { return true; }, callbacks,
                                      nullptr);
    Check(result.ok, "parallel read tools complete successfully");
    Check(starts.size() == 2, "parallel reads emit two tool start events");
    Check(finishes.size() == 2, "parallel reads emit two tool finish events");
    if (starts.size() == 2 && finishes.size() == 2) {
        Check(starts[0].first == "call-a" && starts[1].first == "call-b", "parallel tool starts preserve call ids");
        Check(finishes[0].first == "call-a" && finishes[1].first == "call-b",
              "parallel tool finishes preserve call ids");
    }
    RemoveDirectory(workspace);
}

void TestCancellationAndModelSwitch() {
    FakeServer server;
    const std::string workspace = TempDirectory("switch");
    auto config = MakeConfig(server.Port(), workspace);
    spark::SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, workspace).ok, "switch test session opens");
    server.Enqueue({200, "text/event-stream", std::string(R"(data: {"choices":[{"delta":{"content":"first pass"},"finish_reason":"stop"}]})") + "\n\ndata: [DONE]\n\n"});
    spark::Agent first_agent(config, &journal, nullptr);
    Check(first_agent.RunTurn("first", [](const spark::ApprovalRequest&) { return true; }, {}, nullptr).ok, "first model turn succeeds");

    config.active.model = "test-model";
    server.Enqueue({200, "text/event-stream", std::string(R"(data: {"choices":[{"delta":{"content":"second pass"},"finish_reason":"stop"}]})") + "\n\ndata: [DONE]\n\n"});
    spark::Agent second_agent(config, &journal, nullptr);
    Check(second_agent.RunTurn("second", [](const spark::ApprovalRequest&) { return true; }, {}, nullptr).ok, "second model turn succeeds");
    const auto requests = server.Requests();
    Check(requests.size() == 2, "model switch issues two requests");
    if (requests.size() == 2) {
        Check(requests[0].find("Authorization: Bearer primary-secret") != std::string::npos, "first request uses provider key");
        Check(requests[1].find("Authorization: Bearer primary-secret") != std::string::npos, "second request reuses provider key");
        Check(requests[1].find("first pass") != std::string::npos, "conversation context is preserved across key switch");
    }

    spark::CancellationToken cancellation;
    cancellation.Cancel();
    spark::Agent cancelled_agent(config, &journal, nullptr);
    const auto cancelled = cancelled_agent.RunTurn("cancel me", [](const spark::ApprovalRequest&) { return true; }, {}, &cancellation);
    Check(!cancelled.ok && cancelled.error == "cancelled", "pre-cancelled turn is cancelled without provider request");
    RemoveDirectory(workspace);
}

void TestContinueInterruptedTurn() {
    FakeServer server;
    const std::string workspace = TempDirectory("continue_interrupted");
    auto config = MakeConfig(server.Port(), workspace);
    spark::SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, workspace).ok, "continue test session opens");
    AppendSyntheticMessage(journal, spark::MessageRole::User, "finish the interrupted task");
    AppendSyntheticMessage(journal, spark::MessageRole::Assistant, "partial result");
    journal.MutableState().metadata["paused_turn"] = {
        {"elapsed_ms", 1234},
        {"interrupted_at", 1},
    };

    const size_t users_before = static_cast<size_t>(std::count_if(
        journal.State().messages.begin(), journal.State().messages.end(),
        [](const spark::ChatMessage& message) { return message.role == spark::MessageRole::User; }));
    server.Enqueue({200, "text/event-stream", TextSse("completed after resume")});
    spark::Agent agent(config, &journal, nullptr);
    Check(agent.RunTurn({}, [](const spark::ApprovalRequest&) { return true; }, {}, nullptr,
                        /*continue_interrupted=*/true)
              .ok,
          "interrupted turn resumes without a new user message");
    const size_t users_after = static_cast<size_t>(std::count_if(
        journal.State().messages.begin(), journal.State().messages.end(),
        [](const spark::ChatMessage& message) { return message.role == spark::MessageRole::User; }));
    Check(users_before == users_after, "resume does not add another user bubble");
    Check(!journal.State().metadata.contains("paused_turn"),
          "successful resume clears the paused marker after persisting the continuation event");
    const auto requests = server.Requests();
    Check(requests.size() == 1, "resume issues one provider request");
    if (requests.size() == 1) {
        Check(RequestBody(requests[0]).find("resumed the interrupted turn") != std::string::npos,
              "resume instruction reaches provider context");
    }
    RemoveDirectory(workspace);
}

void TestAgentRedactsToolOutput() {
    FakeServer server;
    const std::string workspace = TempDirectory("redact");
    std::string write_error;
    spark::WriteFileUtf8Atomic(spark::JoinPath(workspace, "secret.txt"), "value=primary-secret\n", &write_error);
    auto config = MakeConfig(server.Port(), workspace);
    config.storage.redact_secrets = true;
    spark::SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, workspace).ok, "redaction test session opens");
    server.Enqueue({200, "text/event-stream",
                    std::string(R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call-secret","function":{"name":"read_file","arguments":"{\"path\":\"secret.txt\"}"}}]},"finish_reason":"tool_calls"}]})") +
                        "\n\ndata: [DONE]\n\n"});
    server.Enqueue({200, "text/event-stream",
                    std::string(R"(data: {"choices":[{"delta":{"content":"read"},"finish_reason":"stop"}]})") + "\n\ndata: [DONE]\n\n"});
    spark::Agent agent(config, &journal, nullptr);
    Check(agent.RunTurn("read secret", [](const spark::ApprovalRequest&) { return true; }, {}, nullptr).ok,
          "redaction test turn succeeds");
    const auto requests = server.Requests();
    Check(requests.size() == 2, "redaction test sends two requests");
    if (requests.size() == 2) {
        const std::string body = RequestBody(requests[1]);
        Check(body.find("primary-secret") == std::string::npos, "secret is absent from tool output sent to provider");
        Check(body.find("[REDACTED]") != std::string::npos, "redaction marker reaches provider context");
    }
    RemoveDirectory(workspace);
}

void TestWriteToolExecutesOnce() {
    FakeServer server;
    const std::string workspace = TempDirectory("write_once");
    auto config = MakeConfig(server.Port(), workspace);
    spark::SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, workspace).ok, "write-once test session opens");
    server.Enqueue({200, "text/event-stream",
                    std::string(R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call-write","function":{"name":"write_file","arguments":"{\"path\":\"once.txt\",\"content\":\"once\"}"}}]},"finish_reason":"tool_calls"}]})") +
                        "\n\ndata: [DONE]\n\n"});
    server.Enqueue({200, "text/event-stream",
                    std::string(R"(data: {"choices":[{"delta":{"content":"done"},"finish_reason":"stop"}]})") + "\n\ndata: [DONE]\n\n"});
    spark::Agent agent(config, &journal, nullptr);
    Check(agent.RunTurn("write once", [](const spark::ApprovalRequest&) { return true; }, {}, nullptr).ok,
          "write-once turn succeeds");
    const auto contents = spark::ReadFileUtf8(spark::JoinPath(workspace, "once.txt"));
    Check(contents.has_value() && *contents == "once", "side-effecting tool executes exactly once");
    Check(server.Requests().size() == 2, "side-effecting tool does not cause an extra retry request");
    RemoveDirectory(workspace);
}

void TestGoalAndPlanMode() {
    FakeServer server;
    const std::string workspace = TempDirectory("goal_plan");
    auto config = MakeConfig(server.Port(), workspace);
    config.workspace.file_write_approval = "allow";
    config.workspace.command_approval = "allow";
    config.workspace.mcp_tool_approval = "allow";
    config.skills.enabled = false;
    Check(spark::WriteFileUtf8Atomic(spark::JoinPath(workspace, "evidence.txt"), "verified evidence", nullptr),
          "plan fixture creates a file to investigate");
    spark::SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, workspace).ok, "plan fixture opens a separate session");
    const auto tool = [](int index, const char* name, const nlohmann::json& arguments) {
        return nlohmann::json{{"index", index}, {"id", "plan-" + std::to_string(index)},
                              {"function", {{"name", name}, {"arguments", arguments.dump()}}}};
    };
    server.Enqueue({200, "text/event-stream", Sse({{"choices", nlohmann::json::array({
        {{"delta", {{"tool_calls", nlohmann::json::array({
            tool(0, "read_file", {{"path", "evidence.txt"}}),
            tool(1, "write_file", {{"path", "forbidden.txt"}, {"content", "must not exist"}}),
            tool(2, "apply_patch", {{"changes", nlohmann::json::array({
                {{"path", "evidence.txt"}, {"old_text", "verified"}, {"new_text", "changed"}}
            })}}),
            tool(3, "run_command", {{"command", "echo forbidden>command.txt"}}),
            tool(4, "mcp__fake__echo", {{"message", "must not run"}}),
            tool(5, "spawn_agent", {{"task", "must not start"}}),
        })}}}, {"finish_reason", "tool_calls"}}
    })}}) + "data: [DONE]\n\n"});
    server.Enqueue({200, "text/event-stream", TextSse("A concrete plan using verified evidence.")});
    spark::SubAgentManager manager;
    spark::Agent plan(config, &journal, nullptr, &manager, {spark::CollaborationMode::Plan, "GOAL-KEEP-123"});
    int approvals = 0;
    std::string output;
    spark::AgentCallbacks callbacks;
    callbacks.on_text_delta = [&](const std::string& delta) { output += delta; };
    Check(plan.RunTurn("Investigate, then implement.", [&](const spark::ApprovalRequest&) {
        ++approvals; return true;
    }, callbacks, nullptr).ok, "plan still finishes after rejecting unexpected side-effecting calls");
    Check(output == "A concrete plan using verified evidence.", "plan output is streamed normally");
    Check(approvals == 0 && manager.List(journal.Path()).empty(), "plan cannot approve or spawn around its policy");
    Check(!spark::PathExists(spark::JoinPath(workspace, "forbidden.txt")) &&
              !spark::PathExists(spark::JoinPath(workspace, "command.txt")) &&
              spark::ReadFileUtf8(spark::JoinPath(workspace, "evidence.txt")).value_or("") == "verified evidence",
          "plan prevents writes, patches and commands even with fully allowed workspace policies");
    const auto requests = server.Requests();
    Check(requests.size() == 2, "plan reads the result and continues");
    for (const auto& request : requests) {
        const auto body = nlohmann::json::parse(RequestBody(request));
        Check(body["messages"][0]["content"].get<std::string>().find("GOAL-KEEP-123") != std::string::npos &&
                  body["messages"][0]["content"].get<std::string>().find("[计划模式]") != std::string::npos,
              "goal and plan rules persist through every tool round trip");
        std::set<std::string> names;
        for (const auto& definition : body["tools"]) names.insert(definition["function"]["name"]);
        Check(names.count("read_file") && !names.count("write_file") && !names.count("apply_patch") &&
                  !names.count("run_command") && !names.count("spawn_agent") && !names.count("mcp__fake__echo"),
              "plan advertises only safe investigation tools");
    }
    if (requests.size() == 2) {
        Check(RequestBody(requests[1]).find("verified evidence") != std::string::npos &&
                  RequestBody(requests[1]).find("Plan Mode allows read-only") != std::string::npos,
              "actual read results and explicit policy denials reach the next request");
    }
    server.Enqueue({200, "text/event-stream", TextSse("checkpoint")});
    Check(plan.RunTurn("/compact", {}, {}, nullptr).ok, "goal conversation can compact");
    spark::SessionJournal restored;
    Check(restored.Resume(journal.Path()).ok, "goal conversation reloads after compaction");
    server.Enqueue({200, "text/event-stream", TextSse("continued")});
    spark::Agent continued(config, &restored, nullptr, &manager, {spark::CollaborationMode::Plan, "GOAL-KEEP-123"});
    Check(continued.RunTurn("Continue investigating.", {}, {}, nullptr).ok, "goal remains usable after compaction");
    const auto later = server.Requests();
    Check(later.size() == 4 && RequestBody(later.back()).find("GOAL-KEEP-123") != std::string::npos,
          "persistent goal is injected after compaction and session reload");
    server.Enqueue({200, "text/event-stream", Sse({{"choices", nlohmann::json::array({
        {{"delta", {{"tool_calls", nlohmann::json::array({
            tool(0, "write_file", {{"path", "allowed.txt"}, {"content", "implemented"}})
        })}}}, {"finish_reason", "tool_calls"}}
    })}}) + "data: [DONE]\n\n"});
    server.Enqueue({200, "text/event-stream", TextSse("done")});
    spark::Agent execute(config, &restored, nullptr);
    Check(execute.RunTurn("Plan mode is now disabled; implement.", {}, {}, nullptr).ok &&
              spark::ReadFileUtf8(spark::JoinPath(workspace, "allowed.txt")).value_or("") == "implemented",
          "disabling plan mode restores normal execution");
    RemoveDirectory(workspace);
}

void TestExternalApprovalOverrides() {
    for (const std::string mode : {"full_access", "approve_for_me"}) {
        FakeServer server;
        const auto workspace = TempDirectory("external_override_" + mode);
        auto config = MakeConfig(server.Port(), workspace);
        spark::ApplyPermissionMode(&config, mode);
        config.workspace.file_write_approval = "ask";
        server.Enqueue({200, "text/event-stream",
            R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"write","function":{"name":"write_file","arguments":"{\"path\":\"approval.txt\",\"content\":\"requires approval\"}"}}]},"finish_reason":"tool_calls"}]})"
            "\n\ndata: [DONE]\n\n"});
        server.Enqueue({200, "text/event-stream", TextSse("finished")});
        {
            spark::ExternalAgentManager manager(config, config.config_path);
            Check(manager.Initialize().ok, "external approval fixture initializes");
            const auto created = manager.Create("write", "approval test", "", "", "", workspace, mode);
            Check(created.ok, "external approval fixture creates a session");
            if (created.ok) {
                const auto waiting = manager.Wait(created.value.id, 2000);
                Check(waiting.ok && waiting.value.awaiting_approval &&
                          !spark::PathExists(spark::JoinPath(workspace, "approval.txt")),
                      "explicit ask is honored even in an automatic permission mode");
                if (waiting.ok && waiting.value.awaiting_approval) {
                    Check(manager.Approve(created.value.id, false).ok, "external request can be denied");
                    const auto done = manager.Wait(created.value.id, 2000);
                    Check(done.ok && done.value.state == "completed" &&
                              !spark::PathExists(spark::JoinPath(workspace, "approval.txt")),
                          "denied write never reaches disk");
                } else {
                    manager.Cancel(created.value.id);
                }
            }
        }
        RemoveDirectory(workspace);
    }
}

void QueueHttpMcpDiscovery(FakeServer& server) {
    server.Enqueue({200, "application/json",
        R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-06-18","capabilities":{}}})"});
    server.Enqueue({202, "application/json", ""});
    server.Enqueue({200, "application/json",
        R"({"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"echo","description":"echo","inputSchema":{"type":"object"}}]}})"});
}

void TestExternalApprovalQueue() {
    FakeServer server;
    const auto root = TempDirectory("external_approval_queue");
    const auto workspace = spark::JoinPath(root, "work");
    spark::fs::create_directories(spark::Utf8ToWide(workspace));
    auto config = MakeConfig(server.Port(), workspace);
    spark::ApplyPermissionMode(&config, "full_access");
    config.workspace.allow_outside_reads = false;
    config.agent.max_parallel_read_tools = 2;
    config.providers.front().models.front().capabilities.context_window = 100000;
    nlohmann::json calls = nlohmann::json::array();
    for (int index = 0; index < 2; ++index) {
        const auto path = spark::JoinPath(root, std::to_string(index) + ".txt");
        Check(spark::WriteFileUtf8Atomic(path, index == 0 ? "FIRST-MARKER" : "SECOND-MARKER"),
              "parallel approval fixture writes outside-workspace evidence");
        calls.push_back({{"index", index}, {"id", "read-" + std::to_string(index)},
                         {"function", {{"name", "read_file"},
                             {"arguments", nlohmann::json{{"path", path}}.dump()}}}});
    }
    server.Enqueue({200, "text/event-stream",
        Sse({{"choices", nlohmann::json::array({
            {{"delta", {{"tool_calls", calls}}}, {"finish_reason", "tool_calls"}}
        })}}) +
        "data: [DONE]\n\n"});
    server.Enqueue({200, "text/event-stream", TextSse("done")});
    {
        spark::ExternalAgentManager manager(config, config.config_path);
        Check(manager.Initialize().ok, "parallel approval manager initializes");
        const auto created = manager.Create("read both files", "approval queue", "", "", "", workspace, "full_access");
        Check(created.ok, "parallel approval session starts");
        if (created.ok) {
            const auto first = manager.Wait(created.value.id, 2000);
            Check(first.ok && first.value.awaiting_approval, "first parallel request waits for approval");
            if (first.ok && first.value.awaiting_approval) {
                Check(manager.Approve(created.value.id, true).ok, "first request is approved separately");
                const auto second = manager.Wait(created.value.id, 2000);
                Check(second.ok && second.value.awaiting_approval &&
                          second.value.approval_detail != first.value.approval_detail,
                      "second parallel request stays pending after approving the first");
                if (second.ok && second.value.awaiting_approval) {
                    Check(manager.Approve(created.value.id, false).ok, "second request is denied separately");
                    const auto done = manager.Wait(created.value.id, 2000);
                    const auto requests = server.Requests();
                    const auto body = requests.size() == 2 ? RequestBody(requests.back()) : std::string{};
                    Check(done.ok && done.value.state == "completed" &&
                              ((body.find("FIRST-MARKER") != std::string::npos) !=
                               (body.find("SECOND-MARKER") != std::string::npos)),
                          "only individually approved file contents reach the model");
                }
            }
            manager.Cancel(created.value.id);
        }
    }
    RemoveDirectory(root);
}

void TestMcpSessionNetworkPolicy() {
    FakeServer model_server;
    FakeServer mcp_server;
    const auto workspace = TempDirectory("mcp_network_policy");
    auto config = MakeConfig(model_server.Port(), workspace);
    config.permissions.allow_network = true;
    spark::McpServerConfig remote;
    remote.id = "remote";
    remote.enabled = true;
    remote.transport = "streamable_http";
    remote.timeout_seconds = 1;
    remote.url = "http://127.0.0.1:" + std::to_string(mcp_server.Port()) + "/mcp";
    config.mcp_servers.push_back(remote);
    QueueHttpMcpDiscovery(mcp_server);
    spark::McpManager manager(config);
    const auto discovered = manager.DiscoverEnabled();
    Check(discovered.ok && discovered.value.size() == 1, "HTTP MCP fixture discovers its tool");
    config.permissions.allow_network = false;
    model_server.Enqueue({200, "text/event-stream",
        R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"remote","function":{"name":"mcp__remote__echo","arguments":"{}"}}]},"finish_reason":"tool_calls"}]})"
        "\n\ndata: [DONE]\n\n"});
    model_server.Enqueue({200, "text/event-stream", TextSse("blocked")});
    spark::SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, workspace).ok, "network policy journal opens");
    spark::Agent agent(config, &journal, &manager);
    Check(agent.RunTurn("check network policy", {}, {}, nullptr).ok, "blocked HTTP MCP turn completes");
    const auto requests = model_server.Requests();
    Check(requests.size() == 2 && RequestBody(requests[0]).find("mcp__remote__echo") == std::string::npos &&
              RequestBody(requests[1]).find("permissions.allow_network") != std::string::npos &&
              mcp_server.Requests().size() == 3,
          "session policy hides cached HTTP tools and blocks forced calls before network access");

    FakeServer external_model;
    FakeServer external_mcp;
    config.providers.front().base_url = "http://127.0.0.1:" + std::to_string(external_model.Port()) + "/v1";
    config.mcp_servers.front().url = "http://127.0.0.1:" + std::to_string(external_mcp.Port()) + "/mcp";
    QueueHttpMcpDiscovery(external_mcp);
    external_model.Enqueue({200, "text/event-stream", TextSse("ready")});
    {
        spark::ExternalAgentManager external(config, config.config_path);
        Check(external.Initialize().ok && external_mcp.Requests().empty(),
              "optional HTTP MCP remains offline under default network restrictions");
        const auto created = external.Create("list available tools", "network test", "", "", "", workspace, "full_access");
        Check(created.ok, "full access external session starts");
        if (created.ok) {
            const auto done = external.Wait(created.value.id, 2000);
            Check(done.ok && done.value.state == "completed" &&
                      external_model.Requests().size() == 1 &&
                      RequestBody(external_model.Requests().front()).find("mcp__remote__echo") != std::string::npos,
                  "external full access session discovers HTTP MCP under its own policy");
            external.Cancel(created.value.id);
        }
    }
    RemoveDirectory(workspace);
}

void TestDsmlToolCallRoundTrip() {
    FakeServer server;
    const std::string workspace = TempDirectory("dsml_round_trip");
    auto config = MakeConfig(server.Port(), workspace);
    spark::SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, workspace).ok, "DSML round-trip test session opens");

    const std::string dsml =
        R"(< | | DSML | | calls>
< | | DSML | | invoke name="write_file">
< | | DSML | | parameter name="path" string="true">dsml.txt</ | | DSML | | parameter>
< | | DSML | | parameter name="content" string="true">written through DSML</ | | DSML | | parameter>
</ | | DSML | | invoke>
</ | | DSML | | calls>)";
    server.Enqueue({200, "text/event-stream", TextSse("before\n" + dsml + "\nafter")});
    server.Enqueue({200, "text/event-stream", TextSse("dsml complete")});

    spark::Agent agent(config, &journal, nullptr);
    spark::AgentCallbacks callbacks;
    std::string streamed;
    std::string error;
    callbacks.on_text_delta = [&](const std::string& text) { streamed += text; };
    callbacks.on_error = [&](const std::string& text) { error = text; };
    const auto result = agent.RunTurn("write through DSML", [](const spark::ApprovalRequest&) { return true; },
                                      callbacks, nullptr);

    Check(result.ok, "DSML tool call turn succeeds");
    Check(error.empty(), "DSML tool call does not report an error");
    const auto contents = spark::ReadFileUtf8(spark::JoinPath(workspace, "dsml.txt"));
    Check(contents.has_value() && *contents == "written through DSML", "DSML tool call executes the requested tool");
    Check(server.Requests().size() == 2, "DSML tool call continues to a second model request");
    Check(streamed.find("DSML") == std::string::npos, "DSML protocol markup is hidden from streamed output");
    Check(streamed.find("before") != std::string::npos && streamed.find("after") != std::string::npos &&
              streamed.find("dsml complete") != std::string::npos,
          "normal text around a DSML block remains visible");
    RemoveDirectory(workspace);
}

void TestRepeatedAutoCompactionAtSixHundredThousandTokens() {
    FakeServer server;
    const std::string workspace = TempDirectory("compact_600k");
    auto config = MakeConfig(server.Port(), workspace);
    config.providers[0].models[0].capabilities.context_window = 1000000;
    config.context.auto_compact = false;  // Legacy config must not disable context safety.
    config.context.auto_compact_threshold_percent = 60;
    spark::SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, workspace).ok, "600k compaction session opens");

    AppendSyntheticContext(journal, 580000, "SIX-HUNDRED-K");
    const std::string first_user(120000, 'a');
    const std::string second_user(120000, 'b');
    server.Enqueue({200, "text/event-stream", TextSse("first model handoff: SIX-HUNDRED-K")});
    server.Enqueue({200, "text/event-stream", TextSse("first final answer")});
    spark::Agent first_agent(config, &journal, nullptr);
    Check(first_agent.RunTurn(first_user, [](const spark::ApprovalRequest&) { return true; }, {}, nullptr).ok,
          "first 600k turn succeeds");

    AppendSyntheticContext(journal, 600000, "SIX-HUNDRED-K-SECOND");
    server.Enqueue({200, "text/event-stream", TextSse("second model handoff: SIX-HUNDRED-K-SECOND")});
    server.Enqueue({200, "text/event-stream", TextSse("second final answer")});
    spark::Agent second_agent(config, &journal, nullptr);
    Check(second_agent.RunTurn(second_user, [](const spark::ApprovalRequest&) { return true; }, {}, nullptr).ok,
          "second 600k turn succeeds");

    const auto events = CompactionEvents(journal.Path());
    Check(events.size() == 2, "600k context performs two automatic compactions");
    if (events.size() == 2) {
        Check(events[0].value("before_tokens", uint64_t{0}) >= 600000,
              "first automatic compaction crosses the configured limit");
        Check(events[1].value("before_tokens", uint64_t{0}) >= 600000,
              "second automatic compaction crosses the configured limit");
        Check(events[0].value("summary_source", std::string{}) == "model",
              "first automatic compaction uses a model handoff summary");
        Check(events[1].value("summary_source", std::string{}) == "model",
              "second automatic compaction uses a model handoff summary");
        Check(events[1].value("summary", std::string{}).find("SIX-HUNDRED-K-SECOND") != std::string::npos,
              "second handoff retains the latest summary marker");
    }
    const auto requests = server.Requests();
    Check(requests.size() == 4, "two 600k compactions use two summary and two normal requests");
    if (requests.size() == 4) {
        Check(RequestBody(requests[0]).find("CONTEXT CHECKPOINT COMPACTION") != std::string::npos,
              "first compaction request carries the Codex-style summary prompt");
        Check(RequestBody(requests[3]).find("second model handoff") != std::string::npos,
              "normal request resumes from the second handoff summary");
    }
    spark::SessionJournal resumed;
    Check(resumed.Resume(journal.Path()).ok, "automatic compaction session resumes from disk");
    Check(spark::EstimateMessagesTokens(spark::ModelContextMessages(resumed.State())) < 600000,
          "automatic compaction context survives a session reload");
    Check(resumed.State().messages.size() > resumed.State().context_messages.size(),
          "compaction keeps the complete visible transcript separate from model context");
    RemoveDirectory(workspace);
}

void TestCompactionBeyondOneMillionTokens() {
    FakeServer server;
    const std::string workspace = TempDirectory("compact_over_1m");
    auto config = MakeConfig(server.Port(), workspace);
    config.providers[0].models[0].capabilities.context_window = 1000000;
    config.context.auto_compact = false;
    config.context.auto_compact_threshold_percent = 100;
    spark::SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, workspace).ok, "over-1m compaction session opens");

    AppendSyntheticContext(journal, 1080000, "OVER-ONE-M", "EARLY-UNIQUE-MARKER");
    server.Enqueue({200, "text/event-stream", TextSse("first chunk summary")});
    server.Enqueue({200, "text/event-stream", TextSse("final handoff: preserved EARLY-UNIQUE-MARKER")});
    server.Enqueue({200, "text/event-stream", TextSse("over-1m final answer")});
    spark::Agent agent(config, &journal, nullptr);
    Check(agent.RunTurn("continue after the large context", [](const spark::ApprovalRequest&) { return true; }, {}, nullptr).ok,
          "over-1m turn succeeds through chunked compaction");

    const auto events = CompactionEvents(journal.Path());
    Check(events.size() == 1, "over-1m context compacts once");
    if (events.size() == 1) {
        Check(events[0].value("before_tokens", uint64_t{0}) > 1000000,
              "over-1m compaction reports more than one million estimated tokens");
        Check(events[0].value("summary_source", std::string{}) == "model",
              "over-1m compaction uses model summaries");
        Check(events[0].value("summary_passes", 0) >= 2,
              "over-1m compaction uses multiple summary passes");
        Check(events[0].value("after_tokens", uint64_t{0}) < 100000,
              "over-1m compaction replaces the large raw history with a bounded handoff");
        Check(events[0].value("summary", std::string{}).find("EARLY-UNIQUE-MARKER") != std::string::npos,
              "critical early marker survives the final model handoff");
    }
    const auto requests = server.Requests();
    Check(requests.size() == 3, "over-1m compaction uses two summary requests and one normal request");
    if (requests.size() == 3) {
        Check(RequestBody(requests[0]).find("CONTEXT CHECKPOINT COMPACTION") != std::string::npos,
              "first over-1m summary request carries the compaction prompt");
        Check(RequestBody(requests[1]).find("CONTEXT CHECKPOINT COMPACTION") != std::string::npos,
              "second over-1m summary request carries the compaction prompt");
        const std::string final_body = RequestBody(requests[2]);
        Check(final_body.find("EARLY-UNIQUE-MARKER") != std::string::npos,
              "critical marker reaches the post-compaction normal request");
        Check(final_body.size() < 200 * 1024, "raw over-1m history is not resent after compaction");
    }
    RemoveDirectory(workspace);
}

void TestManualCompactCommand() {
    FakeServer server;
    const std::string workspace = TempDirectory("compact_command");
    auto config = MakeConfig(server.Port(), workspace);
    config.providers[0].models[0].capabilities.context_window = 1000000;
    config.context.auto_compact_threshold_percent = 90;
    spark::SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, workspace).ok, "manual compaction session opens");
    AppendSyntheticMessage(journal, spark::MessageRole::User, "keep this manual goal");
    AppendSyntheticMessage(journal, spark::MessageRole::Assistant, "manual work completed");

    server.Enqueue({200, "text/event-stream", TextSse("manual model handoff")});
    spark::Agent agent(config, &journal, nullptr);
    std::string status;
    spark::AgentCallbacks callbacks;
    callbacks.on_status = [&](const std::string& value) { status += value + "\n"; };
    Check(agent.RunTurn("/compact", [](const spark::ApprovalRequest&) { return true; }, callbacks, nullptr).ok,
          "manual compact command succeeds");
    const auto events = CompactionEvents(journal.Path());
    Check(events.size() == 1 && events[0].value("trigger", std::string{}) == "manual",
          "manual compact command writes a manual compaction event");
    Check(server.Requests().size() == 1, "manual compact command makes one summary request");
    Check(status.find("manual model handoff") == std::string::npos,
          "manual compact status does not leak the raw summary into chat output");
    RemoveDirectory(workspace);
}

void TestMcpStdioLifecycle(const std::string& fake_server_path) {
    const std::string workspace = TempDirectory("mcp");
    auto config = MakeConfig(1, workspace);
    spark::McpServerConfig server;
    server.id = "fake";
    server.enabled = true;
    server.transport = "stdio";
    server.command = fake_server_path;
    server.timeout_seconds = 5;
    config.mcp_servers.push_back(server);
    spark::McpManager manager(config);
    const auto discovered = manager.DiscoverEnabled(nullptr);
    Check(discovered.ok && discovered.value.size() == 2, "stdio MCP tools discovered");
    std::string echo_name;
    std::string shout_name;
    if (discovered.ok) {
        for (const spark::McpTool& tool : discovered.value) {
            if (tool.name == "echo") echo_name = tool.exposed_name;
            if (tool.name == "shout") shout_name = tool.exposed_name;
        }
    }
    Check(echo_name == "mcp__fake__echo", "MCP tool names are namespaced by server id");
    if (!echo_name.empty()) {
        const std::string name = echo_name;
        const auto first = manager.Execute(name, {{"message", "hello"}}, nullptr);
        const auto second = manager.Execute(name, {{"message", "again"}}, nullptr);
        Check(first.ok && first.value.find("hello|pid=") != std::string::npos, "first MCP call succeeds");
        Check(second.ok && second.value.find("again|pid=") != std::string::npos, "second MCP call succeeds");
        const size_t first_pid = first.value.find("pid=");
        const size_t second_pid = second.value.find("pid=");
        Check(first_pid != std::string::npos && second_pid != std::string::npos &&
                  first.value.substr(first_pid, first.value.find('|', first_pid) - first_pid) ==
                      second.value.substr(second_pid, second.value.find('|', second_pid) - second_pid),
              "two MCP calls reuse one server process");
        Check(second.value.find("count=2") != std::string::npos, "MCP server remains alive between calls");
    }
    if (!shout_name.empty()) {
        const auto shouted = manager.Execute(shout_name, {{"message", "quiet"}}, nullptr);
        Check(shouted.ok && shouted.value.find("QUIET|") != std::string::npos, "second MCP tool is callable");
    }
    manager.StopAll();
    RemoveDirectory(workspace);
}

void TestMcpToolFiltering(const std::string& fake_server_path) {
    const std::string workspace = TempDirectory("mcp_filter");
    auto config = MakeConfig(1, workspace);
    spark::McpServerConfig server;
    server.id = "fake";
    server.enabled = true;
    server.transport = "stdio";
    server.command = fake_server_path;
    server.timeout_seconds = 5;
    server.enabled_tools = {"echo"};
    config.mcp_servers.push_back(server);

    spark::McpManager allow_only(config);
    const auto allow_only_discovered = allow_only.DiscoverEnabled(nullptr);
    Check(allow_only_discovered.ok && allow_only_discovered.value.size() == 1 &&
              allow_only_discovered.value[0].name == "echo",
          "enabled_tools keeps only the listed tool");
    allow_only.StopAll();

    config.mcp_servers[0].enabled_tools.clear();
    config.mcp_servers[0].disabled_tools = {"shout"};
    spark::McpManager deny_one(config);
    const auto deny_one_discovered = deny_one.DiscoverEnabled(nullptr);
    Check(deny_one_discovered.ok && deny_one_discovered.value.size() == 1 &&
              deny_one_discovered.value[0].name == "echo",
          "disabled_tools removes the listed tool");
    deny_one.StopAll();

    config.mcp_servers[0].disabled_tools.clear();
    config.mcp_servers[0].command = fake_server_path + ".missing";
    config.mcp_servers[0].required = true;
    spark::McpManager required(config);
    const auto required_discovered = required.DiscoverEnabled(nullptr);
    Check(!required_discovered.ok &&
              required_discovered.error.find("required MCP server failed") != std::string::npos,
          "a required MCP server surfaces its startup failure");
    required.StopAll();

    RemoveDirectory(workspace);
}

void TestSkillInjectionAndReadTool() {
    FakeServer server;
    const std::string workspace = TempDirectory("skills");
    auto config = MakeConfig(server.Port(), workspace);
    const std::string skills_root = spark::JoinPath(workspace, "skill_library");
    std::error_code error;
    spark::fs::create_directories(spark::Utf8ToWide(spark::JoinPath(skills_root, "review")), error);
    spark::WriteFileUtf8Atomic(spark::JoinPath(skills_root, "review\\SKILL.md"),
                               "---\n"
                               "name: review-skill\n"
                               "description: Review a change before shipping.\n"
                               "version: \"1.2\"\n"
                               "allowed-tools: [read_file, search_text]\n"
                               "---\n"
                               "# Review skill\n"
                               "Check the diff, then run the focused tests.\n",
                               nullptr);
    config.skills.roots = {skills_root};

    server.Enqueue({200, "text/event-stream", TextSse("done")});
    spark::SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, workspace).ok, "skill journal opens");
    spark::Agent agent(config, &journal, nullptr);
    spark::AgentCallbacks callbacks;
    Check(agent.RunTurn("$review-skill please check this change",
                        [](const spark::ApprovalRequest&) { return true; }, callbacks, nullptr)
              .ok,
          "skill turn completes");
    const auto requests = server.Requests();
    Check(requests.size() == 1, "skill turn makes one model request");
    if (!requests.empty()) {
        const std::string body = RequestBody(requests[0]);
        Check(body.find("read_skill") != std::string::npos, "read_skill is offered when skills exist");
        Check(body.find("review-skill") != std::string::npos, "an explicit $skill mention is expanded");
        Check(body.find("Check the diff") != std::string::npos, "the skill body reaches the model context");
    }
    // The visible transcript stays clean: skill bodies are turn-scoped context only.
    bool transcript_has_skill_body = false;
    for (const spark::ChatMessage& message : journal.State().messages) {
        if (message.content.find("Check the diff") != std::string::npos) transcript_has_skill_body = true;
    }
    Check(!transcript_has_skill_body, "the skill body is not written into the visible transcript");
    RemoveDirectory(workspace);
}

void TestConcurrentAgentTurns() {
    FakeServer first_server;
    FakeServer second_server;
    const std::string first_workspace = TempDirectory("concurrent_first");
    const std::string second_workspace = TempDirectory("concurrent_second");
    auto first_config = MakeConfig(first_server.Port(), first_workspace);
    auto second_config = MakeConfig(second_server.Port(), second_workspace);
    first_config.agent.response_header_timeout_seconds = 10;
    second_config.agent.response_header_timeout_seconds = 10;

    first_server.Enqueue({200, "text/event-stream", TextSse("first done"), 600});
    second_server.Enqueue({200, "text/event-stream", TextSse("second done"), 600});

    spark::SessionJournal first_journal;
    spark::SessionJournal second_journal;
    Check(first_journal.Open(first_config.storage.data_dir, first_workspace).ok,
          "first concurrent journal opens");
    Check(second_journal.Open(second_config.storage.data_dir, second_workspace).ok,
          "second concurrent journal opens");

    bool first_ok = false;
    bool second_ok = false;
    std::string first_error;
    std::string second_error;
    const auto started = std::chrono::steady_clock::now();
    std::thread first_thread([&] {
        spark::Agent agent(first_config, &first_journal, nullptr);
        spark::AgentCallbacks callbacks;
        callbacks.on_error = [&](const std::string& error) { first_error = error; };
        first_ok = agent.RunTurn("first", [](const spark::ApprovalRequest&) { return true; }, callbacks, nullptr).ok;
    });
    std::thread second_thread([&] {
        spark::Agent agent(second_config, &second_journal, nullptr);
        spark::AgentCallbacks callbacks;
        callbacks.on_error = [&](const std::string& error) { second_error = error; };
        second_ok = agent.RunTurn("second", [](const spark::ApprovalRequest&) { return true; }, callbacks, nullptr).ok;
    });
    first_thread.join();
    second_thread.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    Check(first_ok, "first concurrent turn completes");
    Check(second_ok, "second concurrent turn completes");
    if (!first_ok) std::cerr << "first concurrent error: " << first_error << "\n";
    if (!second_ok) std::cerr << "second concurrent error: " << second_error << "\n";
    Check(elapsed < 1000, "two independent 600ms turns overlap instead of running sequentially");
    RemoveDirectory(first_workspace);
    RemoveDirectory(second_workspace);
}

void TestSubAgentManagerRun() {
    FakeServer server;
    const std::string workspace = TempDirectory("subagent");
    const auto config = MakeConfig(server.Port(), workspace);
    server.Enqueue({200, "text/event-stream", TextSse("child done")});

    std::mutex callback_mutex;
    std::vector<std::string> created_ids;
    std::vector<std::string> finished_ids;
    spark::SubAgentCallbacks callbacks;
    callbacks.on_created = [&](const spark::SubAgentInfo& info) {
        std::lock_guard<std::mutex> lock(callback_mutex);
        created_ids.push_back(info.id);
    };
    callbacks.on_finished = [&](const spark::SubAgentInfo& info, bool success, const std::string&) {
        if (!success) return;
        std::lock_guard<std::mutex> lock(callback_mutex);
        finished_ids.push_back(info.id);
    };

    spark::SubAgentManager manager(std::move(callbacks));
    const auto spawned = manager.Spawn(config, nullptr, config.storage.data_dir, "parent-session", workspace,
                                       "worker", "inspect the workspace",
                                       [](const spark::ApprovalRequest&) { return true; }, nullptr);
    Check(spawned.ok, "subagent spawn succeeds");
    if (!spawned.ok) {
        RemoveDirectory(workspace);
        return;
    }

    const auto waited = manager.Wait(spawned.value, nullptr);
    Check(waited.ok, "subagent wait succeeds");
    if (waited.ok) {
        Check(waited.value.find("child done") != std::string::npos,
              "subagent result is returned to the parent");
    }
    const auto children = manager.List("parent-session");
    Check(children.size() == 1 && children[0].session_path.find("subagents") != std::string::npos,
          "subagent owns an independent journal");
    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        Check(created_ids.size() == 1 && created_ids[0] == spawned.value,
              "subagent creation callback keeps the child id");
        Check(finished_ids.size() == 1 && finished_ids[0] == spawned.value,
              "subagent completion callback keeps the child id");
    }
    RemoveDirectory(workspace);
}

void TestSubAgentMetadataHistory() {
    FakeServer server;
    const std::string workspace = TempDirectory("subagent_metadata");
    auto config = MakeConfig(server.Port(), workspace);
    config.skills.enabled = false;
    config.active.reasoning_effort = "high";
    server.Enqueue({200, "text/event-stream", TextSse("first child answer"), 75});
    server.Enqueue({200, "text/event-stream", TextSse("second child answer")});
    bool created_metadata = false;
    bool finished_metadata = false;
    spark::SubAgentCallbacks callbacks;
    callbacks.on_created = [&](const spark::SubAgentInfo& info) {
        const auto history = spark::SubAgentManager::LoadHistory(config.storage.data_dir, info.parent_session_key);
        created_metadata = history.size() == 1 && history[0].id == info.id &&
                           history[0].state == spark::SubAgentState::Running &&
                           history[0].model == "test-model" && history[0].reasoning_effort == "high";
    };
    callbacks.on_finished = [&](const spark::SubAgentInfo& info, bool, const std::string&) {
        const auto history = spark::SubAgentManager::LoadHistory(config.storage.data_dir, info.parent_session_key);
        finished_metadata = history.size() == 1 && history[0].id == info.id &&
                            history[0].state == spark::SubAgentState::Completed &&
                            history[0].elapsed_ms == info.elapsed_ms;
        throw std::runtime_error("UI publication failed");
    };
    std::vector<spark::SubAgentInfo> first;
    std::vector<spark::SubAgentInfo> second;
    {
        spark::SubAgentManager manager(std::move(callbacks));
        const auto spawned = manager.Spawn(config, nullptr, config.storage.data_dir, "parent-a", workspace,
                                           "reader", std::string(8000, 't'), {}, nullptr);
        Check(spawned.ok && manager.Wait(spawned.value, nullptr).ok, "metadata child completes");
        Check(created_metadata && finished_metadata, "child metadata is durable before UI publication");
        first = manager.List("parent-a");
        config.active.reasoning_effort = "low";
        const auto other = manager.Spawn(config, nullptr, config.storage.data_dir, "parent-b", workspace,
                                         "other reader", "inspect another task", {}, nullptr);
        Check(other.ok && manager.Wait(other.value, nullptr).ok, "second parent child completes");
        second = manager.List("parent-b");
    }
    if (!first.empty() && !second.empty()) {
        std::error_code error;
        Check(spark::fs::remove(spark::Utf8ToWide(first[0].session_path), error) && !error,
              "restoration fixture has no child journal");
        const auto restored = spark::SubAgentManager::LoadHistory(config.storage.data_dir, "parent-a");
        const auto other = spark::SubAgentManager::LoadHistory(config.storage.data_dir, "parent-b");
        Check(restored.size() == 1 && other.size() == 1 &&
                  spark::SubAgentManager::LoadHistory(config.storage.data_dir, "unknown-parent").empty(),
              "history is scoped to each parent without opening child journals");
        if (restored.size() == 1 && other.size() == 1) {
            Check(restored[0].id == first[0].id && restored[0].parent_session_key == "parent-a" &&
                      restored[0].name == first[0].name && restored[0].session_path == first[0].session_path &&
                      restored[0].model == first[0].model && restored[0].reasoning_effort == "high" &&
                      other[0].id == second[0].id && other[0].reasoning_effort == "low",
                  "restored child identity, session and selection match the originating config");
            Check(!restored[0].task.empty() && restored[0].task.size() <= 1024 &&
                      first[0].task.compare(0, restored[0].task.size(), restored[0].task) == 0 &&
                      restored[0].output.empty(),
                  "history keeps a bounded task preview and does not restore transcript output");
            Check(restored[0].state == spark::SubAgentState::Completed &&
                      restored[0].started_at == first[0].started_at &&
                      restored[0].finished_at == first[0].finished_at &&
                      restored[0].started_tick_ms == first[0].started_tick_ms &&
                      restored[0].elapsed_ms == first[0].elapsed_ms && restored[0].elapsed_ms >= 50,
                  "history retains terminal state and monotonic elapsed timing");
        }
    }
    RemoveDirectory(workspace);
}

void TestSubAgentHistoryBounds() {
    FakeServer server;
    const std::string workspace = TempDirectory("subagent_history_bounds");
    auto config = MakeConfig(server.Port(), workspace);
    config.skills.enabled = false;
    server.Enqueue({200, "text/event-stream", TextSse("seed")});
    {
        spark::SubAgentManager manager;
        const auto spawned = manager.Spawn(config, nullptr, config.storage.data_dir, "parent", workspace,
                                           "reader", "seed history", {}, nullptr);
        Check(spawned.ok && manager.Wait(spawned.value, nullptr).ok, "history bounds seed completes");
    }
    std::string history_path;
    for (const auto& entry : spark::fs::directory_iterator(
             spark::Utf8ToWide(spark::JoinPath(config.storage.data_dir, "subagents")))) {
        if (entry.path().filename().wstring().find(L"history-") == 0) {
            history_path = spark::WideToUtf8(entry.path().wstring());
            break;
        }
    }
    Check(!history_path.empty(), "child writes a compact history file");
    if (!history_path.empty()) {
        const auto contents = spark::ReadFileUtf8(history_path);
        Check(contents.has_value(), "compact history is readable");
        if (contents.has_value()) {
            auto document = nlohmann::json::parse(*contents);
            auto record = document["children"][0];
            document["children"] = nlohmann::json::array();
            for (int index = 0; index < 90; ++index) {
                record["id"] = "child-" + std::to_string(index);
                document["children"].push_back(record);
            }
            std::string error;
            Check(spark::WriteFileUtf8Atomic(history_path, document.dump(), &error), "oversized record list is written");
            const auto bounded = spark::SubAgentManager::LoadHistory(config.storage.data_dir, "parent");
            Check(bounded.size() == 64 && bounded.front().id == "child-26" && bounded.back().id == "child-89",
                  "history loads only the newest bounded set of records");
            server.Enqueue({200, "text/event-stream", TextSse("next")});
            {
                spark::SubAgentManager manager;
                const auto spawned = manager.Spawn(config, nullptr, config.storage.data_dir, "parent", workspace,
                                                   "reader", "append history", {}, nullptr);
                Check(spawned.ok && manager.Wait(spawned.value, nullptr).ok, "child updates restored history");
                const auto updated = spark::SubAgentManager::LoadHistory(config.storage.data_dir, "parent");
                const auto persisted = spark::ReadFileUtf8(history_path);
                Check(updated.size() == 64 && updated.front().id == "child-27" &&
                          updated.back().id == spawned.value && persisted.has_value() &&
                          persisted->size() <= 256 * 1024,
                      "history publication preserves existing children and evicts the oldest record");
            }
            document["children"].back()["parent_session_key"] = "unrelated-parent";
            document["children"][88]["state"] = "invalid-state";
            document["children"][87]["model"] = 42;
            document["children"][86]["elapsed_ms"] = -1;
            Check(spark::WriteFileUtf8Atomic(history_path, document.dump(), &error), "invalid history entries are written");
            const auto filtered = spark::SubAgentManager::LoadHistory(config.storage.data_dir, "parent");
            Check(filtered.size() == 60 &&
                      std::all_of(filtered.begin(), filtered.end(), [](const spark::SubAgentInfo& info) {
                          return info.parent_session_key == "parent" && info.id != "child-89" &&
                                 info.id != "child-88" && info.id != "child-87" && info.id != "child-86";
                      }),
                  "history skips foreign and malformed entries independently");
            Check(spark::WriteFileUtf8Atomic(history_path, std::string(256 * 1024 + 1, 'x'), &error),
                  "oversized history file is written");
            Check(spark::SubAgentManager::LoadHistory(config.storage.data_dir, "parent").empty(),
                  "history rejects files beyond its byte cap");
            Check(spark::WriteFileUtf8Atomic(history_path, "{", &error), "corrupt history is written");
            Check(spark::SubAgentManager::LoadHistory(config.storage.data_dir, "parent").empty(),
                  "corrupt history fails closed without throwing");
        }
    }
    RemoveDirectory(workspace);
}

void TestSubAgentCallbackForwarding() {
    FakeServer server;
    const std::string workspace = TempDirectory("subagent_callbacks");
    auto config = MakeConfig(server.Port(), workspace);
    config.skills.enabled = false;
    config.active.reasoning_effort = "high";
    std::string error;
    Check(spark::WriteFileUtf8Atomic(spark::JoinPath(workspace, "hello.txt"), "child tool output", &error),
          "child tool fixture is written");
    const nlohmann::json thinking = {{"choices", nlohmann::json::array({
        {{"delta", {{"reasoning_content", "private child reasoning"}}}, {"finish_reason", nullptr}}
    })}};
    const nlohmann::json tools = {{"choices", nlohmann::json::array({
        {{"delta", {{"tool_calls", nlohmann::json::array({
            {{"index", 0}, {"id", "read"}, {"function", {
                {"name", "read_file"}, {"arguments", R"({"path":"hello.txt"})"}
            }}},
            {{"index", 1}, {"id", "missing"}, {"function", {
                {"name", "read_file"}, {"arguments", R"({"path":"missing.txt"})"}
            }}}
        })}}}, {"finish_reason", "tool_calls"}}
    })}, {"usage", {{"prompt_tokens", 31}, {"completion_tokens", 7},
                    {"prompt_tokens_details", {{"cached_tokens", 9}}}}}};
    server.Enqueue({200, "text/event-stream", Sse(thinking) + Sse(tools) + "data: [DONE]\n\n"});
    server.Enqueue({200, "text/event-stream",
                    Sse({{"choices", nlohmann::json::array()}, {"usage", {
                        {"prompt_tokens", 41}, {"completion_tokens", 3},
                        {"prompt_tokens_details", {{"cached_tokens", 11}}}
                    }}}) + TextSse("answer only")});
    std::string reasoning;
    std::string text;
    std::vector<spark::ToolCall> starts;
    std::vector<spark::ToolCall> finishes;
    std::vector<spark::ToolResult> results;
    std::vector<spark::TokenUsage> usages;
    bool matching_info = true;
    const auto check_info = [&](const spark::SubAgentInfo& info) {
        matching_info = matching_info && info.parent_session_key == "parent" && info.name == "reader" &&
                        info.model == "test-model" && info.reasoning_effort == "high" &&
                        info.state == spark::SubAgentState::Running && !info.id.empty();
    };
    spark::SubAgentCallbacks callbacks;
    callbacks.on_reasoning_delta = [&](const spark::SubAgentInfo& info, const std::string& delta) {
        check_info(info);
        reasoning += delta;
    };
    callbacks.on_text_delta = [&](const spark::SubAgentInfo& info, const std::string& delta) {
        check_info(info);
        text += delta;
    };
    callbacks.on_tool_started = [&](const spark::SubAgentInfo& info, const spark::ToolCall& call) {
        check_info(info);
        starts.push_back(call);
    };
    callbacks.on_tool_finished = [&](const spark::SubAgentInfo& info, const spark::ToolCall& call,
                                      const spark::ToolResult& result) {
        check_info(info);
        finishes.push_back(call);
        results.push_back(result);
    };
    callbacks.on_usage = [&](const spark::SubAgentInfo& info, const spark::TokenUsage& usage) {
        check_info(info);
        usages.push_back(usage);
    };
    {
        spark::SubAgentManager manager(std::move(callbacks));
        const auto spawned = manager.Spawn(config, nullptr, config.storage.data_dir, "parent", workspace,
                                           "reader", "read the two files", {}, nullptr);
        Check(spawned.ok && manager.Wait(spawned.value, nullptr).ok, "child event turn completes");
        const auto children = manager.List("parent");
        Check(matching_info && reasoning == "private child reasoning" && text == "answer only" &&
                  children.size() == 1 && children[0].output == "answer only",
              "reasoning and answer deltas stay separate and carry child metadata");
        const std::vector<spark::ToolCall> expected = {
            {"read", "read_file", R"({"path":"hello.txt"})"},
            {"missing", "read_file", R"({"path":"missing.txt"})"},
        };
        Check(nlohmann::json(starts) == nlohmann::json(expected) &&
                  nlohmann::json(finishes) == nlohmann::json(expected),
              "parallel child tool events preserve ids, names and argument previews");
        Check(results.size() == 2 && results[0].ok && !results[1].ok &&
                  results[0].output.find("child tool output") != std::string::npos &&
                  !results[1].output.empty(),
              "child tool finishes forward both success and failure outputs");
        Check(usages.size() == 2 && usages[0].input_tokens == 31 && usages[0].output_tokens == 7 &&
                  usages[0].cached_input_tokens == 9 && usages[1].input_tokens == 41 &&
                  usages[1].output_tokens == 3 && usages[1].cached_input_tokens == 11,
              "child usage forwards each provider request including cache counts");
    }
    RemoveDirectory(workspace);
}

void TestSubAgentApprovalAfterParentTurn() {
    FakeServer server;
    const std::string workspace = TempDirectory("subagent_approval");
    const auto config = MakeConfig(server.Port(), workspace);
    spark::SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, workspace).ok, "delegating parent journal opens");
    const auto tool_reply = [](const std::string& id, const std::string& name, const nlohmann::json& arguments) {
        return Sse({{"choices", nlohmann::json::array({
            {{"delta", {{"tool_calls", nlohmann::json::array({
                {{"index", 0}, {"id", id}, {"function", {{"name", name}, {"arguments", arguments.dump()}}}}
            })}}}, {"finish_reason", "tool_calls"}}
        })}}) + "data: [DONE]\n\n";
    };
    server.Enqueue({200, "text/event-stream", tool_reply("spawn", "spawn_agent",
                                                        {{"task", "write child.txt"}, {"name", "writer"}})});
    server.Enqueue({200, "text/event-stream", TextSse("parent finished")});
    server.Enqueue({200, "text/event-stream", tool_reply("write", "write_file",
                                                        {{"path", "child.txt"}, {"content", "child output"}})});
    server.Enqueue({200, "text/event-stream", TextSse("child finished")});
    std::mutex mutex;
    std::condition_variable condition;
    bool parent_finished = false;
    std::string child_id;
    int parent_approvals = 0;
    int child_approvals = 0;
    int approvals = 0;
    spark::SubAgentCallbacks child_callbacks;
    child_callbacks.on_created = [&](const spark::SubAgentInfo& info) { child_id = info.id; };
    child_callbacks.on_phase = [&](const spark::SubAgentInfo&, spark::AgentPhase phase, const std::string&) {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return parent_finished; });
        if (phase == spark::AgentPhase::WaitingApproval) ++child_approvals;
    };
    {
        spark::SubAgentManager manager(std::move(child_callbacks));
        {
            spark::Agent parent(config, &journal, nullptr, &manager);
            spark::AgentCallbacks callbacks;
            callbacks.on_phase = [&](spark::AgentPhase phase, const std::string&) {
                if (phase == spark::AgentPhase::WaitingApproval) ++parent_approvals;
            };
            const auto result = parent.RunTurn("delegate a file write", [&](const spark::ApprovalRequest&) {
                ++approvals;
                return true;
            }, callbacks, nullptr);
            Check(result.ok, "parent turn finishes while its child is waiting");
        }
        Check(manager.HasRunning(), "a child keeps shared services alive after the parent turn finishes");
        {
            std::lock_guard<std::mutex> lock(mutex);
            parent_finished = true;
        }
        condition.notify_all();
        const auto waited = manager.Wait(child_id, nullptr);
        Check(!manager.HasRunning(), "a completed child releases shared services for configuration reload");
        Check(waited.ok && waited.value.find("child finished") != std::string::npos,
              "child still runs tools after the parent Agent and callbacks have been destroyed");
        Check(approvals == 1 && child_approvals == 1 && parent_approvals == 0,
              "child approval belongs to the child phase stream only");
        Check(spark::ReadFileUtf8(spark::JoinPath(workspace, "child.txt")).value_or("") == "child output",
              "child tool inherits the parent's workspace and approval policy");
        const auto requests = server.Requests();
        if (!requests.empty()) {
            const auto body = nlohmann::json::parse(RequestBody(requests.front()));
            bool catalogue = false;
            bool selection = false;
            for (const auto& tool : body["tools"]) {
                const auto& function = tool["function"];
                if (function["name"] == "list_models") catalogue = true;
                if (function["name"] == "spawn_agent") {
                    const auto& properties = function["parameters"]["properties"];
                    selection = properties.contains("provider") && properties.contains("model") &&
                                properties.contains("reasoning_effort");
                }
            }
            Check(catalogue && selection, "parent receives model discovery and independent child model selection");
        }
    }
    RemoveDirectory(workspace);
}

void TestSubAgentWorkerException() {
    FakeServer server;
    const std::string workspace = TempDirectory("subagent_exception");
    const auto config = MakeConfig(server.Port(), workspace);
    const nlohmann::json event = {{"choices", nlohmann::json::array({
        {{"delta", {{"tool_calls", nlohmann::json::array({
            {{"index", 0}, {"id", "write"}, {"function", {
                {"name", "write_file"}, {"arguments", R"({"path":"child.txt","content":"test"})"}
            }}}
        })}}}, {"finish_reason", "tool_calls"}}
    })}};
    server.Enqueue({200, "text/event-stream", Sse(event) + "data: [DONE]\n\n"});
    std::string error;
    bool finished = false;
    spark::SubAgentCallbacks callbacks;
    callbacks.on_error = [&](const spark::SubAgentInfo&, const std::string& value) { error = value; };
    callbacks.on_finished = [&](const spark::SubAgentInfo&, bool success, const std::string&) { finished = !success; };
    {
        spark::SubAgentManager manager(std::move(callbacks));
        const auto spawned = manager.Spawn(config, nullptr, config.storage.data_dir, "parent", workspace,
                                           "writer", "write a file", [](const spark::ApprovalRequest&) -> bool {
                                               throw std::runtime_error("approval callback failed");
                                           }, nullptr);
        Check(spawned.ok, "failing child starts");
        const auto waited = manager.Wait(spawned.value, nullptr);
        Check(!waited.ok && waited.error.find("approval callback failed") != std::string::npos,
              "child exception returns an error instead of terminating the application");
        const auto children = manager.List("parent");
        Check(finished && error.find("approval callback failed") != std::string::npos &&
                  children.size() == 1 && children[0].state == spark::SubAgentState::Failed,
              "child exception publishes a terminal state and an error event");
        const auto history = spark::SubAgentManager::LoadHistory(config.storage.data_dir, "parent");
        Check(history.size() == 1 && history[0].state == spark::SubAgentState::Failed &&
                  history[0].error.find("approval callback failed") != std::string::npos,
              "child worker failure survives metadata restoration");
        spark::SessionJournal restored;
        Check(restored.Resume(children[0].session_path).ok &&
                  restored.State().metadata["turn_errors"].is_array() &&
                  !restored.State().metadata["turn_errors"].empty(),
              "child worker failure survives a journal reload");
    }
    RemoveDirectory(workspace);
}

void TestSubAgentModelSelection() {
    FakeServer parent_server;
    FakeServer review_server;
    FakeServer code_server;
    const std::string workspace = TempDirectory("subagent_model_selection");
    auto config = MakeConfig(parent_server.Port(), workspace);
    config.skills.enabled = false;
    config.active.reasoning_effort = "high";
    auto review = MakeConfig(review_server.Port(), workspace).providers.front();
    review.id = "review-provider";
    review.api_key = "review-secret";
    review.models = {{"worker-model", "Review model", {true, true, false, true, {"low", "high"}, "low", 8192}}};
    auto code = MakeConfig(code_server.Port(), workspace).providers.front();
    code.id = "code-provider";
    code.protocol = "responses";
    code.api_key = "code-secret";
    code.models.front().id = "worker-model";
    config.providers.push_back(review);
    config.providers.push_back(code);
    const auto original = spark::ConfigToJson(config);
    const auto tool_reply = [](const nlohmann::json& calls) {
        return Sse({{"choices", nlohmann::json::array({
            {{"delta", {{"tool_calls", calls}}}, {"finish_reason", "tool_calls"}}
        })}}) + "data: [DONE]\n\n";
    };
    const auto tool = [](int index, const std::string& id, const std::string& name, const nlohmann::json& arguments) {
        return nlohmann::json{{"index", index}, {"id", id},
                              {"function", {{"name", name}, {"arguments", arguments.dump()}}}};
    };
    parent_server.Enqueue({200, "text/event-stream", tool_reply(nlohmann::json::array({
        tool(0, "catalogue", "list_models", nlohmann::json::object())
    }))});
    parent_server.Enqueue({200, "text/event-stream", tool_reply(nlohmann::json::array({
        tool(0, "spawn-review", "spawn_agent", {{"name", "reviewer"}, {"task", "Review only."},
            {"provider", "review-provider"}, {"model", "worker-model"}, {"reasoning_effort", "high"}}),
        tool(1, "spawn-code", "spawn_agent", {{"name", "coder"}, {"task", "Code analysis only."},
            {"provider", "code-provider"}, {"model", "worker-model"}}),
    }))});
    parent_server.Enqueue({200, "text/event-stream", TextSse("parent done")});
    review_server.Enqueue({200, "text/event-stream", TextSse("review child done")});
    code_server.Enqueue({200, "text/event-stream",
        Sse({{"type", "response.output_text.delta"}, {"delta", "code child done"}}) +
        Sse({{"type", "response.completed"}, {"response", {{"id", "code-response"}, {"status", "completed"}}}})});
    spark::SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, workspace).ok, "multi-model parent journal opens");
    {
        spark::SubAgentManager manager;
        spark::Agent parent(config, &journal, nullptr, &manager);
        Check(parent.RunTurn("delegate to both configured providers", {}, {}, nullptr).ok,
              "parent discovers models and starts two children");
        const auto children = manager.List(journal.Path());
        Check(children.size() == 2, "two independently selected child sessions are created");
        for (const auto& child : children) {
            const auto waited = manager.Wait(child.id, nullptr);
            const std::string expected = child.provider == review.id ? "review child done" : "code child done";
            Check(waited.ok && waited.value.find(expected) != std::string::npos,
                  "each selected provider returns its own child result");
            Check(child.model == "worker-model" &&
                      child.reasoning_effort == (child.provider == review.id ? "high" : ""),
                  "same model ID across providers has independent reasoning selection");
        }
        const auto restored = spark::SubAgentManager::LoadHistory(config.storage.data_dir, journal.Path());
        Check(restored.size() == 2 && restored[0].provider != restored[1].provider,
              "provider identity is retained when child metadata is restored");
    }
    Check(spark::ConfigToJson(config) == original, "child selection does not change the parent's configuration");
    const auto review_requests = review_server.Requests();
    const auto code_requests = code_server.Requests();
    Check(review_requests.size() == 1 && code_requests.size() == 1, "each child calls only its selected endpoint");
    if (review_requests.size() == 1 && code_requests.size() == 1) {
        const auto review_body = nlohmann::json::parse(RequestBody(review_requests[0]));
        const auto code_body = nlohmann::json::parse(RequestBody(code_requests[0]));
        Check(review_requests[0].find("Authorization: Bearer review-secret") != std::string::npos &&
                  review_body["model"] == "worker-model" && review_body["reasoning_effort"] == "high",
              "review child sends the selected key, model and effort");
        Check(code_requests[0].find("POST /v1/responses ") != std::string::npos &&
                  code_requests[0].find("Authorization: Bearer code-secret") != std::string::npos &&
                  code_body["model"] == "worker-model" && !code_body.contains("reasoning"),
              "non-reasoning child uses its own Responses protocol and clears inherited effort");
        Check(RequestBody(review_requests[0]).find("\"name\":\"spawn_agent\"") == std::string::npos &&
                  RequestBody(code_requests[0]).find("\"name\":\"spawn_agent\"") == std::string::npos,
              "selected-model children still cannot create nested children");
    }
    const auto parent_requests = parent_server.Requests();
    for (const auto& request : parent_requests) {
        Check(nlohmann::json::parse(RequestBody(request))["model"] == "test-model",
              "parent continues using its own model");
    }
    if (parent_requests.size() >= 2) {
        const std::string catalogue_body = RequestBody(parent_requests[1]);
        Check(catalogue_body.find("review-provider") != std::string::npos &&
                  catalogue_body.find("code-provider") != std::string::npos &&
                  catalogue_body.find("review-secret") == std::string::npos &&
                  catalogue_body.find("code-secret") == std::string::npos,
              "model catalogue reaches the parent without exposing other providers' keys");
    }
    RemoveDirectory(workspace);
}

void TestSubAgentSelectionValidation() {
    FakeServer server;
    const std::string workspace = TempDirectory("subagent_selection_validation");
    auto config = MakeConfig(server.Port(), workspace);
    config.providers.front().models.front().capabilities.reasoning = true;
    config.active.reasoning_effort = "high";
    config.providers.front().models.push_back({"alternate-model", "Alternate",
        {true, true, false, true, {"low", "medium"}, "medium", 8192}});
    const auto inherited = spark::ResolveSubAgentSelection(config, {});
    Check(inherited.ok && inherited.value.provider == "local" && inherited.value.model == "test-model" &&
              inherited.value.reasoning_effort == "high", "omitted child selection inherits all parent fields");
    const auto changed = spark::ResolveSubAgentSelection(config, {"", "alternate-model", ""});
    Check(changed.ok && changed.value.reasoning_effort == "medium",
          "another model under the same provider uses its configured default effort");
    Check(!spark::ResolveSubAgentSelection(config, {"", "alternate-model", "ultra"}).ok,
          "unsupported explicit child effort is rejected");
    {
        spark::SubAgentManager manager;
        for (const auto& selection : std::vector<spark::ActiveSelection>{
                 {"missing", "test-model", ""}, {"local", "missing", ""}, {"local", "alternate-model", "ultra"}}) {
            Check(!manager.Spawn(config, nullptr, config.storage.data_dir, "parent", workspace,
                                 "invalid", "No request should run.", {}, nullptr, selection).ok,
                  "invalid model selection is rejected before creating a child");
        }
        Check(manager.List("parent").empty() && !spark::PathExists(config.storage.data_dir) &&
                  server.Requests().empty(), "invalid selection creates no sessions, threads or provider requests");
    }
    for (int index = 0; index < 40; ++index) {
        config.providers.front().models.push_back({"extra-" + std::to_string(index), "Additional model", {}});
    }
    size_t offset = 0;
    size_t count = 0;
    for (int pages = 0; pages < 50; ++pages) {
        const auto page = spark::ConfiguredModelsPage(config, offset, 16);
        Check(page.dump().size() <= 4096 && page.dump().find("primary-secret") == std::string::npos,
              "model catalogue pages have a hard byte cap and omit credentials");
        count += page["models"].size();
        if (page["next_offset"].is_null()) break;
        const size_t next = page["next_offset"].get<size_t>();
        Check(next > offset, "model catalogue pagination advances");
        offset = next;
    }
    Check(count == config.providers.front().models.size(), "pagination discovers every configured model exactly once");
    RemoveDirectory(workspace);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--composer-modes") {
        TestGoalAndPlanMode();
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && std::string(argv[1]) == "--subagent-regression") {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        TestSubAgentMetadataHistory();
        TestSubAgentHistoryBounds();
        TestSubAgentCallbackForwarding();
        TestSubAgentApprovalAfterParentTurn();
        TestSubAgentWorkerException();
        TestSubAgentModelSelection();
        TestSubAgentSelectionValidation();
        return failures == 0 ? 0 : 1;
    }
    const std::string fake_mcp_server = argc > 1 ? argv[1] : "";
    TestProviderToolCallStreaming();
    TestResponsesToolCallStreaming();
    TestResponsesFinalOutputFallback();
    TestResponsesEmptyPayloadDiagnostics();
    TestReasoningReplayAcrossToolCalls();
    TestReasoningReplayForFinishedTurn();
    TestFunctionCallArgumentsPreferFinishedItem();
    TestProviderErrors();
    TestSlowProviderStartup();
    TestMidStreamCancellation();
    TestMaxIterationsFinalizesGracefully();
    TestAgentToolRoundTripAndContext();
    TestParallelToolCallIDs();
    TestCancellationAndModelSwitch();
    TestContinueInterruptedTurn();
    TestAgentRedactsToolOutput();
    TestWriteToolExecutesOnce();
    TestExternalApprovalOverrides();
    TestExternalApprovalQueue();
    TestMcpSessionNetworkPolicy();
    TestGoalAndPlanMode();
    TestDsmlToolCallRoundTrip();
    TestRepeatedAutoCompactionAtSixHundredThousandTokens();
    TestCompactionBeyondOneMillionTokens();
    TestManualCompactCommand();
    if (fake_mcp_server.empty()) {
        std::cerr << "FAIL: fake MCP server path was not provided\n";
        ++failures;
    } else {
        TestMcpStdioLifecycle(fake_mcp_server);
        TestMcpToolFiltering(fake_mcp_server);
    }
    TestSkillInjectionAndReadTool();
    TestConcurrentAgentTurns();
    TestSubAgentManagerRun();
    TestSubAgentMetadataHistory();
    TestSubAgentHistoryBounds();
    TestSubAgentCallbackForwarding();
    TestSubAgentApprovalAfterParentTurn();
    TestSubAgentWorkerException();
    TestSubAgentModelSelection();
    TestSubAgentSelectionValidation();
    if (failures != 0) {
        std::cerr << failures << " agent test(s) failed\n";
        return 1;
    }
    std::cout << "all agent tests passed\n";
    return 0;
}
