#include <winsock2.h>
#include <ws2tcpip.h>

#include "core/provider_probe.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

enum class ReplyMode { Complete, HoldHeaders, HoldBody };

class ProbeServer {
public:
    ProbeServer(std::string body, bool streaming = false, int status = 200,
                ReplyMode mode = ReplyMode::Complete)
        : body_(std::move(body)), streaming_(streaming), status_(status), mode_(mode) {
        WSADATA data{};
        started_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        if (!started_) return;
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) return;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int length = sizeof(address);
        if (bind(listener_, reinterpret_cast<sockaddr*>(&address), length) != 0 ||
            listen(listener_, 4) != 0 ||
            getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) return;
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this] { Run(); });
    }

    ~ProbeServer() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (listener_ != INVALID_SOCKET) closesocket(listener_);
        if (started_) WSACleanup();
    }

    int Port() const { return port_; }
    std::string Request() {
        std::lock_guard<std::mutex> lock(mutex_);
        return request_;
    }
    bool WaitForRequest(ReplyMode phase) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(5), [&] {
            return !request_.empty() && (phase != ReplyMode::HoldBody || body_started_);
        });
    }

private:
    bool SendAll(SOCKET client, const std::string& bytes) {
        for (size_t offset = 0; offset < bytes.size();) {
            const int count = send(client, bytes.data() + offset,
                                   static_cast<int>(std::min<size_t>(8192, bytes.size() - offset)), 0);
            if (count <= 0) return false;
            offset += static_cast<size_t>(count);
        }
        return true;
    }
    void Hold() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait_for(lock, std::chrono::seconds(5), [&] { return stop_; });
    }
    void Run() {
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stop_) return;
            }
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(listener_, &readable);
            timeval timeout{0, 100000};
            if (select(0, &readable, nullptr, nullptr, &timeout) > 0) break;
        }
        const SOCKET client = accept(listener_, nullptr, nullptr);
        if (client == INVALID_SOCKET) return;
        const DWORD io_timeout = 1000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&io_timeout), sizeof(io_timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&io_timeout), sizeof(io_timeout));
        std::string request;
        char buffer[4096];
        while (request.size() < 65536) {
            const int count = recv(client, buffer, sizeof(buffer), 0);
            if (count <= 0) break;
            request.append(buffer, static_cast<size_t>(count));
            const size_t end = request.find("\r\n\r\n");
            if (end == std::string::npos) continue;
            const std::string headers = spark::LowerAscii(request.substr(0, end));
            const size_t length = headers.find("content-length:");
            const size_t body_size = length == std::string::npos ? 0 : std::stoul(headers.substr(length + 15));
            if (request.size() >= end + 4 + body_size) break;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            request_ = std::move(request);
        }
        condition_.notify_all();
        if (mode_ == ReplyMode::HoldHeaders) Hold();
        const std::string headers = "HTTP/1.1 " + std::to_string(status_) +
            " Fixture\r\nContent-Type: " + (streaming_ ? "text/event-stream" : "application/json") +
            "\r\nContent-Length: " + std::to_string(body_.size() + (mode_ == ReplyMode::HoldBody ? 32 : 0)) +
            "\r\nConnection: close\r\n\r\n";
        if (SendAll(client, headers) && SendAll(client, body_)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                body_started_ = true;
            }
            condition_.notify_all();
            if (mode_ == ReplyMode::HoldBody) Hold();
        }
        shutdown(client, SD_SEND);
        closesocket(client);
    }

    std::string body_;
    bool streaming_;
    int status_;
    ReplyMode mode_;
    bool started_ = false;
    SOCKET listener_ = INVALID_SOCKET;
    int port_ = 0;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stop_ = false;
    bool body_started_ = false;
    std::string request_;
};

spark::ProviderConfig MakeProvider(const ProbeServer& server, const std::string& protocol) {
    Check(server.Port() != 0, "probe loopback fixture starts");
    spark::ProviderConfig provider;
    provider.id = "probe-fixture";
    provider.protocol = protocol;
    provider.base_url = "http://127.0.0.1:" + std::to_string(server.Port()) + "/v1/";
    provider.api_key = "synthetic-probe-key";
    provider.headers["X-Fixture-Token"] = "synthetic-header-secret";
    provider.models.push_back({"first-model", "First", {}});
    provider.models.push_back({"second-model", "Second", {}});
    for (auto& model : provider.models) model.capabilities.streaming = false;
    return provider;
}

nlohmann::json Snapshot(const spark::ProviderConfig& provider) {
    spark::AppConfig config;
    config.providers.push_back(provider);
    auto snapshot = spark::ConfigToJson(config);
    snapshot["resolved_key"] = provider.api_key;
    return snapshot;
}

std::string Reply(const std::string& protocol, bool streaming, const std::string& text) {
    if (protocol == "anthropic_messages") {
        if (!streaming) return nlohmann::json{
            {"content", nlohmann::json::array({{{"type", "text"}, {"text", text}}})},
            {"stop_reason", "end_turn"}}.dump();
        return "event: content_block_delta\ndata: " + nlohmann::json{
            {"type", "content_block_delta"}, {"index", 0},
            {"delta", {{"type", "text_delta"}, {"text", text}}}}.dump() +
            "\n\nevent: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
    }
    if (protocol == "responses" || protocol == "openai_responses") {
        if (!streaming) return nlohmann::json{{"status", "completed"},
            {"output", nlohmann::json::array({{{"type", "message"},
                {"content", nlohmann::json::array({{{"type", "output_text"}, {"text", text}}})}}})}}.dump();
        return "data: " + nlohmann::json{{"type", "response.output_text.delta"}, {"delta", text}}.dump() +
            "\n\ndata: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n";
    }
    if (!streaming) return nlohmann::json{{"choices", nlohmann::json::array({
        {{"message", {{"role", "assistant"}, {"content", text}}}, {"finish_reason", "stop"}}})}}.dump();
    return "data: " + nlohmann::json{{"choices", nlohmann::json::array({
        {{"delta", {{"content", text}}}}})}}.dump() + "\n\ndata: [DONE]\n\n";
}

void CheckFailure(const spark::Result<spark::ProviderProbeResult>& result) {
    Check(!result.ok && !result.error.empty() && result.error.size() <= 512 &&
              result.value.model_id.size() <= 512 && result.value.text.empty(),
          "failed probe returns a bounded error and no partial success");
    for (const char* secret : {"synthetic-probe-key", "synthetic-header-secret",
                               "synthetic-env-key", "synthetic-expression-key"}) {
        Check(result.error.find(secret) == std::string::npos &&
                  result.value.model_id.find(secret) == std::string::npos,
              "probe failure does not leak credentials");
    }
}

void TestProtocols() {
    for (const char* protocol : {"openai_chat_completions", "chat_completions", "openai_responses",
                                  "responses", "anthropic_messages"}) {
        for (const bool streaming : {false, true}) {
            ProbeServer server(Reply(protocol, streaming, "hello"), streaming);
            auto provider = MakeProvider(server, protocol);
            provider.models[0].capabilities.streaming = streaming;
            const auto before = Snapshot(provider);
            const auto started = std::chrono::steady_clock::now();
            const auto result = spark::ProbeProvider(provider);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            Check(result.ok && result.value.model_id == "first-model" && result.value.text == "hello" &&
                      result.value.http_status == 200 &&
                      result.error.empty() && result.value.elapsed_ms >= 0 && result.value.elapsed_ms <= elapsed,
                  "actual protocol adapter returns model, text and elapsed time");
            const std::string request = server.Request();
            const bool responses = std::string(protocol).find("responses") != std::string::npos;
            const bool anthropic = std::string(protocol) == "anthropic_messages";
            const std::string endpoint = responses ? "responses" : anthropic ? "messages" : "chat/completions";
            Check(request.rfind("POST /v1/" + endpoint + " HTTP/1.1\r\n", 0) == 0,
                  "probe posts to the actual protocol endpoint");
            Check(request.find((anthropic ? "x-api-key: " : "Authorization: Bearer ") +
                      provider.api_key + "\r\n") != std::string::npos &&
                      request.find("X-Fixture-Token: synthetic-header-secret\r\n") != std::string::npos,
                  "probe uses configured authentication and headers");
            const size_t header_end = request.find("\r\n\r\n");
            const auto body = nlohmann::json::parse(
                header_end == std::string::npos ? "" : request.substr(header_end + 4), nullptr, false);
            Check(body.is_object(), "probe sends a JSON body");
            if (!body.is_object()) continue;
            const auto& messages = body.at(responses ? "input" : "messages");
            const nlohmann::json content = responses ? nlohmann::json::array({{{"type", "input_text"}, {"text", "hi"}}}) :
                anthropic ? nlohmann::json::array({{{"type", "text"}, {"text", "hi"}}}) : nlohmann::json("hi");
            auto expected = nlohmann::json{{"role", "user"}, {"content", content}};
            if (responses) expected["type"] = "message";
            Check(messages == nlohmann::json::array({expected}) && body.at("model") == "first-model" &&
                      body.at("stream") == streaming && !body.contains("tools") && !body.contains("system") &&
                      !body.contains("instructions") && !body.contains("previous_response_id"),
                  "probe sends exactly one user hi, without tools, history, images or instructions");
            Check(Snapshot(provider) == before, "probe leaves provider and model capabilities unchanged");
        }
    }
}

void TestModelSelection() {
    for (int scenario = 0; scenario < 6; ++scenario) {
        ProbeServer server(Reply("responses", false, "selected"));
        auto provider = MakeProvider(server, "responses");
        std::string expected = scenario >= 3 ? "first-model" : "second-model";
        if (scenario == 0) { provider.test_model = "first-model"; expected = "first-model"; }
        if (scenario == 1) { provider.test_model = "unlisted-test-model"; expected = provider.test_model; }
        if (scenario == 2) provider.test_model = "second-model";
        if (scenario == 5) provider.test_model = "  ";
        // Unlisted test models copy the first model's non-streaming capability.
        provider.models[1].capabilities.streaming = scenario < 2;
        const auto before = Snapshot(provider);
        const auto result = spark::ProbeProvider(provider);
        Check(result.ok && result.value.model_id == expected && Snapshot(provider) == before,
              "test_model wins, otherwise the first model is used regardless of the active model");
        Check(server.Request().find("\"model\":\"" + expected + "\"") != std::string::npos,
              "selected model ID is sent to the provider");
    }
    ProbeServer server(Reply("responses", true, "test-only"), true);
    auto provider = MakeProvider(server, "responses");
    provider.models.clear();
    provider.test_model = "test-only";
    const auto result = spark::ProbeProvider(provider);
    Check(result.ok && result.value.model_id == "test-only", "test_model works without a saved model list");
}

void TestErrorsAndBounds() {
    for (const char* protocol : {"chat_completions", "responses", "anthropic_messages"}) {
        for (const int status : {400, 401, 403, 429, 500}) {
            ProbeServer server(R"({"error":{"message":"upstream failure synthetic-probe-key synthetic-header-secret"}})",
                               false, status);
            auto provider = MakeProvider(server, protocol);
            const auto result = spark::ProbeProvider(provider);
            CheckFailure(result);
            Check(result.value.http_status == status && result.value.model_id == "first-model",
                  "HTTP errors retain the actual status code and test model");
        }
        for (const char* body : {"", "{synthetic-probe-key", "null", "{}"}) {
            ProbeServer server(body);
            const auto result = spark::ProbeProvider(MakeProvider(server, protocol));
            CheckFailure(result);
            Check(result.value.http_status == 200, "invalid response bodies retain their successful HTTP status");
        }
        ProbeServer failed_stream("event: error\ndata: {\"type\":\"error\",\"error\":{\"message\":"
                                  "\"synthetic-probe-key synthetic-header-secret\"}}\n\n", true);
        auto provider = MakeProvider(failed_stream, protocol);
        provider.models[0].capabilities.streaming = true;
        CheckFailure(spark::ProbeProvider(provider));
    }
    std::string unicode;
    for (int index = 0; index < 300; ++index) unicode += "\xe4\xb8\xad";
    for (const bool fail : {false, true}) {
        // Redaction must precede truncation, including when a key straddles byte 512.
        const std::string text = std::string(500, 'x') + "synthetic-probe-key" + unicode;
        ProbeServer server(fail ? nlohmann::json{{"error", {{"message", text}}}}.dump() :
                                  Reply("responses", false, text), false, fail ? 401 : 200);
        const auto result = spark::ProbeProvider(MakeProvider(server, "responses"));
        if (fail) CheckFailure(result);
        Check((fail ? result.error : result.value.text).find("synthetic-") == std::string::npos,
              "redaction precedes truncation even when a secret crosses the display boundary");
        if (!fail) Check(result.ok && result.value.text.size() <= 512 && spark::IsValidUtf8(result.value.text),
                         "success echo is UTF-8 safe, redacted and at most 512 bytes");
    }
    ProbeServer server(Reply("chat_completions", false, unicode));
    const auto result = spark::ProbeProvider(MakeProvider(server, "chat_completions"));
    Check(result.ok && result.value.text.size() == 510 && spark::IsValidUtf8(result.value.text),
          "512-byte truncation does not split a multi-byte character");
}

void TestCredentialsAndValidation() {
    const wchar_t* name = L"SPARK_PROVIDER_PROBE_FIXTURE_KEY";
    if (GetEnvironmentVariableW(name, nullptr, 0) != 0) {
        Check(false, "synthetic environment name must be unused; existing value left untouched");
        return;
    }
    Check(SetEnvironmentVariableW(name, L"synthetic-env-key") != FALSE, "set synthetic environment key");
    for (const std::string expression : {"${ENV:SPARK_PROVIDER_PROBE_FIXTURE_KEY}", "synthetic-expression-key"}) {
        const std::string expected = expression.front() == '$' ? "synthetic-env-key" : expression;
        for (const bool fail : {false, true}) {
            const std::string text = expected +
                " synthetic-probe-key synthetic-header-secret synthetic-\x1b[31mprobe-key Z7!";
            ProbeServer server(fail ? nlohmann::json{{"error", {{"message", text}}}}.dump() :
                                      Reply("responses", false, text), false, fail ? 401 : 200);
            auto provider = MakeProvider(server, "responses");
            provider.api_key_expression = expression;
            provider.headers["X-Short"] = "Z7!";
            provider.headers["X-Prefix"] = "synthetic-probe";
            const auto before = Snapshot(provider);
            const auto result = spark::ProbeProvider(provider);
            if (fail) CheckFailure(result);
            else Check(result.ok, "credential-redacting success remains successful");
            const std::string output = fail ? result.error : result.value.text;
            Check(output.find("synthetic-") == std::string::npos && output.find("-key") == std::string::npos &&
                      output.find("Z7!") == std::string::npos && Snapshot(provider) == before,
                  "redacts resolved, literal, stale, overlapping, control-split and short secrets");
            Check(server.Request().find("Authorization: Bearer " + expected + "\r\n") != std::string::npos,
                  "existing key-expression rules override the stale resolved key");
        }
    }
    Check(SetEnvironmentVariableW(name, nullptr) != FALSE, "remove synthetic environment key");
    for (const bool fail : {false, true}) {
        const std::string secret = "synthetic-quoted-\"\\-key";
        const std::string text = "quoted " + nlohmann::json(secret).dump();
        ProbeServer server(fail ? nlohmann::json{{"error", {{"message", text}}}}.dump() :
                                  Reply("responses", false, text), false, fail ? 401 : 200);
        auto provider = MakeProvider(server, "responses");
        provider.headers["X-Quoted"] = secret;
        const auto result = spark::ProbeProvider(provider);
        Check((fail ? !result.ok : result.ok) &&
                  (fail ? result.error : result.value.text).find("synthetic-") == std::string::npos,
              "JSON-escaped header credentials are redacted on both success and failure");
    }
    {
        ProbeServer server(R"({"error":{"message":"\u001b[31m"}})", false, 401);
        CheckFailure(spark::ProbeProvider(MakeProvider(server, "chat_completions")));
    }
    ProbeServer server(Reply("responses", false, "unused"));
    auto provider = MakeProvider(server, "responses");
    provider.models.clear();
    CheckFailure(spark::ProbeProvider(provider));
    provider.models.push_back({"", "", {}});
    CheckFailure(spark::ProbeProvider(provider));
    provider.models[0].id = "valid";
    provider.protocol = "unsupported";
    CheckFailure(spark::ProbeProvider(provider));
    provider.protocol = "responses";
    provider.base_url.clear();
    CheckFailure(spark::ProbeProvider(provider));
    provider.base_url = "not-a-url";
    const auto invalid_url = spark::ProbeProvider(provider);
    CheckFailure(invalid_url);
    Check(invalid_url.value.http_status == 0, "a request without a response never invents an HTTP status");
    Check(server.Request().empty(), "local validation failures send no HTTP request");
}

void TestCancellation() {
    for (const auto phase : {ReplyMode::Complete, ReplyMode::HoldHeaders, ReplyMode::HoldBody}) {
        ProbeServer server(phase == ReplyMode::HoldBody ? "" : "partial", false, 200, phase);
        const auto provider = MakeProvider(server, "responses");
        spark::CancellationToken cancellation;
        if (phase == ReplyMode::Complete) cancellation.Cancel();
        spark::Result<spark::ProviderProbeResult> result;
        std::thread probe([&] { result = spark::ProbeProvider(provider, &cancellation); });
        if (phase != ReplyMode::Complete) Check(server.WaitForRequest(phase), "probe reaches cancellation phase");
        if (phase == ReplyMode::HoldBody) std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const auto started = std::chrono::steady_clock::now();
        cancellation.Cancel();
        probe.join();
        CheckFailure(result);
        Check(result.error == "cancelled" && std::chrono::steady_clock::now() - started < std::chrono::seconds(2),
              "pre-cancellation and blocked header/body cancellation return promptly");
        if (phase == ReplyMode::Complete) Check(server.Request().empty(), "pre-cancellation sends no request");
    }
}

}  // namespace

int main() {
    TestProtocols();
    TestModelSelection();
    TestErrorsAndBounds();
    TestCredentialsAndValidation();
    TestCancellation();
    if (failures != 0) {
        std::cerr << failures << " provider probe test(s) failed\n";
        return 1;
    }
    std::cout << "provider probe tests passed\n";
    return 0;
}
