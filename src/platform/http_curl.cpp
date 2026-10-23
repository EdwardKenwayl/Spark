#include "platform/http.h"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>

namespace spark {
#ifdef SPARK_WINDOWS_XP
void InitializeXpTls();
#endif
namespace {

using Clock = std::chrono::steady_clock;
constexpr size_t kBodyPrefixLimit = 1024 * 1024;
constexpr size_t kHeaderLimit = 128 * 1024;

struct CurlRuntime {
    CurlRuntime() {
#ifdef SPARK_WINDOWS_XP
        InitializeXpTls();
#endif
        status = curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    CURLcode status;
    ~CurlRuntime() { if (status == CURLE_OK) curl_global_cleanup(); }
};

struct Transfer {
    const HttpRequest& request;
    const StreamCallback& sink;
    CancellationToken* cancellation;
    HttpResponse response;
    Clock::time_point started = Clock::now();
    Clock::time_point last_activity = started;
    size_t header_bytes = 0;
    bool headers_complete = false;
    bool stopped = false;
    bool cancelled = false;
    std::string error;

    bool IsCancelled() const { return cancellation != nullptr && cancellation->IsCancelled(); }
    long long Elapsed() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    }

    static size_t Header(char* data, size_t width, size_t count, void* context) noexcept {
        auto& self = *static_cast<Transfer*>(context);
        const size_t size = width * count;
        try {
            if (size > kHeaderLimit - self.header_bytes) {
                self.error = "HTTP response headers exceed 128 KiB";
                return 0;
            }
            self.header_bytes += size;
            const std::string_view line(data, size);
            if (line.substr(0, 5) == "HTTP/") {
                self.response.headers.clear();
                const size_t space = line.find(' ');
                self.response.status = space == std::string_view::npos ? 0 :
                    std::stoi(std::string(line.substr(space + 1, 3)));
            } else if (line == "\r\n" && self.response.status >= 200 && !self.headers_complete) {
                self.headers_complete = true;
                self.last_activity = Clock::now();
                self.response.header_ms = self.Elapsed();
                if (self.request.on_status) self.request.on_status(self.response.status);
            } else if (!self.headers_complete) {
                const size_t separator = line.find(':');
                if (separator != std::string_view::npos) {
                    self.response.headers[std::string(line.substr(0, separator))] =
                        Trim(std::string(line.substr(separator + 1)));
                }
            }
            return size;
        } catch (...) {
            self.error = "HTTP response header callback failed";
            return 0;
        }
    }

    static size_t Body(char* data, size_t width, size_t count, void* context) noexcept {
        auto& self = *static_cast<Transfer*>(context);
        const size_t size = width * count;
        try {
            if (self.IsCancelled()) {
                self.cancelled = true;
                return 0;
            }
            self.last_activity = Clock::now();
            self.response.received_bytes += size;
            if (self.sink) {
                const auto action = self.sink(data, size);
                if (action == StreamAction::Cancel) {
                    self.cancelled = true;
                    return 0;
                }
                if (action == StreamAction::Stop) {
                    self.stopped = true;
                    return 0;
                }
            }
            self.response.body.append(data, std::min(size, kBodyPrefixLimit - self.response.body.size()));
            return size;
        } catch (...) {
            self.error = "HTTP response body callback failed";
            return 0;
        }
    }
};

bool ValidHeaders(const HttpRequest& request) {
    const auto token = [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || std::string_view("!#$%&'*+-.^_`|~").find(ch) !=
                   std::string_view::npos;
    };
    if (request.method.empty() || !std::all_of(request.method.begin(), request.method.end(), token)) return false;
    for (const auto& [name, value] : request.headers) {
        if (name.empty() || !std::all_of(name.begin(), name.end(), token)) return false;
        for (unsigned char ch : value) if ((ch < 32 && ch != '\t') || ch == 127) return false;
    }
    return true;
}

Result<HttpResponse> Attempt(const HttpRequest& request, const StreamCallback& sink,
                             CancellationToken* cancellation, bool& retryable) {
    retryable = false;
    Transfer transfer{request, sink, cancellation};
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy(curl_easy_init(), curl_easy_cleanup);
    std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi(curl_multi_init(), curl_multi_cleanup);
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr, curl_slist_free_all);
    if (!easy || !multi) return Result<HttpResponse>::Failure("HTTP initialization failed");
    const auto append_header = [&](const std::string& line) {
        auto* next = curl_slist_append(headers.get(), line.c_str());
        if (next == nullptr) return false;
        headers.release();
        headers.reset(next);
        return true;
    };
    // Avoid the implicit 100-continue wait on a large model request.
    if (!append_header("Expect:")) return Result<HttpResponse>::Failure("HTTP header allocation failed");
    for (const auto& [name, value] : request.headers) {
        if (!append_header(value.empty() ? name + ";" : name + ": " + value)) {
            return Result<HttpResponse>::Failure("HTTP header allocation failed");
        }
    }
    bool configured = true;
    const auto option = [&](CURLoption name, auto value) {
        if (curl_easy_setopt(easy.get(), name, value) != CURLE_OK) configured = false;
    };
    option(CURLOPT_URL, request.url.c_str());
    option(CURLOPT_CUSTOMREQUEST, request.method.c_str());
    option(CURLOPT_HTTPHEADER, headers.get());
    option(CURLOPT_USERAGENT, "Spark/0.1");
    option(CURLOPT_NOSIGNAL, 1L);
    option(CURLOPT_CONNECTTIMEOUT, static_cast<long>(std::max(1, request.timeout_seconds)));
    option(CURLOPT_SSL_VERIFYPEER, 1L);
    option(CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef SPARK_WINDOWS_XP
    const std::string certificates = JoinPath(GetExecutableDirectory(), "cacert.pem");
    if (!PathExists(certificates))
        return Result<HttpResponse>::Failure("The XP package is missing cacert.pem.");
    option(CURLOPT_CAINFO, certificates.c_str());
#endif
    option(CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
    // API credentials must never be forwarded to a redirect destination.
    option(CURLOPT_FOLLOWLOCATION, 0L);
    option(CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
    option(CURLOPT_PROTOCOLS_STR, "http,https");
#else
    option(CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    option(CURLOPT_HEADERFUNCTION, &Transfer::Header);
    option(CURLOPT_HEADERDATA, &transfer);
    option(CURLOPT_WRITEFUNCTION, &Transfer::Body);
    option(CURLOPT_WRITEDATA, &transfer);
    if (!request.body.empty() || request.method == "POST") {
        option(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
        option(CURLOPT_POSTFIELDS, request.body.data());
    }
    if (!configured) return Result<HttpResponse>::Failure("HTTP option setup failed");
    if (curl_multi_add_handle(multi.get(), easy.get()) != CURLM_OK) {
        return Result<HttpResponse>::Failure("HTTP transfer setup failed");
    }
    const auto idle_timeout = std::chrono::seconds(std::max(1, request.timeout_seconds));
    const auto header_timeout = std::max(idle_timeout, std::chrono::seconds(
        std::max(1, request.header_timeout_seconds)));
    CURLcode result = CURLE_OK;
    for (;;) {
        if (transfer.IsCancelled()) { transfer.cancelled = true; break; }
        int running = 0;
        const auto performed = curl_multi_perform(multi.get(), &running);
        if (performed != CURLM_OK) { transfer.error = "HTTP transfer failed"; break; }
        if (transfer.stopped || transfer.cancelled || !transfer.error.empty()) break;
        if (!running) {
            int pending = 0;
            CURLMsg* message = curl_multi_info_read(multi.get(), &pending);
            if (message == nullptr || message->msg != CURLMSG_DONE) transfer.error = "HTTP transfer ended unexpectedly";
            else result = message->data.result;
            break;
        }
        const auto now = Clock::now();
        if (!transfer.headers_complete && now - transfer.started >= header_timeout) {
            transfer.error = "HTTP response header timeout";
            break;
        }
        if (transfer.headers_complete && now - transfer.last_activity >= idle_timeout) {
            transfer.error = "HTTP receive timeout";
            break;
        }
        int ready = 0;
        if (curl_multi_poll(multi.get(), nullptr, 0, 25, &ready) != CURLM_OK) {
            transfer.error = "HTTP poll failed";
            break;
        }
    }
    curl_multi_remove_handle(multi.get(), easy.get());
    if (transfer.cancelled || transfer.IsCancelled()) return Result<HttpResponse>::Failure("cancelled");
    if (!transfer.error.empty()) return Result<HttpResponse>::Failure(std::move(transfer.error));
    if (result != CURLE_OK && !transfer.stopped) {
        // Once any response starts, replay could duplicate a model turn.
        retryable = transfer.header_bytes == 0 && transfer.response.received_bytes == 0 &&
            (result == CURLE_COULDNT_CONNECT || result == CURLE_SEND_ERROR ||
             result == CURLE_RECV_ERROR || result == CURLE_GOT_NOTHING);
        return Result<HttpResponse>::Failure(std::string("HTTP request failed: ") + curl_easy_strerror(result));
    }
    transfer.response.total_ms = transfer.Elapsed();
    return Result<HttpResponse>::Success(std::move(transfer.response));
}

}  // namespace

Result<HttpResponse> HttpRequestStreaming(const HttpRequest& request, const StreamCallback& on_chunk,
                                          CancellationToken* cancellation) {
    if (cancellation != nullptr && cancellation->IsCancelled()) return Result<HttpResponse>::Failure("cancelled");
    if ((!EqualsIgnoreCaseAscii(request.url.substr(0, 7), "http://") &&
         !EqualsIgnoreCaseAscii(request.url.substr(0, 8), "https://")) ||
        request.url.find('\0') != std::string::npos || !ValidHeaders(request)) {
        return Result<HttpResponse>::Failure("Invalid HTTP URL, method or headers");
    }
    static CurlRuntime runtime;
    if (runtime.status != CURLE_OK) return Result<HttpResponse>::Failure("HTTP runtime initialization failed");
    if ((curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_ASYNCHDNS) == 0) {
        return Result<HttpResponse>::Failure("libcurl requires asynchronous DNS for cancellable requests");
    }
    Result<HttpResponse> last = Result<HttpResponse>::Failure("HTTP request was not attempted");
    for (int attempt = 0; attempt < std::max(1, request.max_attempts); ++attempt) {
        if (attempt > 0) {
            const auto until = Clock::now() + std::chrono::milliseconds(attempt == 1 ? 150 : 450);
            while (Clock::now() < until) {
                if (cancellation != nullptr && cancellation->IsCancelled()) return Result<HttpResponse>::Failure("cancelled");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        if (cancellation != nullptr && cancellation->IsCancelled()) return Result<HttpResponse>::Failure("cancelled");
        bool retryable = false;
        auto result = Attempt(request, on_chunk, cancellation, retryable);
        if (result.ok || !retryable) return result;
        last = std::move(result);
    }
    return last;
}

}  // namespace spark
