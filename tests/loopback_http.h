#pragma once

// Include before platform/http.h so Winsock 2 precedes windows.h.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace spark::test {

class LoopbackHttpServer {
#ifdef _WIN32
    using Socket = SOCKET;
    static constexpr Socket kInvalid = INVALID_SOCKET;
#else
    using Socket = int;
    static constexpr Socket kInvalid = -1;
#endif
    using Clock = std::chrono::steady_clock;
    using Milliseconds = std::chrono::milliseconds;

public:
    struct Request {
        std::string method;
        std::string target;
        std::map<std::string, std::string> headers;  // Lowercase names, trimmed values.
        std::string body;
    };

    class Connection {
    public:
        ~Connection() { Close(socket_); }
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;

        bool Send(std::string_view bytes) {
            const auto deadline = Clock::now() + Milliseconds(2000);
            for (size_t offset = 0; offset < bytes.size();) {
                if (!server_.Wait(socket_, true, deadline)) return false;
                int flags = 0;
#ifdef MSG_NOSIGNAL
                flags = MSG_NOSIGNAL;
#endif
                const auto count = send(socket_, bytes.data() + offset,
                    static_cast<int>(std::min<size_t>(16384, bytes.size() - offset)), flags);
                if (count > 0) offset += static_cast<size_t>(count);
                else if (count == 0 || !TransientError()) return false;
            }
            return true;
        }

        bool Pause(Milliseconds duration) {
            std::unique_lock<std::mutex> lock(server_.mutex_);
            return !server_.condition_.wait_for(lock, duration, [&] { return server_.stop_.load(); });
        }

        void Hold() { Pause(Milliseconds(4000)); }

        // Abort at handler exit, after any previously sent bytes were observed.
        void Reset() {
            linger option{};
            option.l_onoff = 1;
            option.l_linger = 0;
            setsockopt(socket_, SOL_SOCKET, SO_LINGER,
                       reinterpret_cast<const char*>(&option), sizeof(option));
        }

    private:
        friend class LoopbackHttpServer;
        Connection(LoopbackHttpServer& server, Socket socket) : server_(server), socket_(socket) {}
        LoopbackHttpServer& server_;
        Socket socket_;
    };

    using Handler = std::function<void(Connection&, size_t attempt)>;

    explicit LoopbackHttpServer(Handler handler) : handler_(std::move(handler)) {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
#endif
        try {
            listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listener_ == kInvalid || !Prepare(listener_)) throw std::runtime_error("fixture socket failed");
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#ifdef _WIN32
            int length = sizeof(address);
#else
            socklen_t length = sizeof(address);
#endif
            if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
                listen(listener_, 8) != 0 ||
                getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
                throw std::runtime_error("fixture bind/listen failed");
            }
            port_ = ntohs(address.sin_port);
            worker_ = std::thread([this] { Run(); });
        } catch (...) {
            Close(listener_);
#ifdef _WIN32
            WSACleanup();
#endif
            throw;
        }
    }

    ~LoopbackHttpServer() {
        stop_.store(true);
        condition_.notify_all();
        if (worker_.joinable()) worker_.join();
        Close(listener_);
#ifdef _WIN32
        WSACleanup();
#endif
    }
    LoopbackHttpServer(const LoopbackHttpServer&) = delete;
    LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

    std::string Url(std::string_view path = "/") const {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
    }

    static std::string Headers(int status, size_t length, std::string_view extra = {}) {
        return "HTTP/1.1 " + std::to_string(status) + " Fixture\r\nContent-Length: " +
            std::to_string(length) + "\r\nConnection: close\r\n" + std::string(extra) + "\r\n";
    }

    bool WaitForRequests(size_t count, Milliseconds timeout = Milliseconds(1500)) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait_for(lock, timeout, [&] { return requests_.size() >= count || !error_.empty(); });
        return requests_.size() >= count;
    }

    std::vector<Request> Requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    std::string Error() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }

private:
    static void Close(Socket socket) {
        if (socket == kInvalid) return;
#ifdef _WIN32
        closesocket(socket);
#else
        close(socket);
#endif
    }

    static bool Prepare(Socket socket) {
#ifdef _WIN32
        u_long enabled = 1;
        return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
        if (socket >= FD_SETSIZE) return false;
#ifdef SO_NOSIGPIPE
        const int enabled = 1;
        if (setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) return false;
#endif
        const int flags = fcntl(socket, F_GETFL, 0);
        return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    }

    static bool TransientError() {
#ifdef _WIN32
        const int error = WSAGetLastError();
        return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
    }

    bool Wait(Socket socket, bool writing, Clock::time_point deadline) {
        while (!stop_.load() && Clock::now() < deadline) {
            fd_set ready;
            FD_ZERO(&ready);
            FD_SET(socket, &ready);
            timeval slice{0, 25000};
#ifdef _WIN32
            const int nfds = 0;
#else
            const int nfds = socket + 1;
#endif
            const int count = select(nfds, writing ? nullptr : &ready, writing ? &ready : nullptr, nullptr, &slice);
            if (count > 0) return true;
            if (count < 0 && !TransientError()) return false;
        }
        return false;
    }

    static std::string Trim(std::string value) {
        const size_t first = value.find_first_not_of(" \t\r");
        return first == std::string::npos ? "" : value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
    }

    bool ReadRequest(Socket socket, Request& request) {
        constexpr size_t kMaxRequest = 128 * 1024;
        const auto deadline = Clock::now() + Milliseconds(1500);
        std::string bytes;
        size_t body_start = std::string::npos, body_size = 0;
        while (bytes.size() < kMaxRequest && Wait(socket, false, deadline)) {
            char buffer[4096];
            const auto count = recv(socket, buffer, sizeof(buffer), 0);
            if (count <= 0) {
                if (count < 0 && TransientError()) continue;
                return false;
            }
            bytes.append(buffer, static_cast<size_t>(count));
            const size_t end = bytes.find("\r\n\r\n");
            if (body_start == std::string::npos && end != std::string::npos) {
                body_start = end + 4;
                std::istringstream lines(bytes.substr(0, end));
                std::string line, version;
                if (!std::getline(lines, line)) return false;
                std::istringstream first(line);
                if (!(first >> request.method >> request.target >> version)) return false;
                while (std::getline(lines, line)) {
                    const size_t separator = line.find(':');
                    if (separator == std::string::npos) continue;
                    std::string name = Trim(line.substr(0, separator));
                    for (char& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                    request.headers[name] = Trim(line.substr(separator + 1));
                }
                const auto length = request.headers.find("content-length");
                if (length != request.headers.end()) {
                    const auto& text = length->second;
                    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), body_size);
                    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return false;
                }
                if (body_start > kMaxRequest || body_size > kMaxRequest - body_start) return false;
            }
            if (body_start != std::string::npos && bytes.size() >= body_start + body_size) {
                request.body = bytes.substr(body_start, body_size);
                return true;
            }
        }
        return false;
    }

    void Run() {
        try {
            while (!stop_.load()) {
                if (!Wait(listener_, false, Clock::now() + Milliseconds(25))) continue;
                const Socket socket = accept(listener_, nullptr, nullptr);
                if (socket == kInvalid) continue;
                Connection peer(*this, socket);
                if (!Prepare(socket)) throw std::runtime_error("fixture accepted socket failed");
                Request request;
                if (!ReadRequest(socket, request)) {
                    if (stop_.load()) return;
                    throw std::runtime_error("fixture did not receive a complete bounded request");
                }
                size_t attempt = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    attempt = requests_.size();
                    if (attempt >= 8) throw std::runtime_error("fixture exceeded eight requests");
                    requests_.push_back(std::move(request));
                }
                condition_.notify_all();
                handler_(peer, attempt);
            }
        } catch (const std::exception& error) {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = error.what();
            condition_.notify_all();
        }
    }

    Handler handler_;
    Socket listener_ = kInvalid;
    unsigned short port_ = 0;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<Request> requests_;
    std::string error_;
    std::thread worker_;
};

}  // namespace spark::test
