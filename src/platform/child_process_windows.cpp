#ifdef _WIN32
#include "platform/child_process.h"
#include "platform/clock.h"

#include <algorithm>
#include <array>
#include <cwchar>
#include <mutex>
#include <thread>
#include <tlhelp32.h>

namespace spark {
namespace {
std::mutex spawn_mutex;
std::atomic<unsigned long> pipe_sequence{0};

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { Close(); }
    void Close() {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = nullptr;
    }
};

std::wstring Quote(std::wstring_view text) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const wchar_t character : text) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(slashes * (character == L'"' ? 2 : 1), L'\\');
        if (character == L'"') result += L'\\';
        result += character;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

struct EnvironmentLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};

std::vector<wchar_t> Environment(const std::map<std::string, std::string>& overrides) {
    std::map<std::wstring, std::wstring, EnvironmentLess> entries;
    LPWCH block = GetEnvironmentStringsW();
    if (block) {
        for (const wchar_t* entry = block; *entry; entry += std::wcslen(entry) + 1) {
            std::wstring text(entry);
            // Preserve "=C:" drive-relative working directories too.
            const size_t separator = text.find(L'=', text.front() == L'=' ? 1 : 0);
            if (separator != std::wstring::npos) entries[text.substr(0, separator)] = text.substr(separator + 1);
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& entry : overrides) entries[Utf8ToWide(entry.first)] = Utf8ToWide(entry.second);
    std::vector<wchar_t> output;
    for (const auto& entry : entries) {
        const std::wstring text = entry.first + L"=" + entry.second;
        output.insert(output.end(), text.begin(), text.end());
        output.push_back(0);
    }
    output.push_back(0);
    if (output.size() == 1) output.push_back(0);
    return output;
}

void TerminateDescendants(DWORD parent) {
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot.value == INVALID_HANDLE_VALUE) return;
    std::vector<PROCESSENTRY32W> entries;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.value, &entry)) {
        do { entries.push_back(entry); } while (Process32NextW(snapshot.value, &entry));
    }
    std::vector<DWORD> owned{parent};
    for (size_t index = 0; index < owned.size() && owned.size() < 4096; ++index) {
        for (const auto& item : entries) {
            if (item.th32ParentProcessID == owned[index] && item.th32ProcessID != parent &&
                std::find(owned.begin(), owned.end(), item.th32ProcessID) == owned.end())
                owned.push_back(item.th32ProcessID);
        }
    }
    for (auto item = owned.rbegin(); item != owned.rend(); ++item) {
        Handle process{OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, *item)};
        if (process.value) TerminateProcess(process.value, 1);
    }
}
}  // namespace

struct ChildProcess::State {
    Handle input, output, error, process, job;
    DWORD pid = 0;
    bool stopped = false;
};

ChildProcess::ChildProcess(std::unique_ptr<State> state) : state_(std::move(state)) {}
ChildProcess::~ChildProcess() { Stop(); }

Result<std::unique_ptr<ChildProcess>> ChildProcess::Start(const ProcessOptions& options) {
    using Outcome = Result<std::unique_ptr<ChildProcess>>;
    const auto valid = [](const std::string& text) { return text.find('\0') == std::string::npos; };
    if (options.executable.empty() || !valid(options.executable) || !valid(options.directory))
        return Outcome::Failure("invalid child executable or directory");
    for (const auto& argument : options.arguments)
        if (!valid(argument)) return Outcome::Failure("child argument contains NUL");
    for (const auto& entry : options.environment)
        if (entry.first.empty() || entry.first.find('=') != std::string::npos ||
            !valid(entry.first) || !valid(entry.second))
            return Outcome::Failure("invalid child environment override");
    std::lock_guard<std::mutex> lock(spawn_mutex);
    auto state = std::make_unique<State>();
    Handle child_input, child_output, child_error, thread;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const std::wstring pipe = L"\\\\.\\pipe\\Spark.Input." + std::to_wstring(GetCurrentProcessId()) +
                              L"." + std::to_wstring(pipe_sequence.fetch_add(1));
    state->input.value = CreateNamedPipeW(pipe.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED |
        FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 16384, 16384, 0, nullptr);
    if (state->input.value != INVALID_HANDLE_VALUE) {
        child_input.value = CreateFileW(pipe.c_str(), GENERIC_READ, 0, &security, OPEN_EXISTING, 0, nullptr);
    }
    if (state->input.value == INVALID_HANDLE_VALUE || !child_input.value ||
        child_input.value == INVALID_HANDLE_VALUE ||
        !CreatePipe(&state->output.value, &child_output.value, &security, 0) ||
        !SetHandleInformation(state->output.value, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&state->error.value, &child_error.value, &security, 0) ||
        !SetHandleInformation(state->error.value, HANDLE_FLAG_INHERIT, 0))
        return Outcome::Failure("create child pipes: " + FormatWin32Error(GetLastError()));
    std::wstring command = Quote(Utf8ToWide(options.executable));
    for (const auto& argument : options.arguments) command += L" " + Quote(Utf8ToWide(argument));
    if (!options.windows_command_line.empty()) command = Utf8ToWide(options.windows_command_line);
    std::vector<wchar_t> environment = Environment(options.environment);
    const std::wstring directory = Utf8ToWide(options.directory);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_input.value;
    startup.hStdOutput = child_output.value;
    startup.hStdError = child_error.value;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                        environment.data(), directory.empty() ? nullptr : directory.c_str(), &startup, &process))
        return Outcome::Failure("start child: " + FormatWin32Error(GetLastError()));
    state->process.value = process.hProcess;
    state->pid = process.dwProcessId;
    thread.value = process.hThread;
    state->job.value = CreateJobObjectW(nullptr, nullptr);
    if (state->job.value) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(state->job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(state->job.value, state->process.value)) state->job.Close();
    }
    auto owner = std::unique_ptr<ChildProcess>(new ChildProcess(std::move(state)));
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1))
        return Outcome::Failure("resume child: " + FormatWin32Error(GetLastError()));
    return Outcome::Success(std::move(owner));
}

ProcessIo ChildProcess::Read(ProcessStream stream, char* buffer, size_t capacity, int timeout_ms,
                             CancellationToken* cancellation) {
    const HANDLE pipe = stream == ProcessStream::Output ? state_->output.value : state_->error.value;
    const uint64_t deadline = MonotonicMillis() + static_cast<uint64_t>(std::max(0, timeout_ms));
    for (;;) {
        if (cancellation && cancellation->IsCancelled()) return {ProcessIoState::Cancelled};
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD code = GetLastError();
            if (code == ERROR_BROKEN_PIPE) return {ProcessIoState::End};
            return {ProcessIoState::Failed, 0, "read child pipe: " + FormatWin32Error(code)};
        }
        if (available > 0) {
            DWORD count = 0;
            if (!ReadFile(pipe, buffer, static_cast<DWORD>(std::min<size_t>(capacity, available)), &count, nullptr))
                return {ProcessIoState::Failed, 0, "read child pipe: " + FormatWin32Error(GetLastError())};
            return count ? ProcessIo{ProcessIoState::Data, count} : ProcessIo{ProcessIoState::End};
        }
        if (MonotonicMillis() >= deadline) return {ProcessIoState::Timeout};
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

Result<void> ChildProcess::Write(std::string_view bytes, int timeout_ms, CancellationToken* cancellation) {
    const uint64_t deadline = MonotonicMillis() + static_cast<uint64_t>(std::max(0, timeout_ms));
    if (!state_->input.value) return Result<void>::Failure("child stdin is closed");
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.value) return Result<void>::Failure("create child write event: " + FormatWin32Error(GetLastError()));
    size_t offset = 0;
    while (offset < bytes.size()) {
        if (cancellation && cancellation->IsCancelled()) return Result<void>::Failure("cancelled");
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.value;
        ResetEvent(event.value);
        DWORD written = 0;
        BOOL done = WriteFile(state_->input.value, bytes.data() + offset,
                              static_cast<DWORD>(std::min<size_t>(4096, bytes.size() - offset)), &written, &overlapped);
        if (!done && GetLastError() != ERROR_IO_PENDING)
            return Result<void>::Failure("write child pipe: " + FormatWin32Error(GetLastError()));
        if (!done) {
            while (WaitForSingleObject(event.value, 10) == WAIT_TIMEOUT) {
                if ((cancellation && cancellation->IsCancelled()) || MonotonicMillis() >= deadline) {
                    // CancelIo is available on XP and cancels this thread's operation.
                    CancelIo(state_->input.value);
                    GetOverlappedResult(state_->input.value, &overlapped, &written, TRUE);
                    return Result<void>::Failure(cancellation && cancellation->IsCancelled() ?
                                                  "cancelled" : "child write timed out");
                }
            }
            if (!GetOverlappedResult(state_->input.value, &overlapped, &written, FALSE))
                return Result<void>::Failure("write child pipe: " + FormatWin32Error(GetLastError()));
        }
        if (written == 0) return Result<void>::Failure("child write made no progress");
        offset += written;
        if (offset < bytes.size() && MonotonicMillis() >= deadline)
            return Result<void>::Failure("child write timed out");
    }
    return Result<void>::Success();
}

void ChildProcess::CloseInput() { state_->input.Close(); }

bool ChildProcess::Exited(int* code) {
    const bool exited = WaitForSingleObject(state_->process.value, 0) == WAIT_OBJECT_0;
    if (exited && code) {
        DWORD value = 0;
        GetExitCodeProcess(state_->process.value, &value);
        *code = static_cast<int>(value);
    }
    return exited;
}

void ChildProcess::Stop() {
    if (!state_ || state_->stopped) return;
    state_->stopped = true;
    CloseInput();
    if (state_->job.value) {
        TerminateJobObject(state_->job.value, 1);
    } else {
        TerminateDescendants(state_->pid);
        TerminateProcess(state_->process.value, 1);
    }
    WaitForSingleObject(state_->process.value, 1000);
}
}  // namespace spark
#endif
