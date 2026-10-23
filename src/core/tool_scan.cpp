#include "core/tool_scan.h"

#include "platform/clock.h"

#include <algorithm>
#include <chrono>
#include "platform/filesystem.h"
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace spark {
namespace {

namespace fs = spark::fs;
constexpr size_t kFooterReserve = 512;
constexpr size_t kDefaultOutputBytes = 16384;
constexpr size_t kMaxOutputBytes = 64 * 1024;
std::timed_mutex scan_mutex;

int LimitArgument(const nlohmann::json& args, const char* key, int fallback, int low, int high) {
    const auto value = args.find(key);
    if (!args.is_object() || value == args.end() || !value->is_number_integer()) return fallback;
    if (value->is_number_unsigned()) {
        return static_cast<int>(std::clamp<uint64_t>(value->get<uint64_t>(), low, high));
    }
    return static_cast<int>(std::clamp<int64_t>(value->get<int64_t>(), low, high));
}

struct ScanPolicy {
    int max_depth;
    uint64_t max_entries;
    uint64_t time_budget_ms;
    size_t max_file_bytes;
    size_t max_matches;

    explicit ScanPolicy(const nlohmann::json& args)
        : max_depth(LimitArgument(args, "max_depth", 8, 0, 32)),
          max_entries(LimitArgument(args, "max_entries", 4000, 1, 100000)),
          time_budget_ms(LimitArgument(args, "time_budget_ms", 2500, 100, 30000)),
          max_file_bytes(LimitArgument(args, "max_file_bytes", 2 * 1024 * 1024, 1024, 16 * 1024 * 1024)),
          max_matches(LimitArgument(args, "max_matches", 200, 1, 1000)) {}
};

bool SkipName(const fs::path& path) {
    static constexpr std::string_view skipped[] = {
        ".git", ".hg", ".svn", "node_modules", "target", "__pycache__", ".venv", "venv",
        "data", "config.json", "config.example.json", "$recycle.bin", "system volume information",
        "windows", "winsxs", "windowsapps", "program files", "program files (x86)", "programdata",
        "appdata", "recovery", "perflogs", "msocache", "config.msi",
        "pagefile.sys", "hiberfil.sys", "swapfile.sys", "id_rsa", "id_ed25519", "known_hosts",
        ".netrc", ".git-credentials", "credentials", "cookies", "login data", "key3.db", "key4.db",
        "logins.json", "auth.json",
    };
    const std::string name = LowerAscii(PathToUtf8(path.filename()));
    return std::find(std::begin(skipped), std::end(skipped), name) != std::end(skipped);
}

bool SkipPath(const fs::path& path) {
#ifdef _WIN32
    std::string lower = LowerAscii(PathToUtf8(path));
    std::replace(lower.begin(), lower.end(), '/', '\\');
    return lower.rfind("\\\\.\\", 0) == 0 || lower.rfind("\\\\?\\globalroot", 0) == 0 ||
           lower.find("\\windows\\winsxs") != std::string::npos ||
           lower.find("\\windowsapps\\") != std::string::npos ||
           (lower.size() >= 12 && lower.compare(lower.size() - 12, 12, "\\windowsapps") == 0);
#else
    (void)path;
    return false;
#endif
}

fs::path ScanPath(std::string_view value) {
    fs::path path = NativePath(value).lexically_normal();
    while (path.has_relative_path() && path.filename().empty()) path = path.parent_path();
    return path;
}

std::string DisplayPath(const fs::path& path, const fs::path& workspace) {
    auto child = path.begin();
    for (auto parent = workspace.begin(); parent != workspace.end(); ++parent, ++child) {
        if (child == path.end()) return PathToUtf8(path);
#ifdef _WIN32
        if (!EqualsIgnoreCaseAscii(PathToUtf8(*child), PathToUtf8(*parent))) return PathToUtf8(path);
#else
        if (*child != *parent) return PathToUtf8(path);
#endif
    }
    fs::path relative;
    for (; child != path.end(); ++child) relative /= *child;
    return relative.empty() ? "." : PathToUtf8(relative);
}

enum class EntryKind { Error, Skip, Directory, File };

struct EntryInfo {
    EntryKind kind = EntryKind::Error;
    uint64_t size = 0;
};

EntryInfo InspectEntry(const fs::path& path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return {};
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return {EntryKind::Skip};
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return {EntryKind::Directory};
    return {EntryKind::File, (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow};
#else
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error || !fs::exists(status)) return {};
    if (fs::is_directory(status)) return {EntryKind::Directory};
    if (!fs::is_regular_file(status)) return {EntryKind::Skip};
    const auto size = fs::file_size(path, error);
    return error ? EntryInfo{} : EntryInfo{EntryKind::File, size};
#endif
}

// Truncate a view before copying, including when a matching line is many MiB.
std::string_view Prefix(std::string_view value, size_t bytes) {
    if (value.size() <= bytes) return value;
    while (bytes > 0 && (static_cast<unsigned char>(value[bytes]) & 0xC0) == 0x80) --bytes;
    return value.substr(0, bytes);
}

struct Scan {
    const ToolContext& ctx;
    const ScanPolicy policy;
    const fs::path workspace;
    const uint64_t started = MonotonicMillis();
    const size_t output_limit;
    uint64_t scanned = 0;
    uint64_t skipped = 0;
    uint64_t warnings = 0;
    size_t matches = 0;
    size_t row_bytes = 0;
    std::string reason;
    std::string cursor;
    std::vector<std::string> rows;

    Scan(const ToolContext& context, const nlohmann::json& args, fs::path root, size_t limit)
        : ctx(context), policy(args), workspace(std::move(root)), output_limit(limit) {}

    void Stop(const char* cause) {
        if (reason.empty()) reason = cause;
    }

    bool Check() {
        if (ctx.cancellation != nullptr && ctx.cancellation->IsCancelled()) Stop("cancelled");
        if (MonotonicMillis() - started >= policy.time_budget_ms) Stop("time_budget");
        return reason.empty();
    }

    EntryInfo InspectRoot(const fs::path& root) {
        EntryInfo result;
        fs::path component = root;
        while (!component.empty() && Check()) {
            // Validate explicit subpaths too; entering data/foo or link/foo must
            // not bypass the exclusions applied to enumerated children.
            const bool at_workspace = DisplayPath(component, workspace) == ".";
            if ((!at_workspace && SkipName(component)) || SkipPath(component)) return {EntryKind::Skip};
            const EntryInfo info = InspectEntry(component);
            if (component == root) result = info;
            if (info.kind == EntryKind::Skip || info.kind == EntryKind::Error) return info;
            if (at_workspace) break;
            const fs::path parent = component.parent_path();
            if (parent == component) break;
            component = parent;
        }
        return result;
    }

    bool AddRow(std::string_view prefix, std::string_view text = {}) {
        const size_t available = output_limit - kFooterReserve - row_bytes;
        if (available == 0) {
            Stop("output_limit");
            return false;
        }
        std::string row(Prefix(prefix, available - 1));
        if (row.size() == prefix.size()) row.append(Prefix(text, available - 1 - row.size()));
        const bool complete = row.size() == prefix.size() + text.size();
        row += '\n';
        row_bytes += row.size();
        rows.push_back(std::move(row));
        if (!complete) Stop("output_limit");
        return complete;
    }

    std::optional<std::string> ReadText(const fs::path& path) {
#ifdef _WIN32
        struct File {
            HANDLE handle;
            ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
        };
        const File file{CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
        BY_HANDLE_FILE_INFORMATION info{};
        if (file.handle == INVALID_HANDLE_VALUE || GetFileType(file.handle) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(file.handle, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
            return std::nullopt;
        }
        const uint64_t size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
#else
        struct File {
            int descriptor;
            ~File() { if (descriptor >= 0) ::close(descriptor); }
        };
        int descriptor;
        do {
            if (!Check()) return std::nullopt;
            descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        } while (descriptor < 0 && errno == EINTR);
        const File file{descriptor};
        if (descriptor < 0) return std::nullopt;
        struct stat info{};
        int status;
        do {
            if (!Check()) return std::nullopt;
            status = ::fstat(file.descriptor, &info);
        } while (status < 0 && errno == EINTR);
        if (status != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) return std::nullopt;
        const uint64_t size = static_cast<uint64_t>(info.st_size);
#endif
        if (size > policy.max_file_bytes) return std::nullopt;
        std::string contents;
        contents.reserve(static_cast<size_t>(size));
        char buffer[16384];
        while (Check()) {
            // Read at most one extra byte to detect growth past the size cap.
            const size_t requested = std::min(sizeof(buffer), policy.max_file_bytes + 1 - contents.size());
#ifdef _WIN32
            DWORD count = 0;
            if (!ReadFile(file.handle, buffer, static_cast<DWORD>(requested), &count, nullptr)) return std::nullopt;
#else
            const auto count = ::read(file.descriptor, buffer, requested);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) return std::nullopt;
#endif
            if (count == 0) {
                if (contents.compare(0, 3, "\xEF\xBB\xBF") == 0) contents.erase(0, 3);
                return contents;
            }
            contents.append(buffer, static_cast<size_t>(count));
            if (contents.size() > policy.max_file_bytes) return std::nullopt;
        }
        return std::nullopt;
    }

    void SearchFile(const fs::path& path, uint64_t size, const std::string& query) {
        if (!Check()) return;
        if (size > policy.max_file_bytes) {
            ++skipped;
            ++warnings;
            return;
        }
        const auto contents = ReadText(path);
        if (!contents) {
            ++skipped;
            if (reason.empty()) ++warnings;
            return;
        }
        if (std::string_view(*contents).substr(0, 8192).find('\0') != std::string_view::npos) {
            ++skipped;
            return;
        }
        size_t line_number = 0;
        size_t offset = 0;
        while (offset < contents->size() && Check()) {
            const size_t end = contents->find('\n', offset);
            const size_t length = (end == std::string::npos ? contents->size() : end) - offset;
            const std::string_view line(contents->data() + offset, length);
            ++line_number;
            if (line.find(query) != std::string_view::npos) {
                ++matches;
                if (!AddRow(DisplayPath(path, workspace) + ":" + std::to_string(line_number) + ": ", line)) break;
                if (matches >= policy.max_matches) {
                    Stop("match_limit");
                    break;
                }
            }
            if (end == std::string::npos) break;
            offset = end + 1;
        }
    }

    std::string Finish(bool search) {
        if (!search) std::sort(rows.begin(), rows.end());
        std::string output;
        for (const auto& row : rows) output += row;
        if (search && matches == 0) output = "(no matches)\n";
        if (!reason.empty()) {
            output += "[scan] result is partial; narrow the path or raise a limit.\n";
            if (reason == "output_limit") output += "[output truncated by Spark limit]\n";
        }
        std::ostringstream footer;
        footer << "[scan] truncated=" << (reason.empty() ? "false" : "true")
               << " reason=" << (reason.empty() ? "none" : reason)
               << " scanned_entries=" << scanned << " skipped_entries=" << skipped
               << " warnings=" << warnings << " elapsed_ms=" << MonotonicMillis() - started;
        if (!cursor.empty()) footer << " cursor=" << std::string(Prefix(cursor, 160));
        output += footer.str() + "\n";
        return output;
    }
};

}  // namespace

ToolExecutionResult ScanWorkspace(const std::string& path, const std::string& workspace,
                                 const nlohmann::json& args, const ToolContext& ctx, bool search) {
    ToolExecutionResult result;
    const size_t output_limit = std::min(kMaxOutputBytes, ctx.limits.max_tool_output_bytes > 0
        ? static_cast<size_t>(ctx.limits.max_tool_output_bytes) : kDefaultOutputBytes);
    if (output_limit < kFooterReserve) {
        result.output = std::string(Prefix(ToolError("scan output limit must be at least 512 bytes"), output_limit));
        return result;
    }
    std::string query;
    const auto query_arg = args.find("query");
    if (search && args.is_object() && query_arg != args.end() && query_arg->is_string()) {
        query = query_arg->get<std::string>();
    }
    if (search && query.empty()) {
        result.output = ToolError("query cannot be empty");
        return result;
    }
    Scan scan(ctx, args, ScanPath(workspace), output_limit);
    std::unique_lock<std::timed_mutex> lock(scan_mutex, std::defer_lock);
    while (scan.Check() && !lock.try_lock_for(std::chrono::milliseconds(10))) {}
    if (scan.Check()) {
        const fs::path root = ScanPath(path);
        const EntryInfo info = SkipName(root) || SkipPath(root)
            ? EntryInfo{EntryKind::Skip} : scan.InspectRoot(root);
        if (!scan.Check()) {
            result.ok = true;
            result.output = scan.Finish(search);
            return result;
        }
        if (SkipName(root) || SkipPath(root) || info.kind == EntryKind::Skip) {
            ++scan.skipped;
        } else if (info.kind == EntryKind::Error || (!search && info.kind != EntryKind::Directory)) {
            const std::string error = ToolError((search ? "path does not exist: " : "directory does not exist: ") + path);
            result.output = std::string(Prefix(error, output_limit));
            return result;
        } else if (info.kind == EntryKind::File) {
            ++scan.scanned;
            scan.cursor = DisplayPath(root, scan.workspace);
            scan.SearchFile(root, info.size, query);
        } else {
            struct DirectoryFrame {
                fs::directory_iterator iterator;
                int depth;
            };
            std::vector<DirectoryFrame> frames;
            const auto enter = [&](const fs::path& directory, int depth) {
                if (!scan.Check()) return;
                const EntryInfo current = InspectEntry(directory);
                if (current.kind != EntryKind::Directory) {
                    ++scan.skipped;
                    if (current.kind == EntryKind::Error) ++scan.warnings;
                    return;
                }
                std::error_code error;
                fs::directory_iterator iterator(directory, fs::directory_options::none, error);
                if (error) {
                    ++scan.skipped;
                    ++scan.warnings;
                } else {
                    frames.push_back({std::move(iterator), depth});
                }
            };
            enter(root, 0);
            while (!frames.empty() && scan.Check()) {
                auto& frame = frames.back();
                if (frame.iterator == fs::directory_iterator{}) {
                    frames.pop_back();
                    continue;
                }
                if (scan.scanned >= scan.policy.max_entries) {
                    scan.Stop("entry_limit");
                    break;
                }
                const fs::path child = frame.iterator->path();
                const int depth = frame.depth;
                std::error_code error;
                frame.iterator.increment(error);
                if (error) {
                    ++scan.warnings;
                    frame.iterator = fs::directory_iterator{};
                }
                ++scan.scanned;
                scan.cursor = DisplayPath(child, scan.workspace);
                if (SkipName(child) || SkipPath(child)) {
                    ++scan.skipped;
                    continue;
                }
                const EntryInfo entry = InspectEntry(child);
                if (entry.kind == EntryKind::Skip || entry.kind == EntryKind::Error) {
                    ++scan.skipped;
                    if (entry.kind == EntryKind::Error) ++scan.warnings;
                    continue;
                }
                if (entry.kind == EntryKind::Directory) {
                    if (!search && !scan.AddRow(scan.cursor + "/")) break;
                    if (depth < scan.policy.max_depth) enter(child, depth + 1);
                } else if (search) {
                    scan.SearchFile(child, entry.size, query);
                } else if (!scan.AddRow(scan.cursor + " (" + std::to_string(entry.size) + " bytes)")) {
                    break;
                }
            }
        }
    }
    result.ok = true;
    result.output = scan.Finish(search);
    return result;
}

}  // namespace spark
