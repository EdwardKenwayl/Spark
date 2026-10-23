#include <winsock2.h>

#include "core/agent.h"
#include "core/agent_types.h"
#include "core/agent_instructions.h"
#include "core/common.h"
#include "core/config.h"
#include "core/context_compactor.h"
#include "core/dsml_tool_calls.h"
#include "core/external_agents.h"
#include "core/provider.h"
#include "core/session.h"
#include "core/session_exchange.h"
#include "core/skills.h"
#include "core/tools.h"
#include "platform/window_text.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <thread>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

// Load Winsock locally so parser fixtures need no new core-test link dependency.
class SourceFixtureServer {
public:
    SourceFixtureServer(std::string body, const char* content_type) {
        if (module_ == nullptr || startup_ == nullptr || cleanup_ == nullptr || socket_ == nullptr ||
            close_ == nullptr || bind_ == nullptr || listen_ == nullptr || address_ == nullptr ||
            accept_ == nullptr || recv_ == nullptr || send_ == nullptr || option_ == nullptr) return;
        WSADATA data{};
        started_ = startup_(MAKEWORD(2, 2), &data) == 0;
        if (!started_) return;
        listener_ = socket_(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) return;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.S_un.S_un_b.s_b1 = 127;
        address.sin_addr.S_un.S_un_b.s_b4 = 1;
        int size = sizeof(address);
        if (bind_(listener_, reinterpret_cast<sockaddr*>(&address), size) != 0 ||
            listen_(listener_, 1) != 0 ||
            address_(listener_, reinterpret_cast<sockaddr*>(&address), &size) != 0) return;
        const auto* port_bytes = reinterpret_cast<const unsigned char*>(&address.sin_port);
        port = (port_bytes[0] << 8) | port_bytes[1];
        const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(content_type) +
            "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        worker_ = std::thread([this, response, listener = listener_] {
            const SOCKET client = accept_(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            const DWORD timeout = 5000;
            option_(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            option_(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            char buffer[1024];
            size_t header_end = std::string::npos;
            size_t body_size = 0;
            for (;;) {
                const int count = recv_(client, buffer, sizeof(buffer), 0);
                if (count <= 0) break;
                request_.append(buffer, static_cast<size_t>(count));
                if (header_end == std::string::npos) {
                    header_end = request_.find("\r\n\r\n");
                    if (header_end != std::string::npos) {
                        const std::string headers = spark::LowerAscii(request_.substr(0, header_end));
                        const size_t length = headers.find("content-length:");
                        if (length != std::string::npos) body_size = std::stoul(headers.substr(length + 15));
                    }
                }
                if (header_end != std::string::npos && request_.size() >= header_end + 4 + body_size) break;
            }
            for (size_t offset = 0; offset < response.size();) {
                const int count = send_(client, response.data() + offset,
                                        static_cast<int>(std::min<size_t>(256, response.size() - offset)), 0);
                if (count <= 0) break;
                offset += static_cast<size_t>(count);
            }
            close_(client);
        });
    }

    ~SourceFixtureServer() {
        Finish();
        if (started_) cleanup_();
        if (module_ != nullptr) FreeLibrary(module_);
    }

    nlohmann::json RequestBody() {
        Finish();
        const size_t header_end = request_.find("\r\n\r\n");
        return header_end == std::string::npos ? nlohmann::json{} :
            nlohmann::json::parse(request_.substr(header_end + 4), nullptr, false);
    }

    int port = 0;

private:
    void Finish() {
        if (listener_ != INVALID_SOCKET) {
            close_(listener_);
            listener_ = INVALID_SOCKET;
        }
        if (worker_.joinable()) worker_.join();
    }

    HMODULE module_ = LoadLibraryW(L"ws2_32.dll");
    decltype(&WSAStartup) startup_ = reinterpret_cast<decltype(&WSAStartup)>(GetProcAddress(module_, "WSAStartup"));
    decltype(&WSACleanup) cleanup_ = reinterpret_cast<decltype(&WSACleanup)>(GetProcAddress(module_, "WSACleanup"));
    decltype(&socket) socket_ = reinterpret_cast<decltype(&socket)>(GetProcAddress(module_, "socket"));
    decltype(&closesocket) close_ = reinterpret_cast<decltype(&closesocket)>(GetProcAddress(module_, "closesocket"));
    decltype(&bind) bind_ = reinterpret_cast<decltype(&bind)>(GetProcAddress(module_, "bind"));
    decltype(&listen) listen_ = reinterpret_cast<decltype(&listen)>(GetProcAddress(module_, "listen"));
    decltype(&getsockname) address_ = reinterpret_cast<decltype(&getsockname)>(GetProcAddress(module_, "getsockname"));
    decltype(&accept) accept_ = reinterpret_cast<decltype(&accept)>(GetProcAddress(module_, "accept"));
    decltype(&recv) recv_ = reinterpret_cast<decltype(&recv)>(GetProcAddress(module_, "recv"));
    decltype(&send) send_ = reinterpret_cast<decltype(&send)>(GetProcAddress(module_, "send"));
    decltype(&setsockopt) option_ = reinterpret_cast<decltype(&setsockopt)>(GetProcAddress(module_, "setsockopt"));
    bool started_ = false;
    SOCKET listener_ = INVALID_SOCKET;
    std::thread worker_;
    std::string request_;
};

struct SourceFixtureReply {
    spark::Result<spark::ModelReply> reply;
    nlohmann::json request;
};

SourceFixtureReply CompleteSourceFixture(const char* protocol, bool streaming, const std::string& body,
                                        const std::vector<spark::SourceReference>& history_sources = {}) {
    SourceFixtureServer server(body, streaming ? "text/event-stream" : "application/json");
    Check(server.port != 0, "source fixture loopback server starts");
    if (server.port == 0) return {spark::Result<spark::ModelReply>::Failure("fixture server unavailable"), {}};
    spark::ProviderConfig provider;
    provider.base_url = "http://127.0.0.1:" + std::to_string(server.port) + "/v1";
    provider.api_key = "source-fixture-token";
    spark::ModelConfig model;
    model.id = "source-fixture-model";
    model.capabilities.streaming = streaming;
    spark::ChatMessage history;
    history.role = spark::MessageRole::Assistant;
    history.content = "previous answer";
    history.sources = history_sources;
    spark::ProviderRequest request;
    request.provider = &provider;
    request.model = &model;
    request.messages.push_back(history);
    request.prompt_cache_key = "stable-source-fixture-cache";
    request.request_timeout_seconds = 5;
    request.response_header_timeout_seconds = 5;
    auto adapter = spark::CreateProviderAdapter(protocol);
    auto reply = adapter->Complete(request, {}, nullptr);
    return {std::move(reply), server.RequestBody()};
}

void TestProviderSources() {
    using nlohmann::json;
    const auto sse = [](const json& event) { return "data: " + event.dump() + "\n\n"; };
    const json citation = {{"type", "url_citation"}, {"url", "https://example.test/doc"},
                           {"title", "Provider document"}, {"start_index", 0}, {"end_index", 6}};
    const json message = {{"type", "message"}, {"role", "assistant"}, {"content", json::array({
        {{"type", "output_text"}, {"text", "answer"}, {"annotations", json::array({citation})}}
    })}};
    const json search = {{"type", "web_search_call"}, {"action", {
        {"type", "search"}, {"query", "first query"}, {"queries", json::array({"first query", "second query"})},
        {"sources", json::array({{{"type", "url"}, {"url", "https://example.test/result"}}})}
    }}};
    const json open = {{"type", "web_search_call"}, {"action", {
        {"type", "open_page"}, {"url", "https://example.test/open"}
    }}};
    const json find = {{"type", "web_search_call"}, {"action", {
        {"type", "find"}, {"url", "https://example.test/find"}, {"pattern", "needle"}
    }}};
    const json expected = json::array({
        {{"kind", "web_search"}, {"label", "Web search"}, {"detail", "first query"}},
        {{"kind", "web_search"}, {"label", "Web search"}, {"detail", "second query"}},
        {{"kind", "web_source"}, {"label", "https://example.test/result"}, {"detail", "https://example.test/result"}},
        {{"kind", "web_open"}, {"label", "Open page"}, {"detail", "https://example.test/open"}},
        {{"kind", "web_find"}, {"label", "Find: needle"}, {"detail", "https://example.test/find"}},
        {{"kind", "url_citation"}, {"label", "Provider document"}, {"detail", "https://example.test/doc"}}
    });
    std::string body = sse({{"type", "response.output_text.delta"}, {"delta", "answer"}});
    body += sse({{"type", "response.output_item.added"}, {"item", {
        {"type", "web_search_call"}, {"action", nullptr}
    }}});
    for (const auto& item : {search, open, find, message}) {
        body += sse({{"type", "response.output_item.added"}, {"item", item}});
        body += sse({{"type", "response.output_item.done"}, {"item", item}});
    }
    body += sse({{"type", "response.output_text.annotation.added"}, {"annotation", citation}});
    const json completed = {{"type", "response.completed"}, {"response", {
        {"status", "completed"}, {"output", json::array({search, open, find, message})}
    }}};
    body += sse(completed);
    const auto streamed = CompleteSourceFixture("responses", true, body);
    Check(streamed.reply.ok, "Responses source SSE parses successfully");
    Check(streamed.reply.value.text == "answer" && streamed.reply.value.tool_calls.empty(),
          "source collection preserves streamed text and does not create tool calls");
    Check(json(streamed.reply.value.sources) == expected,
          "Responses action and citation sources deduplicate across added, done and completed output");
    const auto fallback = CompleteSourceFixture("responses", true, sse(completed));
    Check(fallback.reply.ok && json(fallback.reply.value.sources) == expected &&
              fallback.reply.value.text == "answer",
          "Responses completed output alone supplies real sources and text");
    const auto nonstream = CompleteSourceFixture("responses", false, completed["response"].dump());
    Check(nonstream.reply.ok && json(nonstream.reply.value.sources) == expected,
          "nonstreaming Responses output supplies the same sources");

    json partial_citation = citation;
    partial_citation["title"] = nullptr;
    const json alias_find = {{"type", "web_search_call"}, {"action", {
        {"type", "find_in_page"}, {"url", "https://example.test/find"}, {"pattern", "needle"}
    }}};
    json partial_find = alias_find;
    partial_find["action"].erase("pattern");
    const json malformed = {{"type", "message"}, {"content", json::array({
        {{"type", "output_text"}, {"annotations", json::array({
            nullptr, {{"type", "url_citation"}, {"url", 123}},
            {{"type", "file_citation"}, {"file_id", "ignored"}}
        })}}
    })}};
    std::string optional_body = sse({{"type", "response.output_text.annotation.added"},
                                     {"annotation", partial_citation}});
    for (const auto& item : {malformed, partial_find, alias_find, alias_find}) {
        optional_body += sse({{"type", "response.output_item.done"}, {"item", item}});
    }
    optional_body += sse({{"type", "response.completed"}, {"response", {
        {"status", "completed"}, {"output", json::array({message})}
    }}});
    const auto optional = CompleteSourceFixture("responses", true, optional_body);
    Check(optional.reply.ok && json(optional.reply.value.sources) == json::array({expected[5], expected[4]}),
          "optional malformed metadata is ignored, find_in_page works and a late citation title enriches its URL");

    std::string unicode;
    for (int index = 0; index < 900; ++index) unicode += "\xe4\xb8\xad";
    json many = json::array();
    for (int index = 0; index < 80; ++index) {
        many.push_back({{"type", "url_citation"}, {"title", unicode},
                        {"url", "https://example.test/" + std::to_string(index) + "/" + unicode}});
    }
    const json bounded_message = {{"type", "message"}, {"content", json::array({
        {{"type", "output_text"}, {"text", "bounded"}, {"annotations", many}}
    })}};
    const json bounded_search = {{"type", "web_search_call"}, {"action", {
        {"type", "search"}, {"query", unicode}, {"queries", json::array({unicode, nullptr, 123})}
    }}};
    const json bounded_open = {{"type", "web_search_call"}, {"action", {
        {"type", "open_page"}, {"url", "https://example.test/" + unicode}
    }}};
    const json bounded_find = {{"type", "web_search_call"}, {"action", {
        {"type", "find"}, {"url", "https://example.test/" + unicode}, {"pattern", unicode}
    }}};
    const auto bounded = CompleteSourceFixture("responses", true, sse({{"type", "response.completed"}, {"response", {
        {"status", "completed"}, {"output", json::array({bounded_search, bounded_open, bounded_find, bounded_message})}
    }}}));
    Check(bounded.reply.ok && bounded.reply.value.sources.size() == 64,
          "provider source references have a hard per-reply limit");
    for (const auto& source : bounded.reply.value.sources) {
        Check(source.kind.size() <= 2048 && source.label.size() <= 2048 && source.detail.size() <= 2048 &&
                  spark::IsValidUtf8(source.kind) && spark::IsValidUtf8(source.label) &&
                  spark::IsValidUtf8(source.detail),
              "source strings stay within their byte limit without splitting UTF-8");
    }

    const json chat_citation = {{"type", "url_citation"}, {"url_citation", {
        {"url", citation["url"]}, {"title", citation["title"]}, {"start_index", 0}, {"end_index", 6}
    }}};
    const json chat_message = {{"content", "chat answer"}, {"annotations", json::array({chat_citation})}};
    const std::string chat_body = sse({{"choices", json::array({{{"delta", chat_message}}})}}) +
        sse({{"choices", json::array({{{"delta", {{"annotations", json::array({chat_citation})}}},
                                       {"finish_reason", "stop"}}})}}) + "data: [DONE]\n\n";
    const auto chat_streamed = CompleteSourceFixture("chat_completions", true, chat_body);
    Check(chat_streamed.reply.ok && json(chat_streamed.reply.value.sources) == json::array({expected[5]}),
          "chat completion delta citations use the same bounded deduplication");
    const std::string chat_json = json({{"choices", json::array({
        {{"message", chat_message}, {"finish_reason", "stop"}}
    })}}).dump();
    const auto chat = CompleteSourceFixture("chat_completions", false, chat_json);
    Check(chat.reply.ok && json(chat.reply.value.sources) == json::array({expected[5]}),
          "chat completion message citations are collected");
    const auto responses_with_history = CompleteSourceFixture("responses", true, body, streamed.reply.value.sources);
    const auto chat_with_history = CompleteSourceFixture("chat_completions", false, chat_json, chat.reply.value.sources);
    Check(responses_with_history.reply.ok && chat_with_history.reply.ok &&
              streamed.request.is_object() && chat.request.is_object() &&
              responses_with_history.request == streamed.request && chat_with_history.request == chat.request,
          "source metadata does not change outbound messages or cache prefixes");
    const auto uncited = CompleteSourceFixture("responses", false, json({
        {"status", "completed"}, {"output", json::array({
            {{"type", "message"}, {"content", json::array({
                {{"type", "output_text"}, {"text", "https://example.test/uncited"}}
            })}}
        })}
    }).dump());
    Check(uncited.reply.ok && uncited.reply.value.sources.empty(),
          "URLs in ordinary answer text are never invented as source references");

    for (const int iterations : {1, 0}) {
        const std::string directory = spark::JoinPath(spark::CurrentDirectoryUtf8(),
                                                     "spark_sources_test_" + spark::NewSessionId());
        SourceFixtureServer server(body, "text/event-stream");
        Check(server.port != 0, "source persistence fixture server starts");
        if (server.port == 0) continue;
        spark::AppConfig config;
        config.workspace.path = directory;
        config.storage.data_dir = spark::JoinPath(directory, "data");
        config.skills.enabled = false;
        config.agent.max_iterations = iterations;
        config.agent.request_timeout_seconds = 5;
        config.agent.response_header_timeout_seconds = 5;
        config.active = {"source-fixture", "source-fixture-model", ""};
        spark::ProviderConfig provider;
        provider.id = "source-fixture";
        provider.protocol = "responses";
        provider.base_url = "http://127.0.0.1:" + std::to_string(server.port) + "/v1";
        provider.api_key = "source-fixture-token";
        spark::ModelConfig model;
        model.id = "source-fixture-model";
        model.capabilities.streaming = true;
        model.capabilities.tool_calls = false;
        provider.models.push_back(model);
        config.providers.push_back(provider);
        spark::SessionJournal journal;
        Check(journal.Open(config.storage.data_dir, directory).ok, "source persistence journal opens");
        spark::Agent agent(config, &journal, nullptr);
        spark::CancellationToken cancellation;
        const auto turn = agent.RunTurn("collect sources", {}, {}, &cancellation);
        Check(turn.ok && !journal.State().messages.empty() &&
                  json(journal.State().messages.back().sources) == expected,
              "normal and iteration-limit assistant replies receive provider sources");
        Check(journal.SaveSnapshot().ok, "source persistence snapshot saves");
        spark::SessionJournal restored;
        Check(restored.Resume(journal.Path()).ok && !restored.State().messages.empty() &&
                  json(restored.State().messages.back().sources) == expected,
              "assistant provider sources survive journal resume");
        std::error_code error;
        spark::fs::remove_all(spark::Utf8ToWide(directory), error);
    }
}

}  // namespace

int main() {
    // provider_order is an optional hint: ignore invalid/repeated IDs, retain all providers.
    const auto provider_order_fixture = spark::ParseConfigJson(R"({
        "providers": {"alpha": {"label": "A"}, "beta": {"label": "B"},
                      "gamma": {"label": "C"}, "invalid": false},
        "provider_order": ["gamma", "unknown", "gamma", null, 42, {}, false, "invalid", "alpha"]
    })");
    const auto provider_ids = [](const spark::AppConfig& config) {
        std::vector<std::string> ids;
        for (const auto& provider : config.providers) ids.push_back(provider.id);
        return ids;
    };
    auto ordered_providers = spark::ConfigFromJson(provider_order_fixture, {});
    Check(provider_ids(ordered_providers) == std::vector<std::string>{"gamma", "alpha", "beta"},
          "provider_order ignores duplicate/unknown/non-string IDs and appends omitted providers");
    Check(ordered_providers.providers.size() == 3 && ordered_providers.providers[0].label == "C",
          "provider_order retains provider contents and skips malformed provider entries");
    for (const auto& order : std::vector<nlohmann::json>{
             nullptr, false, 12, "gamma", nlohmann::json::object(), nlohmann::json::array()}) {
        auto fixture = provider_order_fixture;
        fixture["provider_order"] = order;
        Check(provider_ids(spark::ConfigFromJson(fixture, {})) ==
                  std::vector<std::string>{"alpha", "beta", "gamma"},
              "malformed or empty provider_order preserves legacy provider iteration order");
    }
    auto legacy_provider_order = provider_order_fixture;
    legacy_provider_order.erase("provider_order");
    Check(provider_ids(spark::ConfigFromJson(legacy_provider_order, {})) ==
              std::vector<std::string>{"alpha", "beta", "gamma"},
          "missing provider_order remains backwards compatible");
    std::reverse(ordered_providers.providers.begin(), ordered_providers.providers.end());
    const auto saved_provider_order = spark::ConfigToJson(ordered_providers);
    Check(saved_provider_order["provider_order"] == nlohmann::json::array({"beta", "alpha", "gamma"}),
          "provider_order is regenerated from the current vector instead of stale raw JSON");
    const auto reloaded_provider_order = spark::ConfigFromJson(
        nlohmann::json::parse(saved_provider_order.dump()), {});
    Check(provider_ids(reloaded_provider_order) == provider_ids(ordered_providers) &&
              spark::ConfigToJson(reloaded_provider_order) == saved_provider_order,
          "provider_order survives serialized configuration round trips");
    ordered_providers.providers.clear();
    const auto empty_provider_order = spark::ConfigToJson(ordered_providers);
    Check(empty_provider_order["provider_order"] == nlohmann::json::array() &&
              spark::ConfigFromJson(empty_provider_order, {}).providers.empty(),
          "provider_order drops stale IDs when all providers are removed");

    TestProviderSources();
    Check(spark::SanitizeDisplayText("before\x1b[31mafter\x07") == "beforeafter",
          "display sanitizer removes ANSI CSI and bell characters");
    Check(spark::SanitizeDisplayText("before\x1b[31") == "before",
          "display sanitizer removes an incomplete ANSI CSI sequence");
    Check(spark::SanitizeDisplayText("\x1b]0;title\x07ok") == "ok",
          "display sanitizer removes an ANSI OSC title sequence");
    Check(spark::SanitizeDisplayText("a\r\nb\tc") == "a\nb\tc",
          "display sanitizer preserves newlines and tabs while removing CR");
    Check(spark::SanitizeDisplayText("e\xc3\xa9\xc2\x85\xe4\xb8\xad") == "e\xc3\xa9\xe4\xb8\xad",
          "display sanitizer removes UTF-8 C1 control characters");

    std::set<std::string> session_ids;
    for (int index = 0; index < 10000; ++index) session_ids.insert(spark::NewSessionId());
    Check(session_ids.size() == 10000, "rapidly created session ids are unique");

    const auto parsed = spark::ConfigFromJson(spark::ParseConfigJson(spark::DefaultConfigExampleJson()), "test-config.json");
    const std::string windows_text = spark::WindowsTextFileContents("a\nb\r\nc");
    Check(windows_text == std::string("\xEF\xBB\xBF") + "a\r\nb\r\nc",
          "Windows text files get a UTF-8 BOM and CRLF endings");
    spark::AppConfig assigned;
    assigned = spark::ConfigFromJson(spark::ParseConfigJson(spark::DefaultConfigExampleJson()), "test-config.json");
    Check(assigned.schema_version == 2, "configuration move assignment");
    Check(parsed.schema_version == 2, "example schema version");
    Check(parsed.providers.size() == 1, "example provider count");
    Check(spark::FindProvider(parsed, "provider-a") != nullptr, "example active provider");
    Check(parsed.providers[0].protocol == "openai_responses", "example uses Responses API with comments");
    Check(spark::FindConfiguredApiKey(parsed).empty(), "example key starts empty");
    Check(parsed.providers[0].models[0].capabilities.streaming, "model streaming defaults to true");
    Check(parsed.providers[0].models[0].capabilities.tool_calls, "model tool calls default to true");
    Check(parsed.providers[0].models[0].capabilities.images, "model image support defaults to true");
    Check(parsed.providers[0].models[0].capabilities.reasoning, "model reasoning defaults to true");
    nlohmann::json explicit_capabilities = spark::ParseConfigJson(spark::DefaultConfigExampleJson());
    explicit_capabilities["providers"]["provider-a"]["models"][0]["capabilities"]["images"] = false;
    explicit_capabilities["providers"]["provider-a"]["models"][0]["capabilities"]["reasoning"] = false;
    const auto explicit_disabled = spark::ConfigFromJson(explicit_capabilities, "explicit-capabilities.json");
    Check(!explicit_disabled.providers[0].models[0].capabilities.images &&
              !explicit_disabled.providers[0].models[0].capabilities.reasoning,
          "explicit false capability values override defaults");
    Check(parsed.providers[0].models[0].capabilities.streaming, "model streaming defaults to true");
    Check(parsed.providers[0].models[0].capabilities.tool_calls, "model tool calls default to true");
    Check(parsed.providers[0].models[0].capabilities.images, "model image support defaults to true");
    Check(parsed.providers[0].models[0].capabilities.reasoning, "model reasoning defaults to true");
    const auto validation = spark::ValidateConfig(parsed, true);
    Check(std::find(validation.begin(), validation.end(), "providers.provider-a.api_key: empty") != validation.end(),
          "empty key is reported");
    nlohmann::json legacy_context = spark::ParseConfigJson(spark::DefaultConfigExampleJson());
    legacy_context["context"]["auto_compact"] = false;
    const auto legacy_parsed = spark::ConfigFromJson(legacy_context, "legacy-config.json");
    Check(legacy_parsed.context.auto_compact, "legacy auto_compact=false is upgraded to always enabled");
    Check(spark::ConfigToPrettyJson(parsed).find("\"auto_compact\"") == std::string::npos,
          "configuration output no longer exposes an auto-compaction switch");

    const std::string dsml_text =
        "Before\n<||DSML|| calls>\n"
        "<||DSML|| invoke name=\"write_file\">\n"
        "<||DSML|| parameter name=\"path\" string=\"true\">work\\isolate-parse.ps1</||DSML|| parameter>\n"
        "<||DSML|| parameter name=\"content\" string=\"true\">$work = 'C:\\Temp'\nWrite-Output $work</||DSML|| parameter>\n"
        "</||DSML|| invoke>\n"
        "</||DSML|| calls>\nAfter";
    const spark::DsmlParseResult dsml = spark::ExtractDsmlToolCalls(dsml_text);
    Check(dsml.found && !dsml.incomplete && !dsml.malformed, "DSML tool call is detected");
    Check(dsml.tool_calls.size() == 1 && dsml.tool_calls[0].name == "write_file",
          "DSML invoke name is parsed");
    if (dsml.tool_calls.size() == 1) {
        const nlohmann::json arguments = nlohmann::json::parse(dsml.tool_calls[0].arguments_json);
        Check(arguments.value("path", std::string{}) == "work\\isolate-parse.ps1",
              "DSML string parameter keeps its path");
        Check(arguments.value("content", std::string{}).find("Write-Output $work") != std::string::npos,
              "DSML string parameter keeps multiline content");
    }
    Check(dsml.text.find("Before") != std::string::npos && dsml.text.find("After") != std::string::npos &&
              dsml.text.find("DSML") == std::string::npos,
          "DSML markup is removed from visible text");

    spark::DsmlStreamFilter stream_filter;
    const std::string first_stream = stream_filter.Push("Before\n<||DSML|| ca");
    const std::string second_stream = stream_filter.Push(
        "lls><||DSML|| invoke name=\"run_command\"><||DSML|| parameter name=\"command\" "
        "string=\"true\">echo ok</||DSML|| parameter></||DSML|| invoke></||DSML|| calls>After");
    Check(first_stream == "Before\n", "stream filter delays a partial DSML marker");
    Check(second_stream == "After", "stream filter hides a complete DSML block");
    Check(stream_filter.Finish().empty() && !stream_filter.incomplete(), "complete streamed DSML block closes cleanly");

    spark::DsmlStreamFilter incomplete_filter;
    Check(incomplete_filter.Push("<||DSML|| calls><||DSML|| invoke name=\"write_file\">").empty(),
          "incomplete DSML block stays hidden");
    Check(incomplete_filter.Finish().empty() && incomplete_filter.incomplete(),
          "incomplete streamed DSML block reports an error");
    const spark::DsmlParseResult incomplete_dsml =
        spark::ExtractDsmlToolCalls("<||DSML|| calls><||DSML|| invoke name=\"write_file\">");
    Check(incomplete_dsml.incomplete, "incomplete DSML block is not treated as a final answer");

    const std::string recovery_directory =
        spark::JoinPath(spark::CurrentDirectoryUtf8(), "spark_session_recovery_test");
    std::error_code recovery_error;
    spark::fs::remove_all(spark::Utf8ToWide(recovery_directory), recovery_error);
    spark::SessionJournal stuck_journal;
    Check(stuck_journal.Open(recovery_directory, recovery_directory).ok, "stuck session opens");
    spark::ChatMessage interrupted_marker;
    interrupted_marker.role = spark::MessageRole::System;
    interrupted_marker.content = spark::kInterruptedTurnMarker;
    stuck_journal.MutableState().messages.push_back(interrupted_marker);
    Check(stuck_journal.Append("message", interrupted_marker).ok, "interrupted marker persists");
    stuck_journal.MutableState().metadata["paused_turn"] = {
        {"elapsed_ms", 4321},
        {"interrupted_at", 99},
    };
    Check(stuck_journal.SaveSnapshot().ok, "paused snapshot persists");
    stuck_journal.MutableState().metadata.erase("paused_turn");
    Check(stuck_journal.SaveSnapshot().ok, "failed resume snapshot persists");
    spark::SessionJournal recovered_journal;
    const auto recovered = recovered_journal.Resume(stuck_journal.Path());
    Check(recovered.ok && recovered_journal.State().metadata.contains("paused_turn"),
          "loading recovers a paused turn whose final snapshot lost the marker");
    Check(stuck_journal
              .Append("context.compacted",
                      {{"trigger", "pre_turn"},
                       {"message_count", 2},
                       {"before_tokens", 258000},
                       {"after_tokens", 42000}})
              .ok,
          "compaction event persists");
    Check(stuck_journal.SaveSnapshot().ok, "compaction snapshot persists");
    spark::SessionJournal compaction_journal;
    const auto compaction_reloaded = compaction_journal.Resume(stuck_journal.Path());
    Check(compaction_reloaded.ok && compaction_journal.State().metadata.contains("context_compactions") &&
              compaction_journal.State().metadata["context_compactions"].size() == 1,
          "compaction marker survives session reload");
    spark::fs::remove_all(spark::Utf8ToWide(recovery_directory), recovery_error);

    spark::AppConfig key_selection = parsed;
    key_selection.providers[0].api_key = "active-secret";
    key_selection.providers[0].api_key_expression = "active-secret";
    const auto active_key_validation = spark::ValidateConfig(key_selection, true);
    Check(std::find(active_key_validation.begin(), active_key_validation.end(), "providers.provider-a.api_key: empty") ==
              active_key_validation.end(),
          "single provider key validates");

    const std::string config_directory = spark::JoinPath(spark::CurrentDirectoryUtf8(), "spark_config_test");
    std::error_code config_error;
    spark::fs::create_directories(spark::Utf8ToWide(config_directory), config_error);
    spark::AppConfig save_candidate = parsed;
    save_candidate.config_path = spark::JoinPath(config_directory, "config.json");
    save_candidate.providers[0].api_key = "test-secret";
    save_candidate.providers[0].api_key_expression = "test-secret";
    Check(spark::SaveConfigAtomically(save_candidate).ok, "configuration saves atomically");
    const auto loaded = spark::LoadConfig(save_candidate.config_path, false);
    Check(loaded.ok && spark::FindConfiguredApiKey(loaded.value) == "test-secret", "configuration reload preserves selected key");
    save_candidate.workspace.path = std::string("invalid-") + static_cast<char>(0xFF);
    bool invalid_config_serializable = true;
    try {
        static_cast<void>(spark::ConfigToPrettyJson(save_candidate));
    } catch (const nlohmann::json::exception&) {
        invalid_config_serializable = false;
    }
    Check(invalid_config_serializable, "invalid UTF-8 configuration still serializes safely");
    spark::fs::remove_all(spark::Utf8ToWide(config_directory), config_error);

    spark::ChatMessage message;
    message.role = spark::MessageRole::Assistant;
    message.content = "hello";
    message.timestamp = 1700000001;
    spark::ToolCall call{"call-1", "read_file", "{\"path\":\"README.md\"}"};
    message.tool_calls.push_back(call);
    message.sources.push_back({"image", "已查看图像", "C:\\work\\screen.png"});
    const nlohmann::json encoded = message;
    const spark::ChatMessage decoded = encoded.get<spark::ChatMessage>();
    Check(decoded.role == spark::MessageRole::Assistant, "message role round trip");
    Check(decoded.tool_calls.size() == 1 && decoded.tool_calls[0].id == "call-1", "tool call round trip");
    Check(decoded.sources.size() == 1 && decoded.sources[0].kind == "image" &&
              decoded.sources[0].detail == "C:\\work\\screen.png",
          "message source round trip");

    Check(spark::EstimateTextTokens("hello world") > 0, "ascii token estimate");
    Check(spark::EstimateTextTokens("你好世界") >= 4, "cjk token estimate is conservative");
    spark::ContextConfig context;
    spark::ModelConfig model;
    model.capabilities.context_window = 100000;
    context.fallback_context_window = 258000;
    Check(spark::EffectiveContextLimit(context, model) == 100000,
          "effective context limit does not exceed the model context window");
    model.capabilities.context_window = 258000;
    Check(spark::EffectiveContextLimit(context, model) == 258000,
          "explicit compact threshold is preserved when it fits the model context window");
    context.auto_compact = false;
    Check(spark::ShouldCompactContext(context, model, 250000),
          "automatic compaction is always enabled for legacy configurations");
    std::vector<spark::ChatMessage> summary_messages;
    spark::ChatMessage goal;
    goal.role = spark::MessageRole::User;
    goal.content = "修复扫描崩溃";
    summary_messages.push_back(goal);
    spark::ChatMessage answer;
    answer.role = spark::MessageRole::Assistant;
    answer.content = "已限制扫描深度并跳过重解析点。";
    summary_messages.push_back(answer);
    const std::string handoff = spark::BuildHandoffSummary(summary_messages, "test", "C:\\work", 8000);
    Check(handoff.find("[目标]") != std::string::npos, "handoff summary has goal");
    Check(handoff.find("[禁止重复]") != std::string::npos, "handoff summary has replay guard");
    const auto compacted_history = spark::BuildCompactedHistory(summary_messages, handoff);
    Check(!compacted_history.empty() && compacted_history.front().role == spark::MessageRole::System,
          "compacted history starts with handoff");

    // Regression: compacting an already-compacted context used to slice the retained tail in the
    // middle of a tool group, so the provider received a function_call_output whose function_call
    // had been dropped ("No tool call found for tool output with call_id ...").
    std::vector<spark::ChatMessage> twice_compacted_input;
    spark::ChatMessage previous_handoff;
    previous_handoff.role = spark::MessageRole::System;
    previous_handoff.content = "Conversation history handoff summary:\n[目标] 二次压缩";
    twice_compacted_input.push_back(previous_handoff);
    spark::ChatMessage first_call;
    first_call.role = spark::MessageRole::Assistant;
    first_call.content = "读取第一个文件";
    first_call.tool_calls.push_back(spark::ToolCall{"call-a", "read_file", R"({"path":"a.txt"})"});
    first_call.tool_calls.push_back(spark::ToolCall{"call-b", "read_file", R"({"path":"b.txt"})"});
    twice_compacted_input.push_back(first_call);
    spark::ChatMessage first_output;
    first_output.role = spark::MessageRole::Tool;
    first_output.name = "read_file";
    first_output.tool_call_id = "call-a";
    first_output.content = "a";
    twice_compacted_input.push_back(first_output);
    spark::ChatMessage second_output_of_first_call;
    second_output_of_first_call.role = spark::MessageRole::Tool;
    second_output_of_first_call.name = "read_file";
    second_output_of_first_call.tool_call_id = "call-b";
    second_output_of_first_call.content = "b";
    twice_compacted_input.push_back(second_output_of_first_call);
    spark::ChatMessage second_call;
    second_call.role = spark::MessageRole::Assistant;
    second_call.tool_calls.push_back(spark::ToolCall{"call-c", "read_file", R"({"path":"c.txt"})"});
    twice_compacted_input.push_back(second_call);
    spark::ChatMessage second_output;
    second_output.role = spark::MessageRole::Tool;
    second_output.name = "read_file";
    second_output.tool_call_id = "call-c";
    second_output.content = "c";
    twice_compacted_input.push_back(second_output);
    spark::ChatMessage orphan_result;
    orphan_result.role = spark::MessageRole::Tool;
    orphan_result.name = "read_file";
    orphan_result.tool_call_id = "call-orphan";
    orphan_result.content = "stale";
    twice_compacted_input.push_back(orphan_result);
    const auto twice_compacted = spark::BuildCompactedHistory(twice_compacted_input, "[目标] 二次压缩");
    std::set<std::string> retained_calls;
    std::set<std::string> retained_outputs;
    for (const spark::ChatMessage& message : twice_compacted) {
        if (message.role == spark::MessageRole::Assistant) {
            for (const spark::ToolCall& call : message.tool_calls) retained_calls.insert(call.id);
        } else if (message.role == spark::MessageRole::Tool) {
            retained_outputs.insert(message.tool_call_id);
        }
    }
    bool orphan_output = false;
    for (const spark::ChatMessage& message : twice_compacted) {
        if (message.role == spark::MessageRole::Tool && retained_calls.count(message.tool_call_id) == 0) {
            orphan_output = true;
        }
    }
    Check(!orphan_output, "compaction never keeps a tool output whose call was dropped");
    bool unpaired_call = false;
    for (const spark::ChatMessage& message : twice_compacted) {
        if (message.role != spark::MessageRole::Assistant) continue;
        for (const spark::ToolCall& call : message.tool_calls) {
            if (retained_outputs.count(call.id) == 0) unpaired_call = true;
        }
    }
    Check(!unpaired_call, "compaction always keeps an output for every retained tool call");

    const std::vector<std::string> redacted = {spark::RedactSecrets("key=secret-123", {"secret-123"})};
    Check(redacted[0] == "key=[REDACTED]", "secret redaction");
    Check(spark::IsPathWithin("C:\\work\\src\\main.cpp", "C:\\work"), "path containment");
    Check(!spark::IsPathWithin("C:\\work-other\\main.cpp", "C:\\work"), "path containment boundary");
    Check(spark::IsQuotaExhaustedError("Insufficient Balance (request_id: test) | HTTP 402"),
          "insufficient balance is classified as quota exhaustion");
    Check(spark::IsQuotaExhaustedError("错误：额度不足，请充值"), "Chinese quota error is classified");
    Check(!spark::IsQuotaExhaustedError("HTTP 500 internal server error"),
          "unrelated server errors are not classified as quota exhaustion");

    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    Check(parent != nullptr, "window text test parent");
    if (parent != nullptr) {
        HWND edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE, 0, 0, 100, 100, parent,
                                    nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(edit != nullptr, "window text test edit");
        if (edit != nullptr) {
            SetWindowTextW(edit, L"hello\nworld");
            Check(spark::ReadWindowText(edit) == L"hello\nworld", "window text reads complete content");
            DestroyWindow(edit);
        }
        DestroyWindow(parent);
    }

    Check(spark::CreateProviderAdapter("chat_completions") != nullptr, "chat adapter exists");
    Check(spark::CreateProviderAdapter("responses") != nullptr, "responses adapter exists");
    Check(spark::CreateProviderAdapter(" Responses ") != nullptr, "protocol matching is case insensitive");
    Check(spark::CreateProviderAdapter("anthropic_messages") != nullptr, "anthropic adapter exists");
    Check(spark::CreateProviderAdapter("unknown") == nullptr, "unknown adapter rejected");
    spark::ProviderConfig connection_provider;
    connection_provider.base_url = "https://api.deepseek.com/";
    Check(spark::ProviderConnectionCheckUrl(connection_provider) == "https://api.deepseek.com/models",
          "connection check uses models endpoint");
    connection_provider.base_url = "https://api.deepseek.com/v1";
    Check(spark::ProviderConnectionCheckUrl(connection_provider) == "https://api.deepseek.com/v1/models",
          "connection check preserves API version path");

    const std::string temp_directory = spark::JoinPath(spark::CurrentDirectoryUtf8(), "spark_test_data");
    std::error_code remove_error;
    spark::fs::remove_all(spark::Utf8ToWide(temp_directory), remove_error);

    const std::string exchange_directory =
        spark::JoinPath(spark::CurrentDirectoryUtf8(), "spark_session_exchange_test");
    spark::fs::remove_all(spark::Utf8ToWide(exchange_directory), remove_error);
    spark::SessionJournal exchange_source;
    const auto exchange_opened = exchange_source.Open(
        spark::JoinPath(exchange_directory, "data"),
        spark::JoinPath(exchange_directory, "workspace"));
    Check(exchange_opened.ok, "session exchange source opens");
    if (exchange_opened.ok) {
        spark::ChatMessage exchange_user;
        exchange_user.role = spark::MessageRole::User;
        exchange_user.content = "export marker";
        exchange_user.timestamp = 1700000000;
        Check(exchange_source.Append("message", exchange_user).ok, "session exchange user appends");

        spark::ChatMessage exchange_assistant;
        exchange_assistant.role = spark::MessageRole::Assistant;
        exchange_assistant.content = "assistant result";
        exchange_assistant.reasoning_text = "reasoning marker";
        exchange_assistant.timestamp = 1700000001;
        exchange_assistant.tool_calls.push_back({"call-1", "read_file", "{\"path\":\"a.txt\"}"});
        Check(exchange_source.Append("message", exchange_assistant).ok, "session exchange assistant appends");

        spark::ChatMessage exchange_tool;
        exchange_tool.role = spark::MessageRole::Tool;
        exchange_tool.tool_call_id = "call-1";
        exchange_tool.name = "read_file";
        exchange_tool.content = "tool output marker";
        exchange_tool.timestamp = 1700000002;
        Check(exchange_source.Append("message", exchange_tool).ok, "session exchange tool appends");

        const std::string rollout_path = spark::JoinPath(exchange_directory, "rollout.jsonl");
        const auto exported = spark::ExportSparkSessionToCodex(exchange_source.Path(), rollout_path, "provider-test");
        Check(exported.ok, "Spark session exports to Codex rollout");
        std::string rollout_error;
        const auto rollout = spark::ReadFileUtf8(rollout_path, &rollout_error);
        Check(rollout.has_value(), "exported Codex rollout is readable");
        if (rollout.has_value()) {
            Check(rollout->find("\"type\":\"session_meta\"") != std::string::npos,
                  "exported rollout starts with session metadata");
            Check(rollout->find("export marker") != std::string::npos &&
                      rollout->find("assistant result") != std::string::npos &&
                      rollout->find("tool output marker") != std::string::npos,
                  "exported rollout contains the full transcript");
            Check(rollout->find("\"type\":\"function_call\"") != std::string::npos,
                  "exported rollout preserves function calls");
        }

        const auto imported = spark::ImportCodexSessionToSpark(
            rollout_path, spark::JoinPath(exchange_directory, "imported-data"),
            spark::JoinPath(exchange_directory, "workspace"), "project");
        Check(imported.ok, "Codex rollout imports into Spark");
        if (imported.ok) {
            Check(imported.value.title == "export marker", "imported session keeps the first user title");
            Check(imported.value.message_count >= 5, "imported session keeps reasoning, tool and answer items");
            spark::SessionJournal imported_journal;
            Check(imported_journal.Resume(imported.value.path).ok, "imported Spark session resumes");
            Check(!imported_journal.State().messages.empty() &&
                      imported_journal.State().messages.front().content == "export marker",
                  "imported session keeps the first user message");
            std::string imported_error;
            const auto imported_text = spark::ReadFileUtf8(imported.value.path, &imported_error);
            Check(imported_text.has_value() && imported_text->find("\"scope\":\"project\"") != std::string::npos,
                  "imported project session keeps its scope");
        }
    }

    const std::string agents_directory =
        spark::JoinPath(spark::CurrentDirectoryUtf8(), "spark_agents_md_test");
    spark::fs::remove_all(spark::Utf8ToWide(agents_directory), remove_error);
    const std::string agents_global_home = spark::JoinPath(agents_directory, "codex-home");
    const std::string agents_user_profile = spark::JoinPath(agents_directory, "user-profile");
    const std::string agents_codex_fallback = spark::JoinPath(agents_user_profile, ".codex");
    const std::string agents_root = spark::JoinPath(agents_directory, "project");
    const std::string agents_workspace = spark::JoinPath(agents_root, "nested");
    spark::fs::create_directories(spark::Utf8ToWide(spark::JoinPath(agents_root, ".git")), remove_error);
    spark::fs::create_directories(spark::Utf8ToWide(agents_workspace), remove_error);
    spark::fs::create_directories(spark::Utf8ToWide(agents_global_home), remove_error);
    spark::fs::create_directories(spark::Utf8ToWide(agents_codex_fallback), remove_error);
    Check(spark::WriteFileUtf8Atomic(spark::JoinPath(agents_global_home, "AGENTS.md"), "isolated Codex home rule"),
          "Codex home AGENTS.md fixture writes");
    Check(spark::WriteFileUtf8Atomic(spark::JoinPath(agents_codex_fallback, "AGENTS.md"),
                                   "isolated Codex profile rule"),
          "Codex profile AGENTS.md fixture writes");
    Check(spark::WriteFileUtf8Atomic(spark::JoinPath(agents_root, "AGENTS.md"), "root project rule"),
          "root AGENTS.md writes");
    Check(spark::WriteFileUtf8Atomic(spark::JoinPath(agents_workspace, "AGENTS.md"), "nested project rule"),
          "nested AGENTS.md writes");

    const std::wstring old_codex_home = [&]() {
        wchar_t value[32768]{};
        const DWORD length = GetEnvironmentVariableW(L"CODEX_HOME", value, static_cast<DWORD>(std::size(value)));
        return length > 0 ? std::wstring(value, length) : std::wstring{};
    }();
    SetEnvironmentVariableW(L"CODEX_HOME", spark::Utf8ToWide(agents_global_home).c_str());
    const std::string agents_prompt = spark::BuildAgentInstructionsPrompt(agents_workspace);
    Check(agents_prompt.find("isolated Codex home rule") == std::string::npos,
          "AGENTS.md excludes Codex global instructions from CODEX_HOME");
    Check(agents_prompt.find("root project rule") != std::string::npos,
          "AGENTS.md loads the project root instructions");
    Check(agents_prompt.find("nested project rule") != std::string::npos,
          "AGENTS.md loads the nested workspace instructions");
    Check(agents_prompt.find("root project rule") < agents_prompt.find("nested project rule"),
          "AGENTS.md applies root rules before nested rules");
    const std::string old_user_profile = spark::EnvironmentVariable("USERPROFILE");
    SetEnvironmentVariableW(L"CODEX_HOME", nullptr);
    SetEnvironmentVariableW(L"USERPROFILE", spark::Utf8ToWide(agents_user_profile).c_str());
    const std::string fallback_agents_prompt = spark::BuildAgentInstructionsPrompt(agents_workspace);
    Check(fallback_agents_prompt.find("isolated Codex profile rule") == std::string::npos,
          "AGENTS.md excludes Codex global instructions from the user profile");
    Check(fallback_agents_prompt == agents_prompt,
          "AGENTS.md project instructions are independent of Codex global paths");
    SetEnvironmentVariableW(L"USERPROFILE",
                           old_user_profile.empty() ? nullptr : spark::Utf8ToWide(old_user_profile).c_str());
    if (old_codex_home.empty()) {
        SetEnvironmentVariableW(L"CODEX_HOME", nullptr);
    } else {
        SetEnvironmentVariableW(L"CODEX_HOME", old_codex_home.c_str());
    }

    const std::string scope_directory =
        spark::JoinPath(spark::CurrentDirectoryUtf8(), "spark_external_scope_test");
    spark::fs::remove_all(spark::Utf8ToWide(scope_directory), remove_error);
    spark::fs::create_directories(spark::Utf8ToWide(spark::JoinPath(scope_directory, "data")), remove_error);
    spark::AppConfig scope_config;
    scope_config.storage.data_dir = spark::JoinPath(scope_directory, "data");
    const std::string projects_json = {
        "{\"projects\":[{\"name\":\"ExampleProject\",\"workspace_path\":\"C:\\\\Projects\\\\ExampleProject\",\"folders\":[]}]}"};
    Check(spark::WriteFileUtf8Atomic(spark::JoinPath(scope_config.storage.data_dir, "projects.json"), projects_json),
          "project scope fixture writes");
    Check(spark::ResolveExternalAgentScope(scope_config, spark::JoinPath(scope_directory, "config.json"),
                                           "C:\\Projects\\ExampleProject") == "project",
          "external agent uses project scope for a registered project path");
    Check(spark::ResolveExternalAgentScope(scope_config, spark::JoinPath(scope_directory, "config.json"),
                                           "C:\\Projects\\Other") == "recent",
          "external agent uses recent scope for an unregistered path");
    Check(spark::ResolveExternalAgentTitle("审查任务", "Workspace: C:\\Projects\\ExampleProject\nTask: inspect async approvals.") ==
              "审查任务",
          "external agent title prefers the caller-provided name");
    Check(spark::ResolveExternalAgentTitle("", "Workspace: C:\\Projects\\ExampleProject\n\nTask: inspect async approvals.\nDetails") ==
              "inspect async approvals",
          "external agent title skips MCP workspace metadata and extracts a short task title");
    Check(spark::ResolveExternalAgentTitle("", "工作区：C:\\Projects\\ExampleProject\n任务：检查 MCP 审批状态是否正常。更多说明") ==
              "检查 MCP 审批状态是否正常",
          "external agent title handles Chinese metadata and punctuation without splitting UTF-8 characters");
    Check(spark::ExternalAgentInfoJson(spark::ExternalAgentInfo{}).find("\"output_complete\": false") != std::string::npos,
          "external agent output defaults to incomplete until the worker finishes");
    spark::ExternalAgentInfo external_info;
    external_info.awaiting_approval = true;
    external_info.approval_kind = "command";
    external_info.approval_title = "Run command?";
    external_info.approval_detail = "cmake --build .";
    const nlohmann::json external_json = nlohmann::json::parse(spark::ExternalAgentInfoJson(external_info));
    Check(external_json.value("awaiting_approval", false) &&
              external_json.value("approval_kind", std::string{}) == "command" &&
              external_json.value("approval_detail", std::string{}) == "cmake --build ." &&
              !external_json.value("output_complete", true),
          "external agent results expose pending approval and partial-output state");

    spark::fs::remove_all(spark::Utf8ToWide(exchange_directory), remove_error);
    spark::fs::remove_all(spark::Utf8ToWide(agents_directory), remove_error);
    spark::fs::remove_all(spark::Utf8ToWide(scope_directory), remove_error);

    const std::string scan_directory = spark::JoinPath(spark::CurrentDirectoryUtf8(), "spark_scan_test");
    spark::fs::remove_all(spark::Utf8ToWide(scan_directory), remove_error);

    const std::string skills_root = spark::JoinPath(spark::CurrentDirectoryUtf8(), "spark_skills_test");
    spark::fs::remove_all(spark::Utf8ToWide(skills_root), remove_error);
    spark::fs::create_directories(spark::Utf8ToWide(spark::JoinPath(skills_root, "sample")), remove_error);
    spark::fs::create_directories(spark::Utf8ToWide(spark::JoinPath(skills_root, "sample\\references")),
                                        remove_error);
    spark::WriteFileUtf8Atomic(spark::JoinPath(skills_root, "sample\\SKILL.md"),
                               "---\n"
                               "name: sample-skill\n"
                               "description: >\n"
                               "  Test skill loading.\n"
                               "version: \"1.0\"\n"
                               "allowed-tools:\n"
                               "  - read_file\n"
                               "  - search_text\n"
                               "---\n"
                               "# Sample\n"
                               "Read references/guide.md first.\n",
                               nullptr);
    spark::WriteFileUtf8Atomic(spark::JoinPath(skills_root, "sample\\references\\guide.md"),
                               "reference body\n", nullptr);
    spark::AppConfig skills_config;
    skills_config.config_path = spark::JoinPath(spark::CurrentDirectoryUtf8(), "config.json");
    skills_config.skills.roots = {skills_root};
    skills_config.skills.include_workspace_root = false;
    skills_config.skills.include_user_root = false;
    const auto skills = spark::DiscoverSkills(skills_config);
    Check(skills.size() == 1 && skills[0].name == "sample-skill", "skill discovery loads SKILL.md metadata");
    Check(skills.size() == 1 && skills[0].description == "Test skill loading.",
          "multi-line front matter descriptions are folded");
    Check(skills.size() == 1 && skills[0].version == "1.0", "skill version front matter is parsed");
    Check(skills.size() == 1 && skills[0].allowed_tools.size() == 2 && skills[0].allowed_tools[1] == "search_text",
          "skill allowed-tools lists are parsed");
    Check(skills.size() == 1 && spark::FindSkillByName(skills, "SAMPLE-SKILL") == &skills[0],
          "skills can be looked up case-insensitively");
    Check(skills.size() == 1 && spark::FindSkillByName(skills, "sample") == &skills[0],
          "skills can be looked up by directory slug");
    Check(spark::SkillsPromptSection(skills).find("sample-skill") != std::string::npos, "skill prompt section includes skill");
    Check(spark::SkillsPromptSection(skills).find("<skills_instructions>") != std::string::npos,
          "skill prompt section uses the skills instructions wrapper");
    if (skills.size() == 1) {
        Check(spark::SelectExplicitSkills(skills, "please run $sample-skill now").size() == 1,
              "an explicit $skill mention selects the skill");
        Check(spark::SelectExplicitSkills(skills, "please run $sample").size() == 1,
              "a $slug mention selects the skill");
        Check(spark::SelectExplicitSkills(skills, "unrelated request").empty(),
              "unmentioned skills are not selected");
        Check(spark::SkillInvocationContext(skills).find("Read references/guide.md first.") != std::string::npos,
              "the skill invocation context carries the SKILL.md body");
        const auto body = spark::ReadSkillResource(skills[0], "");
        Check(body.ok && body.value.find("name: sample-skill") != std::string::npos,
              "read_skill defaults to SKILL.md");
        const auto reference = spark::ReadSkillResource(skills[0], "references/guide.md");
        Check(reference.ok && reference.value.find("reference body") != std::string::npos,
              "read_skill can read a relative resource");
        const auto escape = spark::ReadSkillResource(skills[0], "../../outside.md");
        Check(!escape.ok, "read_skill refuses to escape the skill directory");
    }
    spark::fs::remove_all(spark::Utf8ToWide(skills_root), remove_error);
    spark::fs::create_directories(spark::Utf8ToWide(spark::JoinPath(scan_directory, "nested")), remove_error);
    spark::WriteFileUtf8Atomic(spark::JoinPath(scan_directory, "nested\\sample.txt"), "needle\n", nullptr);
    spark::ToolContext tool_context;
    tool_context.workspace.path = scan_directory;
    tool_context.limits.max_tool_output_bytes = 65536;
    spark::ToolRegistry registry;
    const auto listed = registry.Execute("list_files", "{\"path\":\".\",\"max_entries\":16}", tool_context);
    Check(listed.ok && listed.output.find("nested\\sample.txt") != std::string::npos, "bounded list files finds workspace file");
    Check(listed.output.find("[scan]") != std::string::npos, "list files reports scan metadata");
    const auto searched = registry.Execute("search_text", "{\"query\":\"needle\"}", tool_context);
    Check(searched.ok && searched.output.find("nested\\sample.txt:1") != std::string::npos, "bounded search finds text");
    const auto limited = registry.Execute("search_text", "{\"query\":\"needle\",\"max_matches\":1}", tool_context);
    Check(limited.ok && limited.output.find("truncated=true") != std::string::npos, "search reports limits");
    bool run_command_has_timeout_override = false;
    for (const auto& definition : registry.Definitions()) {
        if (definition.name == "run_command" && definition.parameters.contains("properties") &&
            definition.parameters["properties"].contains("timeout_seconds")) {
            run_command_has_timeout_override = true;
            break;
        }
    }
    Check(run_command_has_timeout_override, "run_command exposes a per-call timeout override");
    tool_context.limits.max_command_output_bytes = 4096;
    tool_context.limits.max_tool_output_bytes = 8192;
    tool_context.limits.tool_timeout_seconds = 1;
    const auto extended_timeout_command = registry.Execute(
        "run_command", R"({"command":"ping -n 3 127.0.0.1 >nul","cwd":".","timeout_seconds":4})", tool_context);
    Check(extended_timeout_command.ok && !extended_timeout_command.output.empty(),
          "run_command timeout override allows a command beyond the default limit");
    tool_context.limits.tool_timeout_seconds = 15;
    const auto high_output_command = registry.Execute(
        "run_command",
        R"({"command":"for /L %i in (1,1,20000) do @echo 012345678901234567890123456789012345678901234567890123456789012345678901234567890","cwd":"."})",
        tool_context);
    Check(high_output_command.ok, "high-output command succeeds");
    Check(high_output_command.output.find("bytes omitted between head and tail") != std::string::npos,
          "high-output command keeps bounded head and tail output");
    const auto head_tail_command = registry.Execute(
        "run_command",
        R"({"command":"echo BEGIN & for /L %i in (1,1,20000) do @echo filler filler filler filler filler filler filler filler & echo END","cwd":"."})",
        tool_context);
    Check(head_tail_command.ok && head_tail_command.output.find("BEGIN") != std::string::npos &&
              head_tail_command.output.find("END") != std::string::npos,
          "bounded command output preserves head and tail markers");
    const auto oem_command = registry.Execute(
        "run_command",
        R"({"command":"cmd /c chcp 936 >nul && echo 测试","cwd":"."})",
        tool_context);
    Check(oem_command.ok && oem_command.output.find("测试") != std::string::npos,
          "OEM command output is converted to UTF-8");
    const auto dir_drive_command = registry.Execute(
        "run_command",
        R"({"command":"cmd /c dir C:\\","cwd":"."})",
        tool_context);
    Check(dir_drive_command.ok, "directory listing of C drive succeeds");
    bool oem_serializable = true;
    try {
        static_cast<void>(nlohmann::json{{"output", oem_command.output}}.dump());
        static_cast<void>(nlohmann::json{{"output", dir_drive_command.output}}.dump());
    } catch (const nlohmann::json::exception&) {
        oem_serializable = false;
    }
    Check(oem_serializable, "OEM and directory-list output are valid JSON UTF-8");
    spark::ChatMessage image_message;
    image_message.role = spark::MessageRole::User;
    image_message.content = "inspect this image";
    image_message.image_paths = {"C:\\screenshots\\sample.png"};
    const nlohmann::json image_json = image_message;
    const spark::ChatMessage restored_image_message = image_json.get<spark::ChatMessage>();
    Check(restored_image_message.image_paths == image_message.image_paths &&
              restored_image_message.content == image_message.content,
          "chat message image references survive session serialization");
    spark::fs::remove_all(spark::Utf8ToWide(scan_directory), remove_error);
    const std::string conversation_base = spark::JoinPath(spark::CurrentDirectoryUtf8(), "spark_conversation_test");
    spark::fs::remove_all(spark::Utf8ToWide(conversation_base), remove_error);
    std::string conversation_error;
    const std::string conversation_workspace =
        spark::CreateNoProjectConversationWorkspace(conversation_base, &conversation_error);
    Check(!conversation_workspace.empty(), "no-project conversation workspace is created");
    Check(spark::fs::is_directory(spark::Utf8ToWide(spark::JoinPath(conversation_workspace, "work"))),
          "no-project conversation creates work directory");
    Check(spark::fs::is_directory(spark::Utf8ToWide(spark::JoinPath(conversation_workspace, "outputs"))),
          "no-project conversation creates outputs directory");
    Check(spark::NormalizePathForComparison(conversation_workspace)
              .rfind(spark::NormalizePathForComparison(conversation_base), 0) == 0,
          "no-project conversation is nested below the configured root");
    spark::fs::remove_all(spark::Utf8ToWide(conversation_base), remove_error);
    spark::SessionJournal journal;
    const auto opened = journal.Open(temp_directory, spark::CurrentDirectoryUtf8());
    Check(opened.ok, "session opens");
    if (opened.ok) {
        Check(journal.Append("message", message).ok, "session appends message");
        const std::string journal_path = journal.Path();
        spark::SessionJournal resumed;
        const auto resumed_result = resumed.Resume(journal_path);
        Check(resumed_result.ok, "session resumes");
        Check(resumed.State().messages.size() == 1, "session message restored");
        Check(resumed.State().messages.front().timestamp == message.timestamp,
              "session message timestamp restored");
        const auto recent_after_resume = resumed.ListRecent(10);
        Check(recent_after_resume.ok && !recent_after_resume.value.empty(),
              "resumed journal still lists recent sessions");
        // A cold start never calls Open() or Resume(), so the sidebar has to recover the
        // conversation list from the configured storage root alone.
        spark::SessionJournal cold_start;
        const auto recent_without_root = cold_start.ListRecent(10);
        Check(recent_without_root.ok && recent_without_root.value.empty(),
              "a journal without a storage root lists nothing");
        cold_start.SetStorageRoot(temp_directory);
        const auto recent_after_root = cold_start.ListRecent(10);
        Check(recent_after_root.ok && !recent_after_root.value.empty(),
              "cold-start storage root restores the recent conversation list");
    }

    spark::SessionJournal timing;
    const auto timing_opened = timing.Open(temp_directory, spark::CurrentDirectoryUtf8());
    Check(timing_opened.ok, "timing test session opens");
    if (timing_opened.ok) {
        const auto append_message = [&](spark::MessageRole role, const std::string& content) {
            spark::ChatMessage item;
            item.role = role;
            item.content = content;
            timing.MutableState().messages.push_back(item);
            Check(timing.Append("message", item).ok, "timing test message appends");
        };
        append_message(spark::MessageRole::User, "first turn");
        append_message(spark::MessageRole::Assistant, "first answer");
        Check(timing.Append("turn.duration", {{"turn_index", 0}, {"elapsed_ms", 111}}).ok,
              "indexed turn duration appends");
        Check(timing.Append("turn.error", {{"turn_index", 0},
                                           {"error", "HTTP 200 text/event-stream: provider returned an empty stream"},
                                           {"timestamp", uint64_t{12345}}})
                  .ok,
              "turn error appends");
        Check(timing.Append("turn.usage", {{"turn_index", 0},
                                           {"input_tokens", 1000},
                                           {"output_tokens", 120},
                                           {"cached_input_tokens", 800},
                                           {"model_calls", 1},
                                           {"context_tokens", 1120},
                                           {"context_limit_tokens", 258000},
                                           {"elapsed_ms", 111}})
                  .ok,
              "indexed turn usage appends");

        // Simulate an old snapshot written by a version that did not keep the
        // turn-duration metadata in memory. Replay must not lose the event data.
        nlohmann::json stale_snapshot = {
            {"id", timing.State().id},
            {"title", timing.State().title},
            {"workspace", timing.State().workspace},
            {"created_at", timing.State().created_at},
            {"updated_at", timing.State().updated_at},
            {"messages", timing.State().messages},
            {"context_messages", nlohmann::json::array()},
            {"metadata", nlohmann::json::object()},
        };
        Check(timing.Append("snapshot", stale_snapshot).ok, "stale snapshot appends");

        append_message(spark::MessageRole::User, "second turn");
        append_message(spark::MessageRole::Assistant, "second answer");
        Check(timing.Append("turn.completed", {{"elapsed_ms", 222}}).ok,
              "legacy turn duration appends");
        Check(timing.Append("turn.usage", {{"turn_index", 1},
                                           {"input_tokens", 2000},
                                           {"output_tokens", 240},
                                           {"cached_input_tokens", 1500},
                                           {"model_calls", 2},
                                           {"context_tokens", 2240},
                                           {"context_limit_tokens", 258000},
                                           {"elapsed_ms", 222}})
                  .ok,
              "legacy turn usage appends");

        spark::SessionJournal timing_resumed;
        Check(timing_resumed.Resume(timing.Path()).ok, "timing journal resumes");
        const auto& durations = timing_resumed.State().metadata["turn_durations_ms"];
        Check(durations.is_array() && durations.size() == 2,
              "turn durations remain aligned with user turns");
        Check(durations.is_array() && durations.size() == 2 && durations[0].get<uint64_t>() == 111 &&
                  durations[1].get<uint64_t>() == 222,
              "indexed and legacy turn durations map to the correct turns");
        const auto& usages = timing_resumed.State().metadata["turn_usages"];
        Check(usages.is_array() && usages.size() == 2,
              "turn usages remain aligned with user turns");
        Check(usages.is_array() && usages.size() == 2 && usages[0].value("input_tokens", uint64_t{0}) == 1000 &&
                  usages[0].value("cached_input_tokens", uint64_t{0}) == 800 &&
                  usages[1].value("output_tokens", uint64_t{0}) == 240 &&
                  usages[1].value("model_calls", uint64_t{0}) == 2,
              "turn usage values survive replay and snapshots");
        const auto& errors = timing_resumed.State().metadata["turn_errors"];
        Check(errors.is_array() && errors.size() == 1, "turn errors survive a snapshot reload");
        Check(errors.is_array() && errors.size() == 1 && errors[0].value("turn_index", int64_t{-1}) == 0 &&
                  errors[0].value("error", std::string{}) ==
                      "HTTP 200 text/event-stream: provider returned an empty stream" &&
                  errors[0].value("timestamp", uint64_t{0}) == 12345,
              "turn error keeps its turn and event timestamp");
        Check(timing_resumed.State().messages.size() >= 2 &&
                  timing_resumed.State().messages[0].timestamp != 0 &&
                  timing_resumed.State().messages[1].timestamp != 0,
              "legacy snapshot messages recover timestamps from journal events");
    }

    spark::SessionJournal edited;
    Check(edited.Open(temp_directory, spark::CurrentDirectoryUtf8()).ok, "edited error session opens");
    const auto add_edited_user = [&](const std::string& text) {
        spark::ChatMessage user;
        user.role = spark::MessageRole::User;
        user.content = text;
        edited.MutableState().messages.push_back(user);
        Check(edited.Append("message", user).ok, "edited session appends user");
    };
    add_edited_user("earlier turn");
    Check(edited.Append("turn.error", {{"turn_index", 0}, {"error", "earlier error"}}).ok,
          "earlier turn error appends");
    add_edited_user("retry this");
    Check(edited.Append("turn.error", {{"turn_index", 1}, {"error", "old error"}}).ok, "old error appends");
    Check(edited.Append("turn.duration", {{"turn_index", 1}, {"elapsed_ms", 99}}).ok, "old duration appends");
    Check(edited.Append("turn.usage", {{"turn_index", 1}, {"input_tokens", 999}}).ok, "old usage appends");
    Check(edited.SaveSnapshot().ok, "failed turn snapshot saves");
    for (int retry = 0; retry < 3; ++retry) {
        edited.MutableState().messages.resize(1);
        edited.MutableState().metadata["turn_errors"].erase(
            edited.MutableState().metadata["turn_errors"].begin() + 1,
            edited.MutableState().metadata["turn_errors"].end());
        edited.MutableState().metadata["turn_durations_ms"] = nlohmann::json::array();
        edited.MutableState().metadata["turn_usages"] = nlohmann::json::array();
        Check(edited.SaveSnapshot().ok, "edited branch snapshot saves");
        Check(edited.Resume(edited.Path()).ok, "edited branch resumes before resend");
        Check(edited.State().metadata["turn_errors"].size() == 1,
              "edited branch does not resurrect the removed error");
        Check(edited.State().metadata["turn_durations_ms"].empty() &&
                  edited.State().metadata["turn_usages"].empty(),
              "edited branch does not resurrect removed duration or usage");
        add_edited_user("retry this");
        Check(edited.Append("turn.error", {{"turn_index", 1}, {"error", "latest error"}}).ok, "retry error appends");
        Check(edited.Append("turn.error", {{"turn_index", 1}, {"error", "replacement error"}}).ok,
              "replacement error appends");
        Check(edited.SaveSnapshot().ok, "retried branch snapshot saves");
        Check(edited.Resume(edited.Path()).ok, "retried branch resumes");
        const auto& retry_errors = edited.State().metadata["turn_errors"];
        Check(retry_errors.size() == 2 && retry_errors[0].value("error", "") == "earlier error" &&
                  retry_errors[1].value("error", "") == "replacement error",
              "retry replaces only its own error and preserves the earlier turn");
    }

    spark::SessionJournal legacy;
    const auto legacy_opened = legacy.Open(temp_directory, spark::CurrentDirectoryUtf8());
    Check(legacy_opened.ok, "legacy compaction test session opens");
    if (legacy_opened.ok) {
        spark::ChatMessage old_user;
        old_user.role = spark::MessageRole::User;
        old_user.content = "old visible user message";
        spark::ChatMessage old_assistant;
        old_assistant.role = spark::MessageRole::Assistant;
        old_assistant.content = "old visible assistant message";
        Check(legacy.Append("message", old_user).ok, "legacy transcript user event appends");
        Check(legacy.Append("message", old_assistant).ok, "legacy transcript assistant event appends");
        Check(legacy.Append("context.compacted", {{"trigger", "manual"}, {"summary", "legacy handoff"}}).ok,
              "legacy compaction event appends");

        spark::ChatMessage handoff;
        handoff.role = spark::MessageRole::System;
        handoff.content = "Conversation history handoff summary:\nlegacy handoff";
        nlohmann::json compacted_messages = nlohmann::json::array();
        compacted_messages.push_back(handoff);
        nlohmann::json legacy_snapshot = {
            {"id", legacy.State().id},
            {"title", "legacy compacted"},
            {"workspace", spark::CurrentDirectoryUtf8()},
            {"created_at", legacy.State().created_at},
            {"updated_at", legacy.State().updated_at},
            {"messages", compacted_messages},
            {"metadata", nlohmann::json::object()},
        };
        Check(legacy.Append("snapshot", legacy_snapshot).ok, "legacy compacted snapshot appends");
        spark::ChatMessage new_user;
        new_user.role = spark::MessageRole::User;
        new_user.content = "new message after legacy compaction";
        Check(legacy.Append("message", new_user).ok, "post-compaction message event appends");

        spark::SessionJournal migrated;
        Check(migrated.Resume(legacy.Path()).ok, "legacy compacted journal resumes");
        Check(migrated.State().messages.size() == 3, "legacy compaction restores the visible transcript");
        Check(migrated.State().context_messages.size() == 2,
              "legacy compaction snapshot becomes bounded model context");
        Check(migrated.State().messages.front().content == "old visible user message",
              "legacy transcript keeps the first visible message");
        Check(migrated.State().messages.back().content == "new message after legacy compaction",
              "new messages append to the restored transcript");
    }
    spark::fs::remove_all(spark::Utf8ToWide(temp_directory), remove_error);

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all core tests passed\n";
    return 0;
}
