#include "core/mcp.h"
#include "core/tool_process.h"
#include "platform/child_process.h"
#include "platform/clock.h"

#include <chrono>
#include <iostream>
#include <thread>

namespace {
int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << "\n"; }
}
std::string Collect(spark::ChildProcess& child, spark::ProcessStream stream) {
    std::string output;
    const uint64_t deadline = spark::MonotonicMillis() + 2000;
    while (spark::MonotonicMillis() < deadline) {
        char buffer[4096];
        const auto read = child.Read(stream, buffer, sizeof(buffer), 25);
        if (read.state == spark::ProcessIoState::Data) output.append(buffer, read.size);
        else if (read.state == spark::ProcessIoState::End) break;
        else if (read.state != spark::ProcessIoState::Timeout) { Check(false, "child stream read"); break; }
    }
    return output;
}
}  // namespace

int main(int argc, char** argv) {
    using namespace spark;
    if (argc != 2) return 2;
    const std::string helper = AbsolutePath(argv[1]);
    ProcessOptions options;
    options.executable = helper;
    options.directory = ParentPath(helper);
    options.arguments = {"--inspect", "", "quoted \"argument\"", "ends\\", "with space"};
    options.environment["SPARK_PROCESS_TEST_VALUE"] = "value with spaces";
    auto child = ChildProcess::Start(options);
    Check(child.ok, "child executable starts without a shell");
    if (child.ok) {
        child.value->CloseInput();
        const auto json = nlohmann::json::parse(Collect(*child.value, ProcessStream::Output), nullptr, false);
        Check(json.is_object(), "child reports a structured invocation");
        if (json.is_object()) {
            Check(json["args"] == std::vector<std::string>({"", "quoted \"argument\"", "ends\\", "with space"}),
                  "empty and quoted arguments are preserved");
            Check(json.value("environment", "") == "value with spaces", "environment overrides reach child");
            Check(NormalizePathForComparison(json.value("cwd", "")) == NormalizePathForComparison(options.directory),
                  "working directory reaches child");
        }
    }
    options.executable = JoinPath(ParentPath(helper), "spark-executable-that-does-not-exist");
    Check(!ChildProcess::Start(options).ok, "exec failure reported during startup");
    options.executable = helper;
    options.arguments = {"--silent"};
    child = ChildProcess::Start(options);
    Check(child.ok, "silent child starts");
    if (child.ok) {
        CancellationToken cancellation;
        std::thread cancel([&] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); cancellation.Cancel(); });
        char buffer[16];
        const uint64_t started = MonotonicMillis();
        const auto read = child.value->Read(ProcessStream::Output, buffer, sizeof(buffer), 5000, &cancellation);
        cancel.join();
        Check(read.state == ProcessIoState::Cancelled && MonotonicMillis() - started < 1000,
              "silent read is cancelled promptly");
        const uint64_t stop = MonotonicMillis();
        child.value->Stop();
        Check(MonotonicMillis() - stop < 1500, "child stop is bounded");
    }
    ToolContext context;
    context.workspace.path = ParentPath(helper);
    context.workspace.command_approval = "allow";
    context.limits.max_command_output_bytes = 4096;
    context.limits.max_tool_output_bytes = 16384;
    const std::string command = "\"" + helper + "\" --pipes";
    auto captured = ExecuteShellCommand(command, ParentPath(helper), 3, context);
    Check(captured.ok && captured.output.find("bytes omitted") != std::string::npos &&
          captured.output.find("[stderr]") != std::string::npos, "both large pipes drain with bounded retention");
    captured = ExecuteShellCommand(command + " 7", ParentPath(helper), 3, context);
    Check(!captured.ok && captured.output.find("exit code") != std::string::npos, "nonzero exit is a tool failure");
    McpServerConfig server;
    server.id = "fixture";
    server.enabled = true;
    server.command = helper;
    server.args = {"--wrong-ids"};
    server.cwd = ParentPath(helper);
    McpClient mcp(server, false);
    Check(mcp.EnsureStarted().ok, "stdio MCP initializes through the process adapter");
    const auto tools = mcp.ListTools();
    Check(tools.ok && tools.value.size() == 2, "stdio tools are discovered");
    const auto result = mcp.CallTool("echo", {{"message", "hello"}});
    Check(result.ok && result.value.find("hello|pid=") == 0, "stdio tool response is correlated");
    mcp.Stop();
    Check(!mcp.IsRunning(), "MCP stop clears lifecycle state");
    return failures ? 1 : 0;
}
