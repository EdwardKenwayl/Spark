#include "loopback_http.h"

#include "core/provider.h"
#include "core/provider_models.h"
#include "core/provider_probe.h"
#include "platform/http.h"

#include "platform/filesystem.h"
#include <future>
#include <iostream>

namespace {
using namespace spark;
using Server = spark::test::LoopbackHttpServer;
using Json = nlohmann::json;
int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

std::string Event(const std::string& type, const Json& value) {
    return "event: " + type + "\ndata: " + value.dump() + "\n\n";
}

std::string Reply(const std::string& protocol, bool streaming) {
    const std::string text = u8"Hi \u4e16\u754c";
    if (protocol == "responses") {
        if (!streaming) return Json{{"id", "response-fixture"}, {"status", "completed"},
            {"output", Json::array({{{"type", "message"}, {"role", "assistant"},
                {"content", Json::array({{{"type", "output_text"}, {"text", text}}})}}})}}.dump();
        return Event("response.reasoning_summary_text.delta",
                {{"type", "response.reasoning_summary_text.delta"}, {"delta", "Checking"}}) +
            Event("response.output_text.delta", {{"type", "response.output_text.delta"}, {"delta", text}}) +
            Event("response.completed", {{"type", "response.completed"},
                {"response", {{"id", "response-fixture"}, {"status", "completed"},
                    {"usage", {{"input_tokens", 20}, {"output_tokens", 4}}}}}});
    }
    if (protocol == "anthropic_messages") {
        if (!streaming) return Json{{"id", "message-fixture"}, {"stop_reason", "end_turn"},
            {"content", Json::array({{{"type", "text"}, {"text", text}}})}}.dump();
        return Event("content_block_delta", {{"type", "content_block_delta"}, {"index", 0},
                {"delta", {{"type", "thinking_delta"}, {"thinking", "Checking"}}}}) +
            Event("content_block_delta", {{"type", "content_block_delta"}, {"index", 1},
                {"delta", {{"type", "text_delta"}, {"text", text}}}}) +
            Event("message_delta", {{"type", "message_delta"},
                {"delta", {{"stop_reason", "end_turn"}}}, {"usage", {{"output_tokens", 4}}}}) +
            Event("message_stop", {{"type", "message_stop"}});
    }
    if (!streaming) return Json{{"id", "chat-fixture"}, {"choices", Json::array({
        {{"message", {{"role", "assistant"}, {"content", text}}}, {"finish_reason", "stop"}}})}}.dump();
    return "data: " + Json{{"choices", Json::array({{{"delta",
        {{"reasoning_content", "Checking"}, {"content", text}}}, {"finish_reason", nullptr}}})}}.dump() +
        "\n\ndata: " + Json{{"choices", Json::array({{{"delta", Json::object()}, {"finish_reason", "stop"}}})},
            {"usage", {{"prompt_tokens", 20}, {"completion_tokens", 4}}}}.dump() + "\n\ndata: [DONE]\n\n";
}

void ProtocolRoundTrip(const std::string& protocol, bool streaming, const std::string& image) {
    const auto body = Reply(protocol, streaming);
    Server server([&](Server::Connection& connection, size_t) {
        connection.Send(Server::Headers(200, body.size() + (streaming ? 64 : 0),
            streaming ? "Content-Type: text/event-stream\r\n" : "Content-Type: application/json\r\n"));
        for (size_t offset = 0; offset < body.size(); offset += 11) {
            if (!connection.Send(std::string_view(body).substr(offset, 11))) return;
        }
        if (streaming) connection.Hold();
    });
    ProviderConfig provider;
    provider.id = "fixture";
    provider.protocol = protocol;
    provider.base_url = server.Url("/v1");
    provider.api_key = "synthetic-key";
    ModelConfig model;
    model.id = "fixture-model";
    model.capabilities.streaming = streaming;
    ProviderRequest request;
    request.provider = &provider;
    request.model = &model;
    request.request_timeout_seconds = 1;
    request.response_header_timeout_seconds = 2;
    ChatMessage user;
    user.content = u8"Read \u56fe\u7247";
    user.image_paths.push_back(image);
    request.messages.push_back(user);
    std::string text, reasoning;
    int status = 0;
    ProviderCallbacks callbacks;
    callbacks.on_text_delta = [&](const auto& delta) { text += delta; };
    callbacks.on_reasoning_delta = [&](const auto& delta) { reasoning += delta; };
    callbacks.on_http_status = [&](int code) { status = code; };
    const auto reply = CreateProviderAdapter(protocol)->Complete(request, callbacks, nullptr);
    if (!reply.ok) std::cerr << protocol << ": " << reply.error << '\n';
    Check(reply.ok && reply.value.text == u8"Hi \u4e16\u754c" && text == reply.value.text,
          "native provider preserves Unicode text and emits deltas once");
    Check(status == 200, "native provider reports status before the response completes");
    if (streaming) Check(reasoning == "Checking", "native streaming provider emits reasoning deltas");
    const auto requests = server.Requests();
    Check(requests.size() == 1, "a model response is requested once");
    if (requests.empty()) return;
    const auto payload = Json::parse(requests.front().body);
    Check(payload["model"] == model.id && payload["stream"] == streaming,
          "native provider retains model and streaming settings");
    Check(requests.front().body.find("AAEC/w==") != std::string::npos,
          "native provider encodes attachment bytes without text conversion");
    const auto& headers = requests.front().headers;
    const auto credential = headers.find(protocol == "anthropic_messages" ? "x-api-key" : "authorization");
    Check(credential != headers.end() &&
          credential->second == (protocol == "anthropic_messages" ? "synthetic-key" : "Bearer synthetic-key"),
          "native protocol uses the correct authentication header");
}

void ResponsesToolFollowUp() {
    Server server([](Server::Connection& connection, size_t attempt) {
        const Json output = attempt == 0
            ? Json::array({{{"type", "function_call"}, {"id", "item-1"}, {"call_id", "call-1"},
                {"name", "read_file"}, {"arguments", "{\"path\":\"README.md\"}"}}})
            : Json::array({{{"type", "message"}, {"role", "assistant"},
                {"content", Json::array({{{"type", "output_text"}, {"text", "Reviewed"}}})}}});
        const auto body = Json{{"status", "completed"}, {"output", output}}.dump();
        connection.Send(Server::Headers(200, body.size()) + body);
    });
    ProviderConfig provider;
    provider.protocol = "responses";
    provider.base_url = server.Url("/v1");
    ModelConfig model;
    model.id = "fixture-model";
    model.capabilities.streaming = false;
    ProviderRequest request;
    request.provider = &provider;
    request.model = &model;
    request.messages.push_back({MessageRole::User, "Review the readme"});
    auto adapter = CreateProviderAdapter(provider.protocol);
    const auto first = adapter->Complete(request, {}, nullptr);
    Check(first.ok && first.value.tool_calls.size() == 1 && first.value.tool_calls[0].id == "call-1",
          "Responses tool call retains the call id across platforms");
    if (!first.ok || first.value.tool_calls.empty()) return;
    ChatMessage assistant;
    assistant.role = MessageRole::Assistant;
    assistant.tool_calls = first.value.tool_calls;
    request.messages.push_back(assistant);
    ChatMessage tool;
    tool.role = MessageRole::Tool;
    tool.tool_call_id = first.value.tool_calls[0].id;
    tool.content = "Fixture readme contents";
    request.messages.push_back(tool);
    const auto final = adapter->Complete(request, {}, nullptr);
    Check(final.ok && final.value.text == "Reviewed", "native Responses accepts a tool result follow-up");
    const auto requests = server.Requests();
    if (requests.size() != 2) { Check(false, "tool follow-up has two HTTP requests"); return; }
    const auto payload = Json::parse(requests.back().body);
    bool paired = false;
    for (const auto& item : payload["input"]) {
        if (item.value("type", "") == "function_call_output") {
            paired = item.value("call_id", "") == "call-1" && item.value("output", "") == tool.content;
        }
    }
    Check(paired, "tool results serialize with their original call id");
}

void CatalogAndProbe() {
    Server server([](Server::Connection& connection, size_t attempt) {
        const auto body = attempt == 0
            ? Json{{"data", Json::array({{{"id", "fixture-a"}}, {{"id", "fixture-b"}}})}}.dump()
            : Reply("chat_completions", false);
        connection.Send(Server::Headers(200, body.size()) + body);
    });
    ProviderConfig provider;
    provider.id = "fixture";
    provider.base_url = server.Url("/v1");
    provider.api_key = "synthetic-key";
    provider.test_model = "fixture-b";
    provider.models.push_back({"fixture-a", "A", {}});
    provider.models.push_back({"fixture-b", "B", {}});
    for (auto& model : provider.models) model.capabilities.streaming = false;
    const auto models = FetchProviderModelIds(provider);
    Check(models.ok && models.value == std::vector<std::string>{"fixture-a", "fixture-b"},
          "native model catalog fetch uses the provider connection endpoint");
    const auto probe = ProbeProvider(provider);
    Check(probe.ok && probe.value.model_id == "fixture-b" && probe.value.http_status == 200 &&
              probe.value.text == u8"Hi \u4e16\u754c",
          "native hi test preserves Unicode, model selection and HTTP status");
    const auto requests = server.Requests();
    Check(requests.size() == 2 && requests.front().method == "GET" &&
              requests.front().target == "/v1/models" &&
              Json::parse(requests.back().body)["model"] == "fixture-b",
          "catalog and probe serialize their distinct methods and models");
}

void ConcurrentCancellation() {
    Server held([](Server::Connection& connection, size_t) {
        connection.Send(Server::Headers(200, 4096, "Content-Type: text/event-stream\r\n") +
            "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n");
        connection.Hold();
    });
    Server ready([](Server::Connection& connection, size_t) {
        const auto body = Reply("chat_completions", true);
        connection.Send(Server::Headers(200, body.size(), "Content-Type: text/event-stream\r\n") + body);
    });
    ProviderConfig first_provider, second_provider;
    first_provider.base_url = held.Url("/v1");
    second_provider.base_url = ready.Url("/v1");
    ModelConfig model;
    model.id = "fixture-model";
    ProviderRequest first;
    first.provider = &first_provider;
    first.model = &model;
    first.request_timeout_seconds = first.response_header_timeout_seconds = 5;
    first.messages.push_back({MessageRole::User, "First request"});
    ProviderRequest second = first;
    second.provider = &second_provider;
    CancellationToken cancellation;
    auto pending = std::async(std::launch::async, [&] {
        return CreateProviderAdapter("chat_completions")->Complete(first, {}, &cancellation);
    });
    Check(held.WaitForRequests(1), "first concurrent request starts");
    const auto completed = CreateProviderAdapter("chat_completions")->Complete(second, {}, nullptr);
    cancellation.Cancel();
    const auto interrupted = pending.get();
    Check(completed.ok && completed.value.text == u8"Hi \u4e16\u754c" &&
              !interrupted.ok && interrupted.error.find("cancel") != std::string::npos,
          "parallel providers preserve independent output and cancellation state");
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--reject-untrusted-tls") {
        spark::HttpRequest request;
        request.url = argv[2];
        request.method = "GET";
        request.timeout_seconds = request.header_timeout_seconds = 5;
        request.max_attempts = 1;
        const auto response = spark::HttpRequestStreaming(request);
        Check(!response.ok && response.error.find("certificate") != std::string::npos,
              "HTTPS rejects an untrusted local certificate");
        if (!response.ok) std::cout << response.error << '\n';
        return failures ? 1 : 0;
    }
    namespace fs = spark::fs;
    const auto root = fs::temp_directory_path() /
        ("spark-provider-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    try {
        const auto image = spark::PathToUtf8(root / fs::u8path(u8"\u56fe\u7247.png"));
        Check(spark::WriteFileUtf8Atomic(image, std::string("\0\1\2\xff", 4)), "create binary image fixture");
        for (const std::string protocol : {"chat_completions", "responses", "anthropic_messages"}) {
            ProtocolRoundTrip(protocol, false, image);
            ProtocolRoundTrip(protocol, true, image);
        }
        ResponsesToolFollowUp();
        CatalogAndProbe();
        ConcurrentCancellation();
    } catch (const std::exception& error) {
        Check(false, error.what());
    }
    std::error_code error;
    fs::remove_all(root, error);
    Check(!error, "remove native provider fixtures");
    if (!failures) std::cout << "PASS: native provider protocols, streaming, images and tool follow-up\n";
    return failures ? 1 : 0;
}
