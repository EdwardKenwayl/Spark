#include "platform/clock.h"
#include "platform/http.h"

#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <mutex>
#include <sstream>
#include <vector>

namespace spark {
namespace {

struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

void ApplySystemProxy(HINTERNET session, HINTERNET request, const std::wstring& url) {
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ie_config{};
    if (!WinHttpGetIEProxyConfigForCurrentUser(&ie_config)) return;
    WINHTTP_AUTOPROXY_OPTIONS options{};
    if (ie_config.lpszAutoConfigUrl != nullptr) {
        options.dwFlags = WINHTTP_AUTOPROXY_CONFIG_URL;
        options.lpszAutoConfigUrl = ie_config.lpszAutoConfigUrl;
    } else if (ie_config.fAutoDetect) {
        options.dwFlags = WINHTTP_AUTOPROXY_AUTO_DETECT;
        options.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
    } else if (ie_config.lpszProxy != nullptr) {
        WINHTTP_PROXY_INFO info{};
        info.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        info.lpszProxy = ie_config.lpszProxy;
        info.lpszProxyBypass = ie_config.lpszProxyBypass;
        WinHttpSetOption(request, WINHTTP_OPTION_PROXY, &info, sizeof(info));
    }
    if (options.dwFlags != 0) {
        WINHTTP_PROXY_INFO info{};
        if (WinHttpGetProxyForUrl(session, url.c_str(), &options, &info)) {
            WinHttpSetOption(request, WINHTTP_OPTION_PROXY, &info, sizeof(info));
            if (info.lpszProxy != nullptr) GlobalFree(info.lpszProxy);
            if (info.lpszProxyBypass != nullptr) GlobalFree(info.lpszProxyBypass);
        }
    }
    if (ie_config.lpszAutoConfigUrl != nullptr) GlobalFree(ie_config.lpszAutoConfigUrl);
    if (ie_config.lpszProxy != nullptr) GlobalFree(ie_config.lpszProxy);
    if (ie_config.lpszProxyBypass != nullptr) GlobalFree(ie_config.lpszProxyBypass);
}

bool ParseUrl(std::string_view url, ParsedUrl& parsed, std::string* error) {
    const std::wstring wide = Utf8ToWide(url);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &components)) {
        if (error != nullptr) *error = "invalid URL: " + FormatWin32Error(GetLastError());
        return false;
    }
    if (components.nScheme != INTERNET_SCHEME_HTTP && components.nScheme != INTERNET_SCHEME_HTTPS) {
        if (error != nullptr) *error = "only http:// and https:// URLs are supported";
        return false;
    }
    parsed.host.assign(components.lpszHostName, components.dwHostNameLength);
    parsed.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) {
        parsed.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (parsed.path.empty()) parsed.path = L"/";
    parsed.port = components.nPort;
    parsed.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

std::wstring HeaderBlock(const std::map<std::string, std::string>& headers) {
    std::wstring output;
    for (const auto& [name, value] : headers) {
        output += Utf8ToWide(name + ": " + value + "\r\n");
    }
    return output;
}

bool ReadResponseHeaders(HINTERNET request, std::map<std::string, std::string>& output) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return false;
    std::wstring raw(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(),
                             &size, WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }
    raw.resize(wcslen(raw.c_str()));
    std::wstringstream stream(raw);
    std::wstring line;
    std::getline(stream, line);
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        const size_t separator = line.find(L':');
        if (separator == std::wstring::npos) continue;
        output[WideToUtf8(Trim(line.substr(0, separator)))] = WideToUtf8(Trim(line.substr(separator + 1)));
    }
    return true;
}

bool IsRetryableHttpError(DWORD code) {
    switch (code) {
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_CONNECTION_ERROR:
        case ERROR_WINHTTP_RESEND_REQUEST:
        case ERROR_WINHTTP_OPERATION_CANCELLED:
        case ERROR_WINHTTP_INCORRECT_HANDLE_STATE:
            return true;
        default:
            return false;
    }
}

enum class HttpCloseReason {
    None,
    Cancellation,
    Timeout,
    Manual,
};

class HttpRequestGuard {
public:
    HttpRequestGuard(HINTERNET handle, CancellationToken* cancellation, int timeout_ms)
        : handle_(handle), cancellation_(cancellation),
          deadline_(spark::MonotonicMillis() + static_cast<uint64_t>(timeout_ms)) {
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
        if (!WinHttpSetOption(handle_, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) {
            setup_error_ = GetLastError();
        } else if (WinHttpSetStatusCallback(handle_, Callback,
                       WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) ==
                   WINHTTP_INVALID_STATUS_CALLBACK) {
            setup_error_ = GetLastError();
        } else {
            callback_registered_ = true;
        }
    }
    ~HttpRequestGuard() { Close(); }
    HttpRequestGuard(const HttpRequestGuard&) = delete;
    HttpRequestGuard& operator=(const HttpRequestGuard&) = delete;

    HINTERNET Handle() const { return handle_; }
    DWORD BytesRead() const { return bytes_read_; }
    HttpCloseReason CloseReason() const { return close_reason_; }
    void SetDeadline(uint64_t deadline) { deadline_ = deadline; }

    template <typename Start>
    DWORD Run(DWORD expected, Start start) {
        if (setup_error_ != ERROR_SUCCESS) return setup_error_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            expected_ = expected; ready_ = false; error_ = ERROR_SUCCESS; bytes_read_ = 0;
        }
        if (cancellation_ != nullptr && cancellation_->IsCancelled()) {
            close_reason_ = HttpCloseReason::Cancellation;
            return ERROR_WINHTTP_OPERATION_CANCELLED;
        }
        if (!start()) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) return error;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            if (cancellation_ != nullptr && cancellation_->IsCancelled()) {
                close_reason_ = HttpCloseReason::Cancellation;
                return ERROR_WINHTTP_OPERATION_CANCELLED;
            }
            if (ready_) return error_;
            if (spark::MonotonicMillis() >= deadline_) {
                close_reason_ = HttpCloseReason::Timeout;
                return ERROR_WINHTTP_TIMEOUT;
            }
            condition_.wait_for(lock, std::chrono::milliseconds(25));
        }
    }

    void Close() {
        if (handle_ == nullptr) return;
        const BOOL closed = WinHttpCloseHandle(handle_);
        handle_ = nullptr;
        if (closed && callback_registered_) {
            // HANDLE_CLOSING is the final notification. Keep callback state and
            // read buffers alive until WinHTTP can no longer access either.
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [&] { return closed_; });
        }
    }

private:
    static void CALLBACK Callback(HINTERNET, DWORD_PTR context, DWORD status, void* info, DWORD size) {
        if (context == 0) return;
        auto* self = reinterpret_cast<HttpRequestGuard*>(context);
        std::lock_guard<std::mutex> lock(self->mutex_);
        if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) self->closed_ = true;
        else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
            self->error_ = static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError;
            self->ready_ = true;
        } else if (status == self->expected_) {
            self->bytes_read_ = status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE &&
                                       info != nullptr && size >= sizeof(DWORD)
                                   ? *static_cast<DWORD*>(info) : size;
            self->ready_ = true;
        } else return;
        self->condition_.notify_all();
    }
    HINTERNET handle_;
    CancellationToken* cancellation_;
    uint64_t deadline_;
    DWORD setup_error_ = ERROR_SUCCESS, expected_ = 0, error_ = ERROR_SUCCESS, bytes_read_ = 0;
    bool callback_registered_ = false, ready_ = false, closed_ = false;
    HttpCloseReason close_reason_ = HttpCloseReason::None;
    std::mutex mutex_;
    std::condition_variable condition_;
};

}  // namespace

namespace {

// Runs a single HTTP attempt. |retryable| is set only when the failure happened
// before any response byte reached the stream callback, so replaying the request
// cannot duplicate streamed output.
Result<HttpResponse> AttemptHttpRequest(const HttpRequest& request, const StreamCallback& on_chunk,
                                        CancellationToken* cancellation, bool* retryable) {
    *retryable = false;
    ParsedUrl url;
    std::string error;
    if (!ParseUrl(request.url, url, &error)) {
        return Result<HttpResponse>::Failure(std::move(error));
    }
    HINTERNET session = WinHttpOpen(L"Spark/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (session == nullptr) {
        const DWORD open_error = GetLastError();
        *retryable = IsRetryableHttpError(open_error);
        return Result<HttpResponse>::Failure("WinHttpOpen failed: " + FormatWin32Error(open_error));
    }
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    const int timeout_ms = std::max(1000, request.timeout_seconds * 1000);
    // Waiting for the first response byte is a separate phase from reading an
    // established stream. A slow endpoint that needs minutes of prefill must not
    // be reported as a stalled connection (WinHTTP error 12002), while an idle
    // stream still has to be dropped quickly. Both phases are polled in short
    // slices so cancellation stays responsive and the deadline is enforced by
    // this code instead of one unbounded blocking WinHTTP call.
    const int header_timeout_ms = std::max(timeout_ms, std::max(1000, request.header_timeout_seconds * 1000));
    WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, header_timeout_ms);

    HINTERNET connection = WinHttpConnect(session, url.host.c_str(), url.port, 0);
    if (connection == nullptr) {
        const DWORD connect_error = GetLastError();
        *retryable = IsRetryableHttpError(connect_error);
        WinHttpCloseHandle(session);
        return Result<HttpResponse>::Failure("WinHttpConnect failed: " + FormatWin32Error(connect_error));
    }
    const DWORD flags = url.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET http_request = WinHttpOpenRequest(connection, Utf8ToWide(request.method).c_str(), url.path.c_str(), nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (http_request == nullptr) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return Result<HttpResponse>::Failure("WinHttpOpenRequest failed: " + FormatWin32Error(GetLastError()));
    }
    ApplySystemProxy(session, http_request, Utf8ToWide(request.url));

    HttpRequestGuard active_request(http_request, cancellation, header_timeout_ms);
    const std::wstring headers = HeaderBlock(request.headers);
    const DWORD body_size = static_cast<DWORD>(request.body.size());
    const ULONGLONG header_started = spark::MonotonicMillis();
    const DWORD send_error = active_request.Run(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, [&] {
        return WinHttpSendRequest(http_request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
                            body_size == 0 ? nullptr : const_cast<char*>(request.body.data()), body_size, body_size,
                            reinterpret_cast<DWORD_PTR>(&active_request));
    });
    if (send_error != ERROR_SUCCESS) {
        const uint64_t header_elapsed = spark::MonotonicMillis() - header_started;
        *retryable = IsRetryableHttpError(send_error);
        const std::string failure = FormatWin32Error(send_error);
        const HttpCloseReason close_reason = active_request.CloseReason();
        active_request.Close();
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        if ((cancellation != nullptr && cancellation->IsCancelled()) ||
            close_reason == HttpCloseReason::Cancellation) {
            return Result<HttpResponse>::Failure("cancelled");
        }
        const bool deadline_reached =
            close_reason == HttpCloseReason::Timeout ||
            send_error == ERROR_WINHTTP_TIMEOUT ||
            (send_error == ERROR_WINHTTP_OPERATION_CANCELLED &&
             header_elapsed >= static_cast<ULONGLONG>(header_timeout_ms));
        if (deadline_reached) {
            const uint64_t waited_seconds = std::max<uint64_t>(1, header_elapsed / 1000);
            return Result<HttpResponse>::Failure(
                "等待模型开始响应超时（已等待 " + std::to_string(waited_seconds) +
                " 秒，WinHTTP 12002）：供应商没有返回任何数据，请检查网络连接或该供应商的服务状态。");
        }
        return Result<HttpResponse>::Failure("HTTP request failed: " + failure);
    }

    const DWORD header_error = active_request.Run(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, [&] {
        return WinHttpReceiveResponse(http_request, nullptr);
    });
    if (header_error != ERROR_SUCCESS) {
        const HttpCloseReason close_reason = active_request.CloseReason();
        const uint64_t header_elapsed = spark::MonotonicMillis() - header_started;
        active_request.Close();
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        if ((cancellation != nullptr && cancellation->IsCancelled()) ||
            close_reason == HttpCloseReason::Cancellation) {
            return Result<HttpResponse>::Failure("cancelled");
        }
        const bool deadline_reached =
            close_reason == HttpCloseReason::Timeout ||
            header_error == ERROR_WINHTTP_TIMEOUT ||
            header_elapsed >= static_cast<ULONGLONG>(header_timeout_ms);
        if (deadline_reached) {
            const uint64_t waited_seconds =
                std::max<uint64_t>(1, header_elapsed / 1000);
            // Retrying does not help a provider that is merely slow, so the
            // request stays non-retryable and the reason stays diagnosable.
            return Result<HttpResponse>::Failure(
                "等待模型开始响应超时（已等待 " + std::to_string(waited_seconds) +
                " 秒，WinHTTP 12002）：供应商没有返回任何数据，请检查网络连接或该供应商的服务状态。");
        }
        *retryable = IsRetryableHttpError(header_error);
        return Result<HttpResponse>::Failure("HTTP request failed: " + FormatWin32Error(header_error));
    }
    WinHttpSetTimeouts(http_request, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HttpResponse response;
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (WinHttpQueryHeaders(http_request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX)) {
        response.status = static_cast<int>(status);
    }
    if (request.on_status) request.on_status(response.status);
    ReadResponseHeaders(http_request, response.headers);
    response.header_ms = static_cast<long long>(spark::MonotonicMillis() - header_started);

    std::vector<char> buffer(64 * 1024);
    ULONGLONG last_activity = spark::MonotonicMillis();
    bool delivered = false;
    size_t received = 0;
    const StreamCallback sink = on_chunk ? StreamCallback([&](const char* data, size_t size) {
        delivered = true;
        return on_chunk(data, size);
    }) : StreamCallback{};
    for (;;) {
        if (cancellation != nullptr && cancellation->IsCancelled()) {
            active_request.Close();
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return Result<HttpResponse>::Failure("cancelled");
        }
        active_request.SetDeadline(last_activity + static_cast<uint64_t>(timeout_ms));
        // Read only currently available bytes. A fixed large ReadData request
        // tries to fill the buffer and can hide live SSE until EOF or timeout.
        DWORD read_error = active_request.Run(WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, [&] {
            return WinHttpQueryDataAvailable(http_request, nullptr);
        });
        if (read_error == ERROR_SUCCESS) {
            const DWORD available = active_request.BytesRead();
            const DWORD requested = std::max<DWORD>(1, std::min<DWORD>(
                available, static_cast<DWORD>(buffer.size())));
            read_error = active_request.Run(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, [&] {
                return WinHttpReadData(http_request, buffer.data(), requested, nullptr);
            });
        }
        if (read_error != ERROR_SUCCESS) {
            if (active_request.CloseReason() == HttpCloseReason::Cancellation) {
                active_request.Close();
                WinHttpCloseHandle(connection);
                WinHttpCloseHandle(session);
                return Result<HttpResponse>::Failure("cancelled");
            }
            *retryable = received == 0 && !delivered && IsRetryableHttpError(read_error);
            const std::string failure = FormatWin32Error(read_error);
            active_request.Close();
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return Result<HttpResponse>::Failure(read_error == ERROR_WINHTTP_TIMEOUT
                ? "HTTP receive timeout" : "HTTP read failed: " + failure);
        }
        const DWORD read = active_request.BytesRead();
        if (read == 0) break;
        last_activity = spark::MonotonicMillis();
        received += read;
        if (sink) {
            const StreamAction action = sink(buffer.data(), read);
            if (action == StreamAction::Cancel) {
                active_request.Close();
                WinHttpCloseHandle(connection);
                WinHttpCloseHandle(session);
                return Result<HttpResponse>::Failure("cancelled（已取消：流式响应被中断）");
            }
            if (action == StreamAction::Stop) {
                active_request.Close();
                WinHttpCloseHandle(connection);
                WinHttpCloseHandle(session);
                response.received_bytes = received;
                response.total_ms = static_cast<long long>(spark::MonotonicMillis() - header_started);
                return Result<HttpResponse>::Success(std::move(response));
            }
        }
        if (response.body.size() < 1024 * 1024) {
            response.body.append(buffer.data(), std::min<size_t>(read, 1024 * 1024 - response.body.size()));
        }
    }
    active_request.Close();
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    response.received_bytes = received;
    response.total_ms = static_cast<long long>(spark::MonotonicMillis() - header_started);
    return Result<HttpResponse>::Success(std::move(response));
}

}  // namespace

Result<HttpResponse> HttpRequestStreaming(const HttpRequest& request, const StreamCallback& on_chunk,
                                          CancellationToken* cancellation) {
    const int max_attempts = std::max(1, request.max_attempts);
    Result<HttpResponse> last = Result<HttpResponse>::Failure("HTTP request was not attempted");
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        if (cancellation != nullptr && cancellation->IsCancelled()) {
            return Result<HttpResponse>::Failure("cancelled");
        }
        if (attempt > 0) Sleep(attempt == 1 ? 150 : 450);
        bool retryable = false;
        Result<HttpResponse> result = AttemptHttpRequest(request, on_chunk, cancellation, &retryable);
        if (result.ok || !retryable) return result;
        last = std::move(result);
    }
    return last;
}

}  // namespace spark
