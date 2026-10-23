#include "core/tool_process.h"

#include "platform/child_process.h"
#include "platform/clock.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>

namespace spark {
namespace {
std::atomic<uint64_t> previous_start{0};

class Capture {
public:
    explicit Capture(size_t limit) : head_limit_(limit * 3 / 4), tail_(limit - head_limit_) {
        head_.reserve(head_limit_);
    }
    void Append(const char* bytes, size_t size) {
        total_ += size;
        const size_t head = std::min(size, head_limit_ - head_.size());
        head_.append(bytes, head);
        for (size_t index = head; index < size; ++index) {
            tail_[position_] = bytes[index];
            position_ = (position_ + 1) % tail_.size();
            count_ = std::min(count_ + 1, tail_.size());
        }
    }
    std::string Text() const {
        std::string output = head_;
        const size_t limit = head_limit_ + tail_.size();
        if (total_ > limit) {
            output += "\n[output truncated by Spark limit: " + std::to_string(total_ - limit) +
                      " bytes omitted between head and tail]\n";
        }
        const size_t start = count_ == tail_.size() ? position_ : 0;
        const size_t first = std::min(count_, tail_.size() - start);
        output.append(tail_.data() + start, first);
        output.append(tail_.data(), count_ - first);
#ifdef _WIN32
        return DecodeBytesToUtf8(output, GetOEMCP());
#else
        return DecodeBytesToUtf8(output, 65001);
#endif
    }
private:
    size_t head_limit_, position_ = 0, count_ = 0;
    uint64_t total_ = 0;
    std::string head_;
    std::vector<char> tail_;
};
}  // namespace

ToolExecutionResult ExecuteShellCommand(const std::string& command, const std::string& directory,
                                        int timeout_seconds, const ToolContext& context) {
    ToolExecutionResult result;
    for (;;) {
        if (context.cancellation && context.cancellation->IsCancelled())
            return {false, false, "ERROR: cancelled"};
        uint64_t previous = previous_start.load();
        const uint64_t now = MonotonicMillis();
        if ((previous == 0 || now - previous >= 150) && previous_start.compare_exchange_weak(previous, now)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ProcessOptions options;
    options.directory = directory;
#ifdef _WIN32
    options.executable = context.shell.empty() ? "cmd.exe" : context.shell;
    const std::string shell = LowerAscii(options.executable);
    if (shell.find("powershell") != std::string::npos || shell.find("pwsh") != std::string::npos) {
        options.arguments = {"-NoLogo", "-NoProfile", "-NonInteractive", "-Command", command};
    } else {
        options.windows_command_line = "\"" + options.executable + "\" /D /S /C \"" + command + "\"";
    }
#else
    options.executable = context.shell.empty() ? "/bin/sh" : context.shell;
    options.arguments = {"-c", command};
#endif
    auto child = ChildProcess::Start(options);
    if (!child.ok) return {false, false, ToolError(child.error)};
    result.side_effecting = true;
    child.value->CloseInput();
    const size_t limit = static_cast<size_t>(std::max(4096, context.limits.max_command_output_bytes));
    Capture output(limit), error(limit);
    bool output_end = false, error_end = false, stopped = false;
    int exit_code = -1;
    std::string failure;
    const uint64_t deadline = MonotonicMillis() + static_cast<uint64_t>(std::max(1, timeout_seconds)) * 1000;
    uint64_t drain_deadline = 0;
    for (;;) {
        char buffer[8192];
        bool data = false;
        for (const ProcessStream stream : {ProcessStream::Output, ProcessStream::Error}) {
            bool& end = stream == ProcessStream::Output ? output_end : error_end;
            if (end) continue;
            const auto read = child.value->Read(stream, buffer, sizeof(buffer), 0);
            if (read.state == ProcessIoState::Data) {
                (stream == ProcessStream::Output ? output : error).Append(buffer, read.size);
                data = true;
            } else if (read.state == ProcessIoState::End || read.state == ProcessIoState::Failed) {
                end = true;
                if (read.state == ProcessIoState::Failed && failure.empty()) failure = read.error;
            }
        }
        if (!stopped) {
            if (context.cancellation && context.cancellation->IsCancelled()) failure = "cancelled";
            else if (MonotonicMillis() >= deadline) failure = "command timed out";
            const bool exited = child.value->Exited(&exit_code);
            if (!failure.empty() || exited) {
                child.value->Stop();
                stopped = true;
                drain_deadline = MonotonicMillis() + 1000;
            }
        }
        if (stopped && ((output_end && error_end) || MonotonicMillis() >= drain_deadline)) break;
        if (!data) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    result.ok = failure.empty() && exit_code == 0;
    result.output = "$ " + command + "\n" + output.Text();
    const std::string diagnostics = error.Text();
    if (!diagnostics.empty()) result.output += "\n[stderr]\n" + diagnostics;
    if (!failure.empty()) result.output += "\n[error] " + failure;
    else if (exit_code != 0) result.output += "\n[exit code] " + std::to_string(exit_code);
    const size_t max_output = static_cast<size_t>(std::max(1024, context.limits.max_tool_output_bytes));
    if (result.output.size() > max_output)
        result.output = TruncateUtf8(std::move(result.output), max_output) + "\n[output truncated by Spark limit]";
    return result;
}
}  // namespace spark
