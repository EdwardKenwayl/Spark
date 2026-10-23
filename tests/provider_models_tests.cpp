#include <winsock2.h>
#include <ws2tcpip.h>

#include "core/provider_models.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

int failures = 0;
constexpr size_t kMaxBodyBytes = 1024 * 1024;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

enum class ReplyMode { Complete, HoldHeaders, HoldBody, Truncated };

class ModelsServer {
public:
    explicit ModelsServer(std::string body, int status = 200, ReplyMode mode = ReplyMode::Complete)
        : body_(std::move(body)), status_(status), mode_(mode) {
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

    ~ModelsServer() {
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

    bool WaitForRequest(bool body_started = false) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(5), [&] {
            return !request_.empty() && (!body_started || body_started_);
        });
    }

    std::string Request() {
        std::lock_guard<std::mutex> lock(mutex_);
        return request_;
    }

    size_t RequestCount() {
        std::lock_guard<std::mutex> lock(mutex_);
        return request_count_;
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
            if (select(0, &readable, nullptr, nullptr, &timeout) <= 0) continue;
            const SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            const DWORD io_timeout = 1000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&io_timeout), sizeof(io_timeout));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&io_timeout), sizeof(io_timeout));
            std::string request;
            char buffer[4096];
            while (request.find("\r\n\r\n") == std::string::npos && request.size() < 65536) {
                const int count = recv(client, buffer, sizeof(buffer), 0);
                if (count <= 0) break;
                request.append(buffer, static_cast<size_t>(count));
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                request_ = std::move(request);
                ++request_count_;
            }
            condition_.notify_all();
            if (mode_ == ReplyMode::HoldHeaders) Hold();
            const size_t content_length = body_.size() +
                (mode_ == ReplyMode::HoldBody || mode_ == ReplyMode::Truncated ? 32 : 0);
            const std::string headers = "HTTP/1.1 " + std::to_string(status_) +
                " Fixture\r\nContent-Type: application/json\r\nContent-Length: " +
                std::to_string(content_length) + "\r\nConnection: close\r\n\r\n";
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
    }

    std::string body_;
    int status_ = 200;
    ReplyMode mode_ = ReplyMode::Complete;
    bool started_ = false;
    SOCKET listener_ = INVALID_SOCKET;
    int port_ = 0;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stop_ = false;
    bool body_started_ = false;
    std::string request_;
    size_t request_count_ = 0;
};

class ScopedEnvironment {
public:
    ScopedEnvironment(const wchar_t* name, const wchar_t* value) : name_(name) {
        const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
        if (size != 0) {
            previous_.resize(size);
            const DWORD count = GetEnvironmentVariableW(name, previous_.data(), size);
            previous_.resize(count);
            existed_ = true;
        }
        Check(SetEnvironmentVariableW(name, value) != FALSE, "fixture environment is set");
    }

    ~ScopedEnvironment() {
        Check(SetEnvironmentVariableW(name_.c_str(), existed_ ? previous_.c_str() : nullptr) != FALSE,
              "fixture environment is restored");
    }

private:
    std::wstring name_;
    std::wstring previous_;
    bool existed_ = false;
};

spark::ProviderConfig MakeProvider(const ModelsServer& server) {
    spark::ProviderConfig provider;
    provider.id = "models-fixture";
    provider.base_url = "http://127.0.0.1:" + std::to_string(server.Port()) + "/v1/";
    provider.api_key = "synthetic-models-key";
    provider.headers["X-Fixture-Token"] = "synthetic-header-secret";
    provider.models.push_back({"configured-model", "Configured Model", {}});
    return provider;
}

nlohmann::json Snapshot(const spark::ProviderConfig& provider) {
    spark::AppConfig config;
    config.providers.push_back(provider);
    return spark::ConfigToJson(config);
}

std::map<std::string, std::string> RequestHeaders(const std::string& request) {
    std::map<std::string, std::string> headers;
    size_t position = request.find("\r\n");
    if (position == std::string::npos) return headers;
    position += 2;
    for (;;) {
        const size_t end = request.find("\r\n", position);
        if (end == std::string::npos || end == position) return headers;
        const size_t colon = request.find(':', position);
        if (colon < end) {
            const std::string name = spark::LowerAscii(request.substr(position, colon - position));
            Check(headers.count(name) == 0, "request has no duplicate case-insensitive header names");
            headers[name] = spark::Trim(request.substr(colon + 1, end - colon - 1));
        }
        position = end + 2;
    }
}

void CheckFailure(const spark::Result<std::vector<std::string>>& result, const char* message) {
    Check(!result.ok && result.value.empty() && !result.error.empty(), message);
    for (const char* secret : {"synthetic-models-key", "synthetic-header-secret", "upstream-private-body"}) {
        Check(result.error.find(secret) == std::string::npos, "failure does not disclose credentials or upstream body");
    }
}

void TestSuccess() {
    const std::vector<std::string> expected{"model-b", "model-a", "Model/Case:1"};
    for (const char* protocol : {"chat_completions", "responses", "openai_responses"}) {
        ModelsServer server(R"({"data":[{"id":"model-b"},{"id":"model-a"},{"id":"model-b"},{"id":""},
            {"id":null},{"id":23},{"id":true},null,{},4,"ignored",{"id":"Model/Case:1"}]})");
        Check(server.Port() != 0, "success fixture starts");
        if (server.Port() == 0) return;
        auto provider = MakeProvider(server);
        provider.protocol = protocol;
        provider.headers["aCcEpT"] = "text/plain";
        const auto before = Snapshot(provider);
        const auto result = spark::FetchProviderModelIds(provider);
        Check(result.ok && result.value == expected && result.error.empty(), "model IDs are deduplicated in response order");
        const std::string request = server.Request();
        const auto headers = RequestHeaders(request);
        Check(request.rfind("GET /v1/models HTTP/1.1\r\n", 0) == 0, "models request uses GET and the connection-check URL");
        Check(headers.count("authorization") && headers.at("authorization") == "Bearer synthetic-models-key",
              "default authentication uses bearer");
        Check(headers.count("accept") && headers.at("accept") == "application/json", "models request accepts JSON");
        Check(headers.count("x-fixture-token") && headers.at("x-fixture-token") == "synthetic-header-secret",
              "custom headers are preserved");
        Check(request.find("\r\n\r\n") == request.size() - 4, "GET request has no body");
        Check(server.RequestCount() == 1 && Snapshot(provider) == before, "success does not mutate provider or retry");
    }
}

void TestEnvironmentAndAnthropic() {
    ScopedEnvironment environment(L"SPARK_PROVIDER_MODELS_FIXTURE_KEY", L"synthetic-env-key");
    ModelsServer server(R"({"data":[{"id":"anthropic-model"}]})");
    Check(server.Port() != 0, "environment fixture starts");
    if (server.Port() == 0) return;
    auto provider = MakeProvider(server);
    provider.protocol = " Anthropic_Messages ";
    provider.api_key_expression = "${ENV:SPARK_PROVIDER_MODELS_FIXTURE_KEY}";
    const auto before = Snapshot(provider);
    const auto result = spark::FetchProviderModelIds(provider);
    const auto headers = RequestHeaders(server.Request());
    Check(result.ok && result.value == std::vector<std::string>{"anthropic-model"}, "anthropic model list succeeds");
    Check(headers.count("x-api-key") && headers.at("x-api-key") == "synthetic-env-key",
          "environment expression overrides a stale resolved key");
    Check(headers.count("anthropic-version") && headers.at("anthropic-version") == "2023-06-01",
          "anthropic version default is supplied");
    Check(headers.count("authorization") == 0, "anthropic does not receive default bearer authentication");
    Check(provider.api_key == "synthetic-models-key" && Snapshot(provider) == before,
          "environment resolution does not mutate provider");
    Check(spark::EnvironmentVariable("SPARK_PROVIDER_MODELS_FIXTURE_KEY") == "synthetic-env-key",
          "fetch does not modify environment");
}

void TestCustomAuthentication() {
    for (const char* protocol : {"responses", "anthropic_messages"}) {
        ModelsServer server(R"({"data":[{"id":"custom-auth-model"}]})");
        Check(server.Port() != 0, "custom-auth fixture starts");
        if (server.Port() == 0) return;
        auto provider = MakeProvider(server);
        provider.protocol = protocol;
        provider.headers["aUtHoRiZaTiOn"] = "Custom fixture-auth";
        provider.headers["X-Api-Key"] = "custom-fixture-key";
        provider.headers["Anthropic-Version"] = "fixture-version";
        const auto result = spark::FetchProviderModelIds(provider);
        const auto headers = RequestHeaders(server.Request());
        Check(result.ok, "custom authentication succeeds");
        Check(headers.count("authorization") && headers.at("authorization") == "Custom fixture-auth" &&
              headers.count("x-api-key") && headers.at("x-api-key") == "custom-fixture-key" &&
              headers.count("anthropic-version") && headers.at("anthropic-version") == "fixture-version",
              "custom authentication headers are preserved case-insensitively");
    }
}

void TestInvalidResponses() {
    for (const char* body : {"", "{upstream-private-body", R"({"data":[]} trailing-upstream-private-body)",
                             "null", "[]", "{}", R"({"data":null})", R"({"data":{}})", R"({"data":[]})",
                             R"({"data":[{},null,{"id":""},{"id":3}]})"}) {
        ModelsServer server(body);
        Check(server.Port() != 0, "invalid-response fixture starts");
        if (server.Port() == 0) return;
        const auto provider = MakeProvider(server);
        const auto before = Snapshot(provider);
        CheckFailure(spark::FetchProviderModelIds(provider), "invalid JSON or empty/malformed list fails");
        Check(Snapshot(provider) == before, "invalid response leaves provider unchanged");
    }
    for (const int status : {401, 403, 429, 500}) {
        ModelsServer server("upstream-private-body synthetic-models-key synthetic-header-secret", status);
        Check(server.Port() != 0, "HTTP-status fixture starts");
        if (server.Port() == 0) return;
        const auto result = spark::FetchProviderModelIds(MakeProvider(server));
        CheckFailure(result, "non-success HTTP status fails");
        Check(result.error.find("HTTP " + std::to_string(status)) != std::string::npos,
              "failure identifies HTTP status without raw body");
        Check(server.RequestCount() == 1, "non-success response is not retried");
    }
}

void TestBounds() {
    nlohmann::json document = {{"data", nlohmann::json::array()}};
    std::vector<std::string> expected;
    for (size_t index = 0; index < 256; ++index) {
        expected.push_back("model-" + std::to_string(index));
        document["data"].push_back({{"id", expected.back()}});
    }
    document["data"].push_back({{"id", expected.front()}});
    {
        ModelsServer server(document.dump());
        Check(server.Port() != 0, "ID-limit fixture starts");
        if (server.Port() == 0) return;
        const auto result = spark::FetchProviderModelIds(MakeProvider(server));
        Check(result.ok && result.value == expected, "256 unique IDs plus duplicates succeed");
    }
    document["data"].push_back({{"id", "overflow-model"}});
    const std::string boundary_id(2048, 'm');
    std::string boundary_body = nlohmann::json({{"data", nlohmann::json::array({{{"id", boundary_id}}})}}).dump();
    boundary_body.resize(kMaxBodyBytes, ' ');
    const std::string deeply_nested = R"({"data":[{"id":"valid-prefix"}],"metadata":)" +
        std::string(80, '[') + "0" + std::string(80, ']') + "}";
    const nlohmann::json long_id_document = {
        {"data", nlohmann::json::array({{{"id", "valid-prefix"}}, {{"id", std::string(2049, 'x')}}})}
    };
    for (const auto& body : {document.dump(),
                            long_id_document.dump(), boundary_body + " ", deeply_nested}) {
        ModelsServer server(body);
        Check(server.Port() != 0, "oversize fixture starts");
        if (server.Port() == 0) return;
        CheckFailure(spark::FetchProviderModelIds(MakeProvider(server)), "oversize fails without a partial model list");
    }
    {
        ModelsServer server(boundary_body);
        Check(server.Port() != 0, "body-boundary fixture starts");
        if (server.Port() == 0) return;
        const auto result = spark::FetchProviderModelIds(MakeProvider(server));
        Check(result.ok && result.value == std::vector<std::string>{boundary_id},
              "exactly 1 MiB response and 2048-byte ID succeed");
    }
    {
        ModelsServer server(boundary_body + " ", 500);
        Check(server.Port() != 0, "oversize HTTP-error fixture starts");
        if (server.Port() == 0) return;
        const auto result = spark::FetchProviderModelIds(MakeProvider(server));
        CheckFailure(result, "oversize HTTP-error response fails");
        Check(result.error.find("HTTP 500") != std::string::npos, "HTTP status survives oversize body rejection");
    }
    ModelsServer truncated(R"({"data":[{"id":"partial-result"}]})", 200, ReplyMode::Truncated);
    Check(truncated.Port() != 0, "truncation fixture starts");
    if (truncated.Port() == 0) return;
    CheckFailure(spark::FetchProviderModelIds(MakeProvider(truncated)), "truncated transport cannot return a partial success");
}

void TestCancellation() {
    {
        ModelsServer server(R"({"data":[{"id":"must-not-fetch"}]})");
        Check(server.Port() != 0, "pre-cancellation fixture starts");
        if (server.Port() == 0) return;
        spark::CancellationToken cancellation;
        cancellation.Cancel();
        const auto result = spark::FetchProviderModelIds(MakeProvider(server), &cancellation);
        CheckFailure(result, "already cancelled request fails");
        Check(result.error == "cancelled" && server.RequestCount() == 0, "pre-cancellation sends no request");
    }
    for (const auto mode : {ReplyMode::HoldHeaders, ReplyMode::HoldBody}) {
        ModelsServer server(R"({"data":[{"id":"must-not-return"}]})", 200, mode);
        Check(server.Port() != 0, "in-flight cancellation fixture starts");
        if (server.Port() == 0) return;
        const auto provider = MakeProvider(server);
        const auto before = Snapshot(provider);
        spark::CancellationToken cancellation;
        spark::Result<std::vector<std::string>> result;
        std::thread fetch([&] { result = spark::FetchProviderModelIds(provider, &cancellation); });
        Check(server.WaitForRequest(mode == ReplyMode::HoldBody), "request reaches the intended cancellation phase");
        const auto started = std::chrono::steady_clock::now();
        cancellation.Cancel();
        fetch.join();
        CheckFailure(result, "in-flight cancellation fails without partial IDs");
        Check(result.error == "cancelled", "cancellation has a stable error");
        Check(std::chrono::steady_clock::now() - started < std::chrono::seconds(2),
              "cancellation interrupts blocked header/body reads promptly");
        Check(Snapshot(provider) == before && cancellation.IsCancelled(), "cancellation leaves provider and token unchanged");
    }
}

}  // namespace

int main() {
    TestSuccess();
    TestEnvironmentAndAnthropic();
    TestCustomAuthentication();
    TestInvalidResponses();
    TestBounds();
    TestCancellation();
    if (failures != 0) {
        std::cerr << failures << " provider models test(s) failed\n";
        return 1;
    }
    std::cout << "provider models tests passed\n";
    return 0;
}
