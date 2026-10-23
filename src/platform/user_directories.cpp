#include "platform/user_directories.h"

#include "core/common.h"

#ifdef _WIN32
#include "platform/windows_compat.h"
#else
#include <pwd.h>
#include <unistd.h>
#endif

#include <sstream>

namespace spark {
namespace {
#ifndef _WIN32
std::string HomeDirectory() {
    const auto configured = EnvironmentVariable("HOME");
    if (!configured.empty() && NativePath(configured).is_absolute()) return configured;
    passwd entry{};
    passwd* result = nullptr;
    char buffer[16384]{};
    if (getpwuid_r(getuid(), &entry, buffer, sizeof(buffer), &result) == 0 &&
        result != nullptr && entry.pw_dir != nullptr) return entry.pw_dir;
    return {};
}

std::string XdgDirectory(const char* variable, const std::string& fallback) {
    const auto configured = EnvironmentVariable(variable);
    return !configured.empty() && NativePath(configured).is_absolute() ? configured : fallback;
}

std::string DocumentsDirectory(const std::string& home, const std::string& config) {
    const auto contents = ReadFileUtf8(JoinPath(config, "user-dirs.dirs"));
    std::istringstream lines(contents.value_or(""));
    std::string line;
    while (std::getline(lines, line)) {
        line = Trim(std::move(line));
        const auto separator = line.find('=');
        if (separator == std::string::npos || Trim(line.substr(0, separator)) != "XDG_DOCUMENTS_DIR") continue;
        auto value = Trim(line.substr(separator + 1));
        if (value.size() < 2 || value.front() != '"' || value.back() != '"') continue;
        value = value.substr(1, value.size() - 2);
        if (value == "$HOME" || value.rfind("$HOME/", 0) == 0) value.replace(0, 5, home);
        if (value == "${HOME}" || value.rfind("${HOME}/", 0) == 0) value.replace(0, 7, home);
        // Interpret a literal path, never shell-source this file or expand commands.
        if (value.find_first_of("$`\\") != std::string::npos || !NativePath(value).is_absolute()) continue;
        return value;
    }
    return JoinPath(home, "Documents");
}
#endif
}  // namespace

std::string UserDirectory(UserDirectoryKind kind) {
#ifdef _WIN32
    if (const auto path = windows_compat::UserDirectoryPath(kind)) {
        const std::string converted = WideToUtf8(*path);
        if (!converted.empty()) return converted;
    }
    if (kind == UserDirectoryKind::Documents) return GetExecutableDirectory();
    return EnvironmentVariable(kind == UserDirectoryKind::AppData ? "APPDATA" : "LOCALAPPDATA");
#else
    const auto home = HomeDirectory();
    if (home.empty()) return {};
#ifdef __APPLE__
    if (kind == UserDirectoryKind::Documents) return JoinPath(home, "Documents");
    return JoinPath(home, "Library/Application Support");
#else
    const auto config = XdgDirectory("XDG_CONFIG_HOME", JoinPath(home, ".config"));
    if (kind == UserDirectoryKind::Documents) return DocumentsDirectory(home, config);
    if (kind == UserDirectoryKind::AppData) return config;
    return XdgDirectory("XDG_DATA_HOME", JoinPath(home, ".local/share"));
#endif
#endif
}

}  // namespace spark
