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

void ResponsesSummaryRequests() {
    struct Case {
        const char* model;
        bool reasoning;
        const char* effort;
        Json expected;
    };
    const Case cases[] = {
        {"gpt-5.4", true, "medium", {{"effort", "medium"}, {"summary", "auto"}}},
        {"openai/gpt-5.2", true, "high", {{"effort", "high"}, {"summary", "auto"}}},
        {"gpt-6-sol", true, "low", {{"effort", "low"}, {"summary", "auto"}}},
        {"o3", true, "high", {{"effort", "high"}, {"summary", "auto"}}},
        {"o4-mini-2025-04-16", true, "medium", {{"effort", "medium"}, {"summary", "auto"}}},
        {"gpt-5.4", true, "none", {{"effort", "none"}}},
        {"gpt-5.4", true, "off", {{"effort", "off"}}},
        {"gpt-5.4", true, "", {{"summary", "auto"}}},
        {"gpt-5.4", false, "high", nullptr},
        {"deepseek-v4", true, "high", {{"effort", "high"}}},
        {"minimax-m3", true, "high", {{"effort", "high"}}},
        {"gpt-oss-120b", true, "high", {{"effort", "high"}}},
        {"gpt-4o", true, "high", {{"effort", "high"}}},
        {"o3-mini", true, "high", {{"effort", "high"}}},
        {"o1", true, "high", {{"effort", "high"}}},
        {"custom-model", true, "high", {{"effort", "high"}}},
    };
    for (const auto& test : cases) {
        Server server([](Server::Connection& connection, size_t) {
            const auto body = Reply("responses", false);
            connection.Send(Server::Headers(200, body.size()) + body);
        });
        ProviderConfig provider;
        provider.id = "openai";  // A user-defined provider label is not a capability.
        provider.protocol = "responses";
        provider.base_url = server.Url("/v1");
        ModelConfig model;
        model.id = test.model;
        model.capabilities.streaming = false;
        model.capabilities.reasoning = test.reasoning;
        ProviderRequest request;
        request.provider = &provider;
        request.model = &model;
        request.reasoning_effort = test.effort;
        request.messages.push_back({MessageRole::User, "Check summary support"});
        const auto reply = CreateProviderAdapter(provider.protocol)->Complete(request, {}, nullptr);
        const auto requests = server.Requests();
        Check(reply.ok && requests.size() == 1, "summary negotiation completes one local HTTP request");
        if (requests.empty()) continue;
        const auto payload = Json::parse(requests.front().body);
        if (payload.value("reasoning", Json{}) != test.expected) {
            std::cerr << "model=" << test.model << " effort=" << test.effort
                      << " capability=" << test.reasoning << " payload=" << payload.dump() << '\n';
            Check(false, "Responses summary auto is gated by model, reasoning capability and active effort");
        }
    }
}

void ResponsesSummaryFallback(int status, bool unsupported, bool streaming, const std::string& effort) {
    const bool retry = unsupported && (status == 400 || status == 422);
    const auto error = Json{{"error", {
        {"message", unsupported ? "Unsupported parameter: reasoning.summary" : "Invalid reasoning effort"},
        {"param", unsupported ? "reasoning.summary" : "reasoning.effort"},
        {"code", "unsupported_parameter"}}}}.dump();
    Server server([&](Server::Connection& connection, size_t attempt) {
        const auto body = attempt == 0 ? error : Reply("responses", streaming);
        connection.Send(Server::Headers(attempt == 0 ? status : 200, body.size(),
            attempt != 0 && streaming ? "Content-Type: text/event-stream\r\n"
                                      : "Content-Type: application/json\r\n") + body);
    });
    ProviderConfig provider;
    provider.protocol = "responses";
    provider.base_url = server.Url("/v1");
    ModelConfig model;
    model.id = "gpt-5.4";
    model.capabilities.streaming = streaming;
    ProviderRequest request;
    request.provider = &provider;
    request.model = &model;
    request.reasoning_effort = effort;
    request.messages.push_back({MessageRole::User, "Review"});
    std::string displayed;
    ProviderCallbacks callbacks;
    callbacks.on_reasoning_delta = [&](const auto& text) { displayed += text; };
    const auto reply = CreateProviderAdapter(provider.protocol)->Complete(request, callbacks, nullptr);
    const auto requests = server.Requests();
    Check(reply.ok == retry && requests.size() == (retry ? 2u : 1u),
          "only an explicit summary parameter rejection retries once without summary");
    Check(displayed == (retry && streaming ? "Checking" : ""),
          "summary fallback emits only successful response reasoning");
    if (requests.empty()) return;
    auto expected = Json::parse(requests.front().body);
    Check(expected.contains("reasoning") && expected["reasoning"].value("summary", "") == "auto",
          "fallback first requests a supported model summary even with default effort");
    if (expected.contains("reasoning") && expected["reasoning"].is_object()) {
        expected["reasoning"].erase("summary");
        if (expected["reasoning"].empty()) expected.erase("reasoning");
    }
    if (requests.size() == 2) {
        Check(Json::parse(requests.back().body) == expected,
              "summary compatibility retry changes only the optional summary parameter");
    }
}

void ResponsesSummaryRoundTrip(const std::string& delivery, bool observe) {
    const bool streaming = delivery != "json";
    const std::string summary = u8"Checking \u4e16\u754c";
    const Json reasoning = {{"id", "reasoning-1"}, {"type", "reasoning"},
        {"summary", Json::array({{{"type", "summary_text"}, {"text", summary}},
                                {{"type", "summary_text"}, {"text", summary}}})},
        {"encrypted_content", "opaque-fixture"}};
    const Json second_reasoning = {{"id", "reasoning-2"}, {"type", "reasoning"},
        {"summary", Json::array({{{"type", "summary_text"}, {"text", "Next"}}})}};
    const Json raw_reasoning = {{"id", "reasoning-raw"}, {"type", "reasoning"},
        {"content", Json::array({{{"type", "reasoning_text"}, {"text", "Private raw"}}})}};
    const Json message = {{"type", "message"}, {"role", "assistant"},
        {"content", Json::array({{{"type", "output_text"}, {"text", "Reviewed"}}})}};
    const Json output = Json::array({reasoning, second_reasoning, raw_reasoning, message});
    const Json response = {{"id", "summary-fixture"}, {"status", "completed"}, {"output", output}};
    std::string body;
    if (!streaming) {
        body = response.dump();
    } else {
        if (delivery == "delta" || delivery == "partial" || delivery == "done" || delivery == "unindexed") {
            for (int index = 0; index < 2; ++index) {
                const auto event = [&](const char* type, const char* field, const std::string& text) {
                    Json value = {{"type", type}, {"item_id", "reasoning-1"},
                        {"output_index", 0}, {"summary_index", index}, {field, text}};
                    if (delivery == "unindexed") value.erase("output_index");
                    return Event(type, value);
                };
                if (delivery != "done") {
                    body += event("response.reasoning_summary_text.delta", "delta", "Checking ");
                    if (delivery != "partial") {
                        body += event("response.reasoning_summary_text.delta", "delta", u8"\u4e16\u754c");
                    }
                }
                body += event("response.reasoning_summary_text.done", "text", summary);
            }
            body += Event("response.output_text.delta",
                          {{"type", "response.output_text.delta"}, {"delta", "Reviewed"}});
        }
        if (delivery != "completed") {
            for (size_t index = 0; index < (delivery == "unindexed" ? 3u : 2u); ++index) {
                Json event = {{"type", "response.output_item.done"}, {"output_index", index},
                              {"item", output[index]}};
                if (delivery == "unindexed") event.erase("output_index");
                body += Event("response.output_item.done", event);
            }
        }
        auto completed = response;
        if (delivery == "unindexed") completed.erase("output");
        body += Event("response.completed", {{"type", "response.completed"}, {"response", completed}});
    }
    Server server([&](Server::Connection& connection, size_t attempt) {
        const auto reply = attempt == 0 ? body : (streaming
            ? Event("response.completed", {{"type", "response.completed"},
                {"response", {{"status", "completed"}, {"output", Json::array({message})}}}})
            : Reply("responses", false));
        connection.Send(Server::Headers(200, reply.size(),
            streaming ? "Content-Type: text/event-stream\r\n" : "Content-Type: application/json\r\n"));
        for (size_t offset = 0; offset < reply.size(); offset += 11) {
            if (!connection.Send(std::string_view(reply).substr(offset, 11))) return;
        }
    });
    ProviderConfig provider;
    provider.protocol = "responses";
    provider.base_url = server.Url("/v1");
    ModelConfig model;
    model.id = "gpt-5.4";
    model.capabilities.streaming = streaming;
    ProviderRequest request;
    request.provider = &provider;
    request.model = &model;
    request.reasoning_effort = "medium";
    request.messages.push_back({MessageRole::User, "Review"});
    std::string displayed, reasoning_before_answer;
    std::vector<std::string> deltas;
    ProviderCallbacks callbacks;
    if (observe) callbacks.on_reasoning_delta = [&](const auto& text) {
        displayed += text;
        deltas.push_back(text);
    };
    callbacks.on_text_delta = [&](const auto&) { reasoning_before_answer = displayed; };
    auto adapter = CreateProviderAdapter(provider.protocol);
    const auto first = adapter->Complete(request, callbacks, nullptr);
    const std::vector<std::string> expected_items = {
        reasoning.dump(), second_reasoning.dump(), raw_reasoning.dump()};
    Check(first.ok && first.value.text == "Reviewed", "summary delivery preserves the assistant answer");
    Check(displayed == (observe ? summary + summary + "Next" : ""),
          "summary delta/done/item/completed/JSON deliver each indexed part once");
    if (observe) {
        const bool incremental = delivery == "delta" || delivery == "partial" || delivery == "unindexed";
        const bool early = incremental || delivery == "done";
        Check(reasoning_before_answer == (early ? summary + summary : summary + summary + "Next"),
              "streamed summary delta/done emits before later output-item fallback");
        const std::vector<std::string> expected = incremental
            ? std::vector<std::string>{"Checking ", u8"\u4e16\u754c", "Checking ", u8"\u4e16\u754c", "Next"}
            : std::vector<std::string>{summary, summary, "Next"};
        Check(deltas == expected, "summary callbacks preserve incremental chunks and done-only suffixes");
    }
    Check(first.ok && first.value.reasoning_items == expected_items && first.value.reasoning_text.empty(),
          "summary parsing retains unique opaque reasoning items without fabricating raw reasoning");
    if (!first.ok) return;
    ChatMessage assistant;
    assistant.role = MessageRole::Assistant;
    assistant.content = first.value.text;
    assistant.reasoning_text = first.value.reasoning_text;
    assistant.reasoning_items = first.value.reasoning_items;
    request.messages.push_back(assistant);
    request.messages.push_back({MessageRole::User, "Continue"});
    const auto next = adapter->Complete(request, {}, nullptr);
    Check(next.ok, "summary response accepts a reasoning-preserving follow-up");
    const auto requests = server.Requests();
    if (requests.size() != 2) { Check(false, "summary round trip has two requests"); return; }
    Json replayed = Json::array();
    const auto payload = Json::parse(requests.back().body);
    for (const auto& item : payload["input"]) {
        if (item.value("type", "") == "reasoning") replayed.push_back(item);
    }
    Check(replayed == Json::array({reasoning, second_reasoning, raw_reasoning}),
          "follow-up replays exact reasoning summary/content/encrypted fields once");
}

void ResponsesRawReasoning() {
    const auto body = Event("response.reasoning_text.delta",
            {{"type", "response.reasoning_text.delta"}, {"delta", "Private "}}) +
        Event("response.reasoning_text.delta",
            {{"type", "response.reasoning_text.delta"}, {"delta", "raw"}}) +
        Event("response.reasoning_text.done",
            {{"type", "response.reasoning_text.done"}, {"text", "Private raw"}}) +
        Reply("responses", true);
    Server server([&](Server::Connection& connection, size_t) {
        connection.Send(Server::Headers(200, body.size(), "Content-Type: text/event-stream\r\n") + body);
    });
    ProviderConfig provider;
    provider.protocol = "responses";
    provider.base_url = server.Url("/v1");
    ModelConfig model;
    model.id = "deepseek-v4";
    ProviderRequest request;
    request.provider = &provider;
    request.model = &model;
    request.reasoning_effort = "high";
    request.messages.push_back({MessageRole::User, "Review"});
    std::string displayed;
    ProviderCallbacks callbacks;
    callbacks.on_reasoning_delta = [&](const auto& text) { displayed += text; };
    const auto reply = CreateProviderAdapter(provider.protocol)->Complete(request, callbacks, nullptr);
    Check(reply.ok && reply.value.reasoning_text == "Private raw" && displayed == "Private rawChecking",
          "third-party raw reasoning stays separate from the displayed summary");
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
        ResponsesSummaryRequests();
        for (bool streaming : {false, true}) {
            ResponsesSummaryFallback(400, true, streaming, "medium");
            ResponsesSummaryFallback(422, true, streaming, "");
            ResponsesSummaryFallback(400, false, streaming, "medium");
            ResponsesSummaryFallback(500, true, streaming, "medium");
        }
        for (const std::string delivery : {"delta", "partial", "done", "item", "completed", "json", "unindexed"}) {
            ResponsesSummaryRoundTrip(delivery, true);
            ResponsesSummaryRoundTrip(delivery, false);
        }
        ResponsesRawReasoning();
        CatalogAndProbe();
        ConcurrentCancellation();
    } catch (const std::exception& error) {
        Check(false, error.what());
    }
    std::error_code error;
    fs::remove_all(root, error);
    Check(!error, "remove native provider fixtures");
    if (!failures) std::cout << "PASS: native provider protocols, streaming, images, reasoning summaries and tool follow-up\n";
    return failures ? 1 : 0;
}
