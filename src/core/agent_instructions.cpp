#include "core/agent_instructions.h"

#include "core/common.h"

#include "platform/user_directories.h"

#include <algorithm>
#include <sstream>
#include <vector>

namespace spark {
namespace {

constexpr size_t kMaxBytesPerInstructionFile = 32 * 1024;
constexpr size_t kMaxBytesTotal = 128 * 1024;

struct InstructionFile {
    std::string path;
    std::string content;
    bool truncated = false;
};

bool IsRootPath(const std::string& path) {
    const std::string parent = ParentPath(path);
    return parent.empty() || NormalizePathForComparison(parent) == NormalizePathForComparison(path);
}

std::string FindProjectRoot(const std::string& workspace) {
    std::string current = AbsolutePath(workspace);
    for (;;) {
        if (PathExists(JoinPath(current, ".git"))) return current;
        if (IsRootPath(current)) return {};
        current = ParentPath(current);
    }
}

std::vector<std::string> ProjectAncestors(const std::string& workspace, const std::string& project_root) {
    std::vector<std::string> directories;
    if (project_root.empty()) {
        directories.push_back(AbsolutePath(workspace));
        return directories;
    }
    std::string current = AbsolutePath(workspace);
    const std::string normalized_root = NormalizePathForComparison(project_root);
    for (;;) {
        directories.push_back(current);
        if (NormalizePathForComparison(current) == normalized_root || IsRootPath(current)) break;
        current = ParentPath(current);
    }
    std::reverse(directories.begin(), directories.end());
    return directories;
}

void AppendInstructionFile(std::vector<InstructionFile>& files, const std::string& path, size_t* total_bytes) {
    if (path.empty() || !PathExists(path) || *total_bytes >= kMaxBytesTotal) return;
    std::string error;
    const auto contents = ReadFileUtf8(path, &error);
    if (!contents.has_value()) return;

    InstructionFile file;
    file.path = AbsolutePath(path);
    file.content = *contents;
    if (file.content.size() > kMaxBytesPerInstructionFile) {
        file.content = TruncateUtf8(std::move(file.content), kMaxBytesPerInstructionFile);
        file.truncated = true;
    }
    const size_t remaining = kMaxBytesTotal - *total_bytes;
    if (file.content.size() > remaining) {
        file.content = TruncateUtf8(std::move(file.content), remaining);
        file.truncated = true;
    }
    *total_bytes += file.content.size();
    files.push_back(std::move(file));
}

}  // namespace

std::string BuildAgentInstructionsPrompt(const std::string& workspace) {
    if (Trim(workspace).empty()) return {};

    std::vector<InstructionFile> files;
    size_t total_bytes = 0;

    const auto user_directory = UserDirectory(UserDirectoryKind::AppData);
    if (!user_directory.empty()) {
        AppendInstructionFile(files, JoinPath(user_directory, "Spark/AGENTS.md"), &total_bytes);
    }

    const std::string project_root = FindProjectRoot(workspace);
    for (const std::string& directory : ProjectAncestors(workspace, project_root)) {
        AppendInstructionFile(files, JoinPath(directory, "AGENTS.md"), &total_bytes);
        if (total_bytes >= kMaxBytesTotal) break;
    }

    if (files.empty()) return {};

    std::ostringstream prompt;
    prompt << "\n\n# AGENTS.md instructions\n"
              "The following directory-scoped instructions come from the user's project. Apply the most specific file "
              "when instructions differ. They do not override system, safety, or approval-policy instructions.\n";
    for (const InstructionFile& file : files) {
        prompt << "\n<AGENTS_FILE path=\"" << file.path << "\">\n";
        prompt << file.content;
        if (!file.content.empty() && file.content.back() != '\n') prompt << "\n";
        if (file.truncated) prompt << "[truncated to fit the instruction budget]\n";
        prompt << "</AGENTS_FILE>\n";
    }
    return prompt.str();
}

}  // namespace spark
