#ifndef _WIN32
#include "platform/child_process.h"
#include "platform/clock.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace spark {
namespace {
std::mutex spawn_mutex;

struct Descriptor {
    int value = -1;
    ~Descriptor() { Close(); }
    void Close() { if (value >= 0) ::close(std::exchange(value, -1)); }
};

bool MakePipe(Descriptor& read, Descriptor& write) {
    int descriptors[2];
    if (::pipe(descriptors) != 0) return false;
    read.value = descriptors[0];
    write.value = descriptors[1];
    for (Descriptor* endpoint : {&read, &write}) {
        if (endpoint->value < 3) {
            const int duplicate = fcntl(endpoint->value, F_DUPFD_CLOEXEC, 3);
            if (duplicate < 0) return false;
            ::close(endpoint->value);
            endpoint->value = duplicate;
        }
    }
    return fcntl(read.value, F_SETFD, FD_CLOEXEC) == 0 &&
           fcntl(write.value, F_SETFD, FD_CLOEXEC) == 0;
}

std::string ProcessError(std::string_view action, int code = errno) {
    return std::string(action) + ": " + std::strerror(code);
}

int Remaining(uint64_t deadline) {
    const uint64_t now = MonotonicMillis();
    return now >= deadline ? 0 : static_cast<int>(std::min<uint64_t>(25, deadline - now));
}

bool ValidString(const std::string& value) {
    return value.find('\0') == std::string::npos;
}

// Blocking SIGPIPE only in this thread avoids changing the embedding application's
// signal policy. Consume a newly generated signal before restoring the old mask.
ssize_t WriteWithoutSignal(int descriptor, const char* data, size_t count) {
    sigset_t set, old, pending;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    const int masked = pthread_sigmask(SIG_BLOCK, &set, &old);
    if (masked != 0) { errno = masked; return -1; }
    sigpending(&pending);
    const bool already_pending = sigismember(&pending, SIGPIPE) == 1;
    const ssize_t result = ::write(descriptor, data, count);
    const int saved = errno;
    if (result < 0 && saved == EPIPE && !already_pending) {
        sigpending(&pending);
        if (sigismember(&pending, SIGPIPE) == 1) {
            int signal = 0;
            sigwait(&set, &signal);
        }
    }
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    errno = saved;
    return result;
}
}  // namespace

struct ChildProcess::State {
    pid_t pid = -1;
    Descriptor input, output, error;
    bool reaped = false;
    bool exited = false;
    int code = -1;
};

ChildProcess::ChildProcess(std::unique_ptr<State> state) : state_(std::move(state)) {}
ChildProcess::~ChildProcess() { Stop(); }

Result<std::unique_ptr<ChildProcess>> ChildProcess::Start(const ProcessOptions& options) {
    using Outcome = Result<std::unique_ptr<ChildProcess>>;
    if (options.executable.empty() || !ValidString(options.executable) || !ValidString(options.directory))
        return Outcome::Failure("invalid child executable or directory");
    std::vector<std::string> arguments{options.executable};
    for (const auto& argument : options.arguments) {
        if (!ValidString(argument)) return Outcome::Failure("child argument contains NUL");
        arguments.push_back(argument);
    }
    std::map<std::string, std::string> environment;
    for (char** entry = environ; entry && *entry; ++entry) {
        const std::string value(*entry);
        const size_t separator = value.find('=');
        if (separator != std::string::npos) environment[value.substr(0, separator)] = value.substr(separator + 1);
    }
    for (const auto& entry : options.environment) {
        if (entry.first.empty() || entry.first.find('=') != std::string::npos ||
            !ValidString(entry.first) || !ValidString(entry.second))
            return Outcome::Failure("invalid child environment override");
        environment[entry.first] = entry.second;
    }
    const std::string directory = options.directory.empty() ? CurrentDirectoryUtf8() : AbsolutePath(options.directory);
    std::string executable = options.executable;
    if (executable.find('/') == std::string::npos) {
        const auto path = environment.find("PATH");
        const std::string search = path == environment.end() ? "/usr/bin:/bin" : path->second;
        size_t start = 0;
        executable.clear();
        do {
            const size_t end = search.find(':', start);
            const std::string component = search.substr(start, end == std::string::npos ? end : end - start);
            const std::string candidate = JoinPath(AbsolutePath(component.empty() ? "." : component, directory),
                                                   options.executable);
            if (::access(candidate.c_str(), X_OK) == 0) { executable = candidate; break; }
            if (end == std::string::npos) break;
            start = end + 1;
        } while (start <= search.size());
        if (executable.empty()) return Outcome::Failure("child executable not found in PATH: " + options.executable);
    } else {
        executable = AbsolutePath(executable, directory);
    }
    std::vector<std::string> entries;
    for (const auto& entry : environment) entries.push_back(entry.first + "=" + entry.second);
    std::vector<char*> argv, envp;
    for (auto& argument : arguments) argv.push_back(argument.data());
    for (auto& entry : entries) envp.push_back(entry.data());
    argv.push_back(nullptr);
    envp.push_back(nullptr);

    // No allocations, locks or C++ runtime calls are used in the post-fork child.
    std::lock_guard<std::mutex> lock(spawn_mutex);
    auto state = std::make_unique<State>();
    Descriptor child_input, child_output, child_error, exec_read, exec_write;
    if (!MakePipe(child_input, state->input) || !MakePipe(state->output, child_output) ||
        !MakePipe(state->error, child_error) || !MakePipe(exec_read, exec_write))
        return Outcome::Failure(ProcessError("create child pipes"));
    const char* program = executable.c_str();
    const char* cwd = directory.c_str();
    char* const* args = argv.data();
    char* const* env = envp.data();
    state->pid = ::fork();
    if (state->pid < 0) return Outcome::Failure(ProcessError("fork child"));
    if (state->pid == 0) {
        int failure = 0;
        if (::setpgid(0, 0) != 0 || ::chdir(cwd) != 0 ||
            ::dup2(child_input.value, STDIN_FILENO) < 0 ||
            ::dup2(child_output.value, STDOUT_FILENO) < 0 ||
            ::dup2(child_error.value, STDERR_FILENO) < 0) {
            failure = errno;
        } else {
            ::execve(program, args, env);
            failure = errno;
        }
        const auto ignored = ::write(exec_write.value, &failure, sizeof(failure));
        (void)ignored;
        ::_exit(127);
    }
    ::setpgid(state->pid, state->pid);
    child_input.Close();
    child_output.Close();
    child_error.Close();
    exec_write.Close();
    int failure = 0;
    ssize_t count;
    do { count = ::read(exec_read.value, &failure, sizeof(failure)); } while (count < 0 && errno == EINTR);
    auto process = std::unique_ptr<ChildProcess>(new ChildProcess(std::move(state)));
    if (count != 0) return Outcome::Failure(ProcessError("execute child", count > 0 ? failure : errno));
    for (const int descriptor : {process->state_->input.value, process->state_->output.value,
                                 process->state_->error.value}) {
        const int flags = fcntl(descriptor, F_GETFL);
        if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0)
            return Outcome::Failure(ProcessError("set child pipe nonblocking"));
    }
    return Outcome::Success(std::move(process));
}

ProcessIo ChildProcess::Read(ProcessStream stream, char* buffer, size_t capacity, int timeout_ms,
                             CancellationToken* cancellation) {
    const int descriptor = stream == ProcessStream::Output ? state_->output.value : state_->error.value;
    if (descriptor < 0) return {ProcessIoState::End};
    const uint64_t deadline = MonotonicMillis() + static_cast<uint64_t>(std::max(0, timeout_ms));
    for (;;) {
        if (cancellation && cancellation->IsCancelled()) return {ProcessIoState::Cancelled};
        const ssize_t count = ::read(descriptor, buffer, capacity);
        if (count > 0) return {ProcessIoState::Data, static_cast<size_t>(count)};
        if (count == 0) return {ProcessIoState::End};
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return {ProcessIoState::Failed, 0, ProcessError("read child pipe")};
        if (MonotonicMillis() >= deadline) return {ProcessIoState::Timeout};
        pollfd item{descriptor, POLLIN, 0};
        if (::poll(&item, 1, Remaining(deadline)) < 0 && errno != EINTR)
            return {ProcessIoState::Failed, 0, ProcessError("poll child pipe")};
    }
}

Result<void> ChildProcess::Write(std::string_view bytes, int timeout_ms, CancellationToken* cancellation) {
    const uint64_t deadline = MonotonicMillis() + static_cast<uint64_t>(std::max(0, timeout_ms));
    size_t offset = 0;
    while (offset < bytes.size()) {
        if (cancellation && cancellation->IsCancelled()) return Result<void>::Failure("cancelled");
        if (state_->input.value < 0) return Result<void>::Failure("child stdin is closed");
        const ssize_t count = WriteWithoutSignal(state_->input.value, bytes.data() + offset,
                                                 std::min<size_t>(4096, bytes.size() - offset));
        if (count > 0) { offset += static_cast<size_t>(count); continue; }
        if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return Result<void>::Failure(ProcessError("write child pipe"));
        if (MonotonicMillis() >= deadline) return Result<void>::Failure("child write timed out");
        pollfd item{state_->input.value, POLLOUT, 0};
        if (::poll(&item, 1, Remaining(deadline)) < 0 && errno != EINTR)
            return Result<void>::Failure(ProcessError("poll child input"));
    }
    return Result<void>::Success();
}

void ChildProcess::CloseInput() { state_->input.Close(); }

bool ChildProcess::Exited(int* code) {
    if (!state_->exited && !state_->reaped) {
        siginfo_t info{};
        const int result = ::waitid(P_PID, static_cast<id_t>(state_->pid), &info, WEXITED | WNOHANG | WNOWAIT);
        if (result == 0 && info.si_pid == state_->pid) {
            // Retain the zombie until Stop so the owned process-group ID cannot be recycled.
            state_->exited = true;
            state_->code = info.si_code == CLD_EXITED ? info.si_status : 128 + info.si_status;
        } else if (result < 0 && errno == ECHILD) {
            state_->exited = state_->reaped = true;
            state_->code = -1;
        }
    }
    if (code) *code = state_->code;
    return state_->exited || state_->reaped;
}

void ChildProcess::Stop() {
    if (!state_ || state_->pid < 0) return;
    CloseInput();
    // Kill the group even after the direct child exits: grandchildren may hold pipes.
    if (!state_->reaped) ::kill(-state_->pid, SIGKILL);
    if (!state_->reaped) {
        ::kill(state_->pid, SIGKILL);
        int status;
        while (::waitpid(state_->pid, &status, 0) < 0 && errno == EINTR) {}
        state_->reaped = true;
    }
    state_->pid = -1;
}
}  // namespace spark
#endif
