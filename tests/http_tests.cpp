#include "loopback_http.h"
#include "platform/http.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Server = spark::test::LoopbackHttpServer;
using spark::StreamAction;
constexpr size_t kPrefixLimit = 1024 * 1024;
int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

// A broken backend must fail the suite instead of stranding a joined client.
class SuiteDeadline {
public:
    SuiteDeadline() : worker_([this] {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!condition_.wait_for(lock, 19s, [&] { return done_; })) {
            std::fputs("FAIL: HTTP suite exceeded its 19-second deadline\n", stderr);
            std::fflush(stderr);
            std::_Exit(2);
        }
    }) {}
    ~SuiteDeadline() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        condition_.notify_all();
        worker_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool done_ = false;
    std::thread worker_;
};

spark::HttpRequest Request(const Server& server) {
    spark::HttpRequest request;
    request.url = server.Url("/fixture?q=one%20two");
    request.timeout_seconds = 3;
    request.header_timeout_seconds = 3;
    request.max_attempts = 1;
    return request;
}

std::string Header(const spark::HttpResponse& response, const std::string& name) {
    for (const auto& entry : response.headers) {
        if (spark::EqualsIgnoreCaseAscii(entry.first, name)) return entry.second;
    }
    return {};
}

bool IsTimeout(const std::string& error) {
    return error.find("timeout") != std::string::npos || error.find("timed out") != std::string::npos ||
        error.find("12002") != std::string::npos || error.find(u8"\u8d85\u65f6") != std::string::npos;
}

void TestRequestAndMetadata() {
    for (const std::string method : {"POST", "GET", "PATCH"}) {
        const std::string body("a\0b", 3);
        Server server([&](Server::Connection& peer, size_t) {
            peer.Pause(40ms);
            peer.Send(Server::Headers(201, body.size(),
                "Content-Type: application/octet-stream\r\nX-Fixture:   metadata \t\r\n") + body);
        });
        auto request = Request(server);
        request.method = method;
        request.body = method == "GET" ? "" : body;
        request.headers = {{"Content-Type", "application/octet-stream"},
                           {"X-Fixture-Request", "synthetic-value"}, {"Authorization", "Bearer fixture"},
                           {"X-Fixture-Empty", ""}};
        std::vector<int> statuses;
        request.on_status = [&](int status) { statuses.push_back(status); };
        std::string chunks;
        bool status_before_body = true;
        const auto result = spark::HttpRequestStreaming(request, [&](const char* data, size_t size) {
            status_before_body = status_before_body && statuses == std::vector<int>{201};
            chunks.append(data, size);
            return StreamAction::Continue;
        });
        Check(result.ok, "method/body/header request completes");
        Check(status_before_body && statuses == std::vector<int>{201}, "status arrives once, before body callbacks");
        const auto requests = server.Requests();
        Check(requests.size() == 1 && server.Error().empty(), "fixture receives one complete request");
        if (!requests.empty()) {
            const auto& sent = requests.front();
            Check(sent.method == method && sent.target == "/fixture?q=one%20two" && sent.body == request.body,
                  "method, query and binary body arrive unchanged");
            Check(sent.headers.count("x-fixture-request") && sent.headers.at("x-fixture-request") == "synthetic-value" &&
                      sent.headers.count("authorization") && sent.headers.at("authorization") == "Bearer fixture" &&
                      sent.headers.count("content-type") && sent.headers.at("content-type") == "application/octet-stream",
                  "explicit request headers arrive unchanged");
            Check(sent.headers.count("x-fixture-empty") && sent.headers.at("x-fixture-empty").empty(),
                  "an explicitly empty header remains present");
        }
        if (!result.ok) continue;
        Check(result.value.status == 201 && result.value.body == body && chunks == body,
              "status and binary response bytes are preserved even with stream=false");
        Check(Header(result.value, "x-fixture") == "metadata" &&
                  Header(result.value, "content-type") == "application/octet-stream",
              "response headers retain trimmed values with case-insensitive consumer lookup");
        Check(result.value.received_bytes == body.size() && result.value.header_ms >= 20 &&
                  result.value.total_ms >= result.value.header_ms, "response size and monotonic timing metadata are populated");
    }
}

void TestFragmentedStream() {
    const std::vector<std::string> fragments = {"da", "ta: \xf0", "\x9f\x9a", "\x80\n\n", "data: [DONE]\n\n"};
    std::string expected;
    for (const auto& fragment : fragments) expected += fragment;
    Server server([&](Server::Connection& peer, size_t) {
        if (!peer.Send("HTTP/1.1 200 OK\r\nTransfer-Encoding: chu") || !peer.Pause(20ms) ||
            !peer.Send("nked\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n")) return;
        for (size_t i = 0; i < fragments.size(); ++i) {
            if (i > 0 && !peer.Pause(300ms)) return;
            std::ostringstream chunk;
            chunk << std::hex << fragments[i].size() << "\r\n" << fragments[i] << "\r\n";
            if (!peer.Send(chunk.str())) return;
        }
        peer.Send("0\r\n\r\n");
    });
    auto request = Request(server);
    request.stream = true;
    request.timeout_seconds = 1;
    std::string received;
    const auto started = Clock::now();
    const auto result = spark::HttpRequestStreaming(request, [&](const char* data, size_t size) {
        received.append(data, size);
        return StreamAction::Continue;
    });
    Check(result.ok && received == expected, "fragmented chunk framing, SSE lines and split UTF-8 preserve body bytes");
    Check(Clock::now() - started >= 1100ms, "an active stream may outlive the one-second idle timeout");
    if (result.ok) Check(result.value.body == expected && result.value.received_bytes == expected.size(),
                         "chunk framing is excluded from body accumulation and received-byte metadata");
}

void TestStopAndCallbackCancel() {
    for (const auto action : {StreamAction::Stop, StreamAction::Cancel}) {
        Server server([](Server::Connection& peer, size_t) {
            if (peer.Send(Server::Headers(200, 1000, "X-Fixture: held\r\n") + "first")) peer.Hold();
        });
        auto request = Request(server);
        request.stream = true;
        request.max_attempts = 3;
        size_t bytes = 0;
        int callbacks = 0, status = 0;
        request.on_status = [&](int value) { status = value; };
        const auto started = Clock::now();
        const auto result = spark::HttpRequestStreaming(request, [&](const char*, size_t size) {
            ++callbacks;
            bytes += size;
            return action;
        });
        Check(Clock::now() - started < 1500ms, "Stop/Cancel returns promptly while the server holds the connection");
        Check(status == 200 && callbacks == 1 && bytes > 0 && bytes <= 5 && server.Requests().size() == 1,
              "Stop/Cancel does not call the sink again or replay the request");
        if (action == StreamAction::Stop) {
            Check(result.ok, "Stop is logical success without waiting for EOF or Content-Length");
            if (result.ok) Check(result.value.status == 200 && Header(result.value, "x-fixture") == "held" &&
                                     result.value.received_bytes == bytes && result.value.total_ms >= result.value.header_ms,
                                 "Stop retains status, headers, timing and delivered-byte metadata");
        } else {
            Check(!result.ok && result.error.find("cancel") != std::string::npos, "callback Cancel remains a cancellation failure");
        }
    }
}

void TestTokenCancellation() {
    for (const bool body_idle : {false, true}) {
        Server server([&](Server::Connection& peer, size_t) {
            if (!body_idle || peer.Send(Server::Headers(200, 100))) peer.Hold();
        });
        auto request = Request(server);
        request.max_attempts = 3;
        spark::CancellationToken token;
        std::mutex mutex;
        std::condition_variable condition;
        bool headers_seen = false;
        request.on_status = [&](int) {
            std::lock_guard<std::mutex> lock(mutex);
            headers_seen = true;
            condition.notify_all();
        };
        spark::Result<spark::HttpResponse> result;
        size_t callbacks = 0;
        std::thread client([&] {
            result = spark::HttpRequestStreaming(request, [&](const char*, size_t) {
                ++callbacks;
                return StreamAction::Continue;
            }, &token);
        });
        Check(server.WaitForRequests(1), "cancellation request reaches the loopback server");
        if (body_idle) {
            std::unique_lock<std::mutex> lock(mutex);
            Check(condition.wait_for(lock, 1500ms, [&] { return headers_seen; }), "body-idle cancellation observes complete headers");
        }
        std::this_thread::sleep_for(50ms);
        const auto started = Clock::now();
        token.Cancel();
        client.join();
        Check(Clock::now() - started < 1500ms, "token cancellation interrupts silent header/body waits promptly");
        Check(!result.ok && result.error.find("cancel") != std::string::npos && callbacks == 0 &&
                  server.Requests().size() == 1, "token cancellation needs no body callback and never retries");
    }
    Server server([](Server::Connection& peer, size_t) { peer.Send(Server::Headers(200, 0)); });
    auto request = Request(server);
    spark::CancellationToken token;
    token.Cancel();
    const auto result = spark::HttpRequestStreaming(request, {}, &token);
    Check(!result.ok && result.error.find("cancel") != std::string::npos && server.Requests().empty(),
          "a pre-cancelled request sends no network request");
}

void TestTimeoutPhases() {
    {
        Server server([](Server::Connection& peer, size_t) {
            if (peer.Pause(1300ms)) peer.Send(Server::Headers(200, 5) + "ready");
        });
        auto request = Request(server);
        request.timeout_seconds = 1;
        const auto result = spark::HttpRequestStreaming(request);
        Check(result.ok && result.value.body == "ready" && result.value.header_ms >= 1100,
              "the longer header deadline permits startup slower than the body idle timeout");
    }
    for (const bool body_idle : {false, true}) {
        Server server([&](Server::Connection& peer, size_t) {
            if (!body_idle || peer.Send(Server::Headers(200, 1))) peer.Hold();
        });
        auto request = Request(server);
        request.timeout_seconds = 1;
        request.header_timeout_seconds = body_idle ? 4 : 2;
        request.max_attempts = 3;
        std::vector<int> statuses;
        request.on_status = [&](int status) { statuses.push_back(status); };
        const auto started = Clock::now();
        const auto result = spark::HttpRequestStreaming(request);
        const auto elapsed = Clock::now() - started;
        Check(!result.ok && IsTimeout(result.error), "a silent header/body phase reports a timeout failure");
        Check(elapsed >= (body_idle ? 800ms : 1800ms) && elapsed < (body_idle ? 2300ms : 3300ms),
              "each timeout uses its own deadline and is not multiplied by retries");
        Check(statuses == (body_idle ? std::vector<int>{200} : std::vector<int>{}) &&
                  server.Requests().size() == 1, "received status survives body-idle failure; a silent timeout is attempted once");
    }
}

void TestHttpErrors() {
    for (const int status : {401, 503}) {
        const std::string body = R"({"error":{"message":"fixture failure"}})";
        Server server([&](Server::Connection& peer, size_t) { peer.Send(Server::Headers(status, body.size()) + body); });
        auto request = Request(server);
        request.max_attempts = 3;
        int observed = 0;
        request.on_status = [&](int value) { observed = value; };
        const auto result = spark::HttpRequestStreaming(request);
        Check(result.ok && result.value.status == status && result.value.body == body && observed == status &&
                  result.value.received_bytes == body.size() && server.Requests().size() == 1,
              "HTTP error status/body is a transport success and does not trigger replay");
    }
}

void TestBoundedPrefix() {
    std::string body(kPrefixLimit + 65539, '\0');
    for (size_t i = 0; i < body.size(); ++i) body[i] = static_cast<char>(i % 251);
    for (const bool with_sink : {true, false}) {
        std::atomic<size_t> seen{0};
        Server server([&](Server::Connection& peer, size_t) {
            const size_t first = kPrefixLimit - 13;
            if (!peer.Send(Server::Headers(200, body.size())) || !peer.Send(std::string_view(body).substr(0, first))) return;
            // Deliver an unaligned prefix before crossing the cap with another read.
            const auto until = Clock::now() + 1s;
            while (with_sink && seen.load() < first && Clock::now() < until) if (!peer.Pause(5ms)) return;
            peer.Send(std::string_view(body).substr(first));
        });
        auto request = Request(server);
        std::string all;
        const spark::StreamCallback sink = with_sink ? spark::StreamCallback([&](const char* data, size_t size) {
            all.append(data, size);
            seen.fetch_add(size);
            return StreamAction::Continue;
        }) : spark::StreamCallback{};
        const auto result = spark::HttpRequestStreaming(request, sink);
        Check(result.ok, "a response larger than one MiB completes");
        if (result.ok) Check(result.value.body == body.substr(0, kPrefixLimit) &&
                                 result.value.received_bytes == body.size(),
                             "response accumulation is exactly the first one MiB, while byte counts include the entire body");
        if (with_sink) Check(all == body, "the callback receives the entire binary body beyond the retained prefix");
    }
}

void TestNoReplay() {
    {
        Server server([](Server::Connection& peer, size_t) { peer.Reset(); });
        const auto result = spark::HttpRequestStreaming(Request(server));
        Check(!result.ok && server.Requests().size() == 1, "max_attempts=1 never replays a request dropped before headers");
    }
    std::atomic<bool> delivered{false};
    Server server([&](Server::Connection& peer, size_t attempt) {
        if (attempt != 0) {
            peer.Send(Server::Headers(200, 6) + "replay");
            return;
        }
        if (!peer.Send(Server::Headers(200, 100) + "partial")) return;
        const auto until = Clock::now() + 1s;
        while (!delivered.load() && Clock::now() < until) if (!peer.Pause(5ms)) return;
        peer.Reset();
    });
    auto request = Request(server);
    request.stream = true;
    request.max_attempts = 3;
    std::string body;
    const auto result = spark::HttpRequestStreaming(request, [&](const char* data, size_t size) {
        body.append(data, size);
        delivered.store(body.size() >= 7);
        return StreamAction::Continue;
    });
    Check(delivered.load() && body == "partial" && server.Requests().size() == 1,
          "a transport reset after callback delivery never replays partial output");
    Check(!result.ok, "a reset with an incomplete declared body is a transport failure");
}

#ifndef _WIN32
void TestPosixValidationAndCallbackErrors() {
    Server server([](Server::Connection& peer, size_t) { peer.Send(Server::Headers(200, 2) + "ok"); });
    const auto rejected = [&](const spark::HttpRequest& request) {
        const auto result = spark::HttpRequestStreaming(request);
        Check(!result.ok && !result.error.empty(), "invalid URL, method or headers fails before transmission");
    };
    for (const auto& method : std::vector<std::string>{"", "BAD METHOD", "GET\r\nInjected: yes", std::string("GE\0T", 4)}) {
        auto request = Request(server);
        request.method = method;
        rejected(request);
    }
    for (const auto& header : std::vector<std::pair<std::string, std::string>>{
             {"Bad Name", "value"}, {"X-Test", "value\r\nInjected: yes"}, {"X-Test", std::string("a\0b", 3)}}) {
        auto request = Request(server);
        request.headers[header.first] = header.second;
        rejected(request);
    }
    for (const auto& url : {std::string("file://127.0.0.1/fixture"), server.Url() + std::string("\0ignored", 8)}) {
        auto request = Request(server);
        request.url = url;
        rejected(request);
    }
    Check(server.Requests().empty(), "invalid requests never reach the local fixture");
    for (const bool throw_status : {true, false}) {
        auto request = Request(server);
        request.max_attempts = 3;
        if (throw_status) request.on_status = [](int) { throw std::runtime_error("fixture status callback"); };
        const auto result = spark::HttpRequestStreaming(request, [&](const char*, size_t) -> StreamAction {
            if (!throw_status) throw std::runtime_error("fixture body callback");
            return StreamAction::Continue;
        });
        Check(!result.ok && result.error.find("callback") != std::string::npos,
              "POSIX callback exceptions become transport failures instead of crossing the C callback boundary");
    }
    Check(server.Requests().size() == 2, "callback exceptions are never retried");
}
#endif

}  // namespace

int main() {
#ifndef _WIN32
    // These standalone fixture requests must bypass inherited HTTP proxies.
    if (setenv("NO_PROXY", "*", 1) != 0 || setenv("no_proxy", "*", 1) != 0) {
        std::fputs("FAIL: could not isolate loopback requests from proxies\n", stderr);
        return 1;
    }
#endif
    SuiteDeadline deadline;
    try {
        TestRequestAndMetadata();
        TestFragmentedStream();
        TestStopAndCallbackCancel();
        TestTokenCancellation();
        TestTimeoutPhases();
        TestHttpErrors();
        TestBoundedPrefix();
        TestNoReplay();
#ifndef _WIN32
        TestPosixValidationAndCallbackErrors();
#endif
    } catch (const std::exception& error) {
        Check(false, error.what());
    }
    if (failures == 0) std::cout << "PASS: portable loopback HTTP contract\n";
    return failures == 0 ? 0 : 1;
}
