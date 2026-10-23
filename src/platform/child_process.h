#pragma once

#include "core/common.h"

#include <map>
#include <memory>

namespace spark {

enum class ProcessStream { Output, Error };
enum class ProcessIoState { Data, End, Timeout, Cancelled, Failed };

struct ProcessIo {
    ProcessIoState state = ProcessIoState::Failed;
    size_t size = 0;
    std::string error;
};

struct ProcessOptions {
    std::string executable;
    std::vector<std::string> arguments;
    std::string directory;
    std::map<std::string, std::string> environment;
    // CMD uses its own quote parser rather than the C runtime argv convention.
    std::string windows_command_line;
};

// Owns the child and its process group, including all three pipe endpoints.
// One reader per output stream and one writer are permitted concurrently.
// Stop/CloseInput must be called after those operations have returned.
class ChildProcess {
public:
    static Result<std::unique_ptr<ChildProcess>> Start(const ProcessOptions& options);
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    ProcessIo Read(ProcessStream stream, char* buffer, size_t capacity, int timeout_ms,
                   CancellationToken* cancellation = nullptr);
    Result<void> Write(std::string_view bytes, int timeout_ms, CancellationToken* cancellation = nullptr);
    void CloseInput();
    bool Exited(int* code = nullptr);
    void Stop();

private:
    struct State;
    explicit ChildProcess(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

}  // namespace spark
