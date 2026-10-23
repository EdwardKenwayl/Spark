#include "core/tools.h"
#include "core/tool_process.h"
#include "core/tool_scan.h"

#include <algorithm>
#include "platform/filesystem.h"
#include <set>
#include <sstream>

namespace spark {
namespace {

std::string StringArgument(const nlohmann::json& arguments, const char* name, const std::string& fallback = {}) {
    const auto value = arguments.find(name);
    return value != arguments.end() && value->is_string() ? value->get<std::string>() : fallback;
}

int IntArgument(const nlohmann::json& arguments, const char* name, int fallback) {
    const auto value = arguments.find(name);
    if (value == arguments.end() || !value->is_number_integer()) return fallback;
    const auto number = value->get<int64_t>();
    return static_cast<int>(std::clamp<int64_t>(number, -2147483647, 2147483647));
}

std::string BoundedOutput(std::string value, int bytes) {
    if (bytes > 0 && value.size() > static_cast<size_t>(bytes))
        return TruncateUtf8(std::move(value), static_cast<size_t>(bytes)) + "\n[output truncated by Spark limit]";
    return value;
}

const std::set<std::string> image_extensions{".bmp", ".gif", ".jpeg", ".jpg", ".png", ".webp"};

}  // namespace

ToolRegistry::ToolRegistry() {
    auto add = [this](std::string name, std::string description, nlohmann::json parameters) {
        definitions_.push_back({std::move(name), std::move(description), std::move(parameters)});
    };
    add("list_files", "List files under the workspace with depth, time, entry and reparse-point limits.",
        {{"type", "object"}, {"properties", {
            {"path", {{"type", "string"}, {"description", "Directory relative to the workspace."}}},
            {"max_depth", {{"type", "integer"}, {"description", "Maximum directory depth, 0-32."}}},
            {"max_entries", {{"type", "integer"}, {"description", "Maximum scanned entries, up to 100000."}}},
            {"time_budget_ms", {{"type", "integer"}, {"description", "Scan time budget in milliseconds, up to 30000."}}}}}});
    add("read_file", "Read a UTF-8 text file from the workspace, optionally by line range.",
        {{"type", "object"},
         {"properties", {{"path", {{"type", "string"}}}, {"start_line", {{"type", "integer"}}},
                          {"end_line", {{"type", "integer"}}}}},
         {"required", nlohmann::json::array({"path"})}});
    add("view_image", "Read an image file for a vision-capable model. The image is sent by reference and size-limited.",
        {{"type", "object"}, {"properties", {{"path", {{"type", "string"}, {"description", "Image path relative to the workspace."}}}}},
         {"required", nlohmann::json::array({"path"})}});
    add("search_text", "Search workspace text with binary, size, match and time limits.",
        {{"type", "object"}, {"properties", {
            {"query", {{"type", "string"}}}, {"path", {{"type", "string"}}},
            {"max_depth", {{"type", "integer"}}}, {"max_entries", {{"type", "integer"}}},
            {"time_budget_ms", {{"type", "integer"}}}, {"max_file_bytes", {{"type", "integer"}}},
            {"max_matches", {{"type", "integer"}}}}}, {"required", nlohmann::json::array({"query"})}});
    add("write_file", "Atomically write a UTF-8 text file after approval. Content replaces the entire file.",
        {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}, {"content", {{"type", "string"}}}}},
         {"required", nlohmann::json::array({"path", "content"})}});
    add("apply_patch", "Apply one or more literal text replacements to workspace files after approval.",
        {{"type", "object"}, {"properties", {{"changes", {{"type", "array"}, {"items", {{"type", "object"}}}}}}},
         {"required", nlohmann::json::array({"changes"})}});
    add("run_command",
        "Run a command in the workspace. The user must approve the command and its output is bounded. "
        "Optional timeout_seconds overrides the configured command timeout for this call.",
        {{"type", "object"}, {"properties", {
            {"command", {{"type", "string"}}}, {"cwd", {{"type", "string"}}},
            {"timeout_seconds", {{"type", "integer"},
             {"description", "Optional per-command timeout in seconds; 0 uses the configured default."}}}}},
         {"required", nlohmann::json::array({"command"})}});
}

std::string ToolRegistry::WorkspacePath(const ToolContext& context, const std::string& path) {
    return AbsolutePath(path, AbsolutePath(context.workspace.path, GetExecutableDirectory()));
}

bool ToolRegistry::EnsurePathAllowed(const ToolContext& context, const std::string& path, bool write, std::string* error) {
    const std::string workspace = AbsolutePath(context.workspace.path, GetExecutableDirectory());
    if (IsPathWithin(path, workspace)) return true;
    if (!write && context.workspace.allow_outside_reads) return true;
    if (write && context.workspace.allow_outside_writes) return true;
    ApprovalRequest request;
    request.kind = write ? ApprovalKind::OutsideWorkspaceWrite : ApprovalKind::OutsideWorkspaceRead;
    request.title = write ? "允许写入工作区外文件？" : "允许读取工作区外文件？";
    request.detail = path;
    if (context.approve && context.approve(request)) return true;
    if (error) *error = "path is outside the workspace and was not approved";
    return false;
}

ToolExecutionResult ToolRegistry::Execute(const std::string& name, const std::string& arguments_json,
                                         const ToolContext& context) const {
    ToolExecutionResult result;
    nlohmann::json arguments;
    try { arguments = nlohmann::json::parse(arguments_json.empty() ? "{}" : arguments_json); }
    catch (const nlohmann::json::exception& exception) {
        result.output = ToolError(std::string("invalid tool arguments: ") + exception.what());
        return result;
    }
    if (!arguments.is_object()) return {false, false, ToolError("tool arguments must be a JSON object")};
    const auto fail = [](const std::string& message) { return ToolExecutionResult{false, false, ToolError(message)}; };
    const std::string workspace = AbsolutePath(context.workspace.path, GetExecutableDirectory());
    std::string error;
    if (name == "list_files" || name == "search_text") {
        const std::string path = WorkspacePath(context, StringArgument(arguments, "path", "."));
        if (!EnsurePathAllowed(context, path, false, &error)) return fail(error);
        if (name == "search_text" && StringArgument(arguments, "query").empty()) return fail("query cannot be empty");
        return ScanWorkspace(path, workspace, arguments, context, name == "search_text");
    }
    if (name == "read_file" || name == "view_image") {
        const std::string path = WorkspacePath(context, StringArgument(arguments, "path"));
        if (!EnsurePathAllowed(context, path, false, &error)) return fail(error);
        const std::string extension = LowerAscii(PathToUtf8(NativePath(path).extension()));
        if (name == "view_image") {
            std::error_code status;
            if (!spark::fs::is_regular_file(NativePath(path), status) || status)
                return fail("view_image requires a regular file: " + path);
            const uint64_t size = spark::fs::file_size(NativePath(path), status);
            if (status) return fail("image does not exist or cannot be read: " + path);
            if (size == 0 || size > 16ull * 1024ull * 1024ull) return fail("image must be between 1 byte and 16 MB: " + path);
            if (!image_extensions.count(extension)) return fail("unsupported image type: " + extension);
            return {true, false, "image ready: " + path + " (" + extension.substr(1) + ", " +
                                 std::to_string(size) + " bytes)", {path}};
        }
        if (image_extensions.count(extension)) return fail("read_file cannot inspect image data; use view_image for " + path);
        const auto contents = ReadFileUtf8(path, &error);
        if (!contents) return fail(error);
        const int start = std::max(1, IntArgument(arguments, "start_line", 1));
        const int requested_end = IntArgument(arguments, "end_line", 0);
        const int end = requested_end > 0 ? std::max(start, requested_end) : 0;
        if (start == 1 && end == 0) result.output = *contents;
        else {
            std::istringstream stream(*contents);
            std::string line;
            int number = 0;
            while (std::getline(stream, line)) {
                if (context.cancellation && context.cancellation->IsCancelled()) return fail("cancelled");
                ++number;
                if (number >= start) result.output += std::to_string(number) + ": " + line + "\n";
                if ((end && number >= end) || result.output.size() >= static_cast<size_t>(
                        std::max(1024, context.limits.max_tool_output_bytes))) break;
            }
        }
        result.output = BoundedOutput(std::move(result.output), context.limits.max_tool_output_bytes);
        result.ok = true;
        return result;
    }
    if (name == "write_file" || name == "apply_patch") {
        const std::string policy = LowerAscii(Trim(context.workspace.file_write_approval));
        if (policy == "deny") return fail("file writes are disabled by workspace.file_write_approval");
        std::string preview;
        nlohmann::json changes;
        if (name == "write_file") changes = nlohmann::json::array({arguments});
        else {
            const auto value = arguments.find("changes");
            if (value == arguments.end() || !value->is_array() || value->empty())
                return fail("changes must be a non-empty array");
            changes = *value;
        }
        for (const auto& change : changes) {
            if (!change.is_object()) return fail("each change must be an object");
            const std::string path = WorkspacePath(context, StringArgument(change, "path"));
            if (!EnsurePathAllowed(context, path, true, &error)) return fail(error);
            preview += path + "\n";
        }
        ApprovalRequest request{ApprovalKind::FileWrite,
            name == "write_file" ? "允许写入文件？" : "允许应用补丁？", preview};
        if (policy != "allow" && context.approve && !context.approve(request)) return fail("file write was not approved");
        for (const auto& change : changes) {
            if (context.cancellation && context.cancellation->IsCancelled()) return fail("cancelled");
            const std::string path = WorkspacePath(context, StringArgument(change, "path"));
            std::string updated = StringArgument(change, "content");
            if (name == "apply_patch") {
                const auto contents = ReadFileUtf8(path, &error);
                if (!contents) return fail(error);
                const std::string old = StringArgument(change, "old_text");
                const size_t position = contents->find(old);
                if (position == std::string::npos) return fail("old_text was not found in " + path);
                updated = *contents;
                updated.replace(position, old.size(), StringArgument(change, "new_text"));
            }
            if (!WriteFileUtf8Atomic(path, updated, &error)) return fail(error);
            result.side_effecting = true;
            result.output += (name == "write_file" ? "wrote " + std::to_string(updated.size()) + " bytes to " :
                                                    "patched ") + path + "\n";
        }
        result.ok = true;
        return result;
    }
    if (name == "run_command") {
        const std::string command = StringArgument(arguments, "command");
        if (command.empty()) return fail("command cannot be empty");
        const std::string directory = WorkspacePath(context, StringArgument(arguments, "cwd", "."));
        if (!EnsurePathAllowed(context, directory, true, &error)) return fail(error);
        if (context.workspace.command_approval == "deny") return fail("command execution is disabled by workspace.command_approval");
        ApprovalRequest request{ApprovalKind::Command, "允许执行命令？", "cwd: " + directory + "\ncommand: " + command};
        if (context.workspace.command_approval != "allow" && context.approve && !context.approve(request))
            return fail("command was not approved");
        const int requested = IntArgument(arguments, "timeout_seconds", 0);
        return ExecuteShellCommand(command, directory, requested > 0 ? std::min(requested, 86400) :
                                   context.limits.tool_timeout_seconds, context);
    }
    return fail("unknown tool: " + name);
}

std::string ToolError(const std::string& message) { return "ERROR: " + message; }

}  // namespace spark
