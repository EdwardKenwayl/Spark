#pragma once

#include "core/common.h"

#include <functional>
#include <map>
#include <string>

namespace spark {

// Streaming callbacks tell the HTTP layer whether it should keep reading.
// Stop means "the logical payload is complete", so the connection can be
// closed without probing for more bytes.
enum class StreamAction {
    Continue,
    Stop,
    Cancel,
};

using StreamCallback = std::function<StreamAction(const char* data, size_t size)>;

struct HttpRequest {
    std::string method = "POST";
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    // Idle timeout for an established stream: how long the connection may stay
    // silent without delivering a single byte before the request is aborted.
    int timeout_seconds = 180;
    // How long a provider may take to start responding at all (waiting for the
    // status line and headers). Slow prefill on a large context, or a queued
    // endpoint, legitimately needs far more time than the stream idle timeout.
    int header_timeout_seconds = 600;
    // Non-idempotent requests can opt out of automatic replay.
    int max_attempts = 3;
    bool stream = false;
    // Report received headers even if reading the body later fails.
    std::function<void(int)> on_status;
};

struct HttpResponse {
    int status = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    // Timing and size facts, used to explain a stalled stream without guessing.
    long long header_ms = 0;
    long long total_ms = 0;
    size_t received_bytes = 0;
};

Result<HttpResponse> HttpRequestStreaming(const HttpRequest& request, const StreamCallback& on_chunk = {},
                                          CancellationToken* cancellation = nullptr);

}  // namespace spark
