#ifndef _WIN32
#include "core/common.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace spark {
namespace {
namespace fs = spark::fs;
constexpr size_t kFileLimit = 64 * 1024 * 1024;
constexpr size_t kPathLimit = 64 * 1024;

bool ValidPath(std::string_view path) {
    return !path.empty() && path.size() <= kPathLimit && path.find('\0') == std::string_view::npos;
}

template <typename Operation>
auto RetryInterrupted(Operation operation) {
    decltype(operation()) result;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        result = operation();
        if (result >= 0 || errno != EINTR) return result;
    }
    return result;
}

bool Fail(std::string* error, std::string_view operation, int code) {
    if (error) *error = std::string(operation) + ": " + FormatWin32Error(static_cast<uint32_t>(code));
    return false;
}

struct File {
    int descriptor;
    std::string temporary;
    int directory = -1;
    ~File() {
        if (descriptor >= 0) ::close(descriptor);
        if (!temporary.empty()) RetryInterrupted([&] { return ::unlinkat(directory, temporary.c_str(), 0); });
    }
    // Never retry close: an interrupted close can already have released the descriptor.
    int Close() { return ::close(std::exchange(descriptor, -1)); }
};

bool NextScalar(std::string_view input, size_t& index, uint32_t& scalar) {
    const auto lead = static_cast<unsigned char>(input[index++]);
    unsigned extra = 0;
    uint32_t minimum = 0;
    if (lead < 0x80) { scalar = lead; return true; }
    if (lead >= 0xC2 && lead <= 0xDF) { extra = 1; scalar = lead & 0x1F; minimum = 0x80; }
    else if (lead >= 0xE0 && lead <= 0xEF) { extra = 2; scalar = lead & 0x0F; minimum = 0x800; }
    else if (lead >= 0xF0 && lead <= 0xF4) { extra = 3; scalar = lead & 0x07; minimum = 0x10000; }
    else return false;
    if (input.size() - index < extra) return false;
    for (unsigned count = 0; count < extra; ++count) {
        const auto byte = static_cast<unsigned char>(input[index++]);
        if ((byte & 0xC0) != 0x80) return false;
        scalar = (scalar << 6) | (byte & 0x3F);
    }
    return scalar >= minimum && scalar <= 0x10FFFF && !(scalar >= 0xD800 && scalar <= 0xDFFF);
}
}  // namespace

std::string WideToUtf8(std::wstring_view value) {
    std::string output;
    for (wchar_t character : value) {
        const auto scalar = static_cast<uint32_t>(character);
        if (scalar > 0x10FFFF || (scalar >= 0xD800 && scalar <= 0xDFFF)) return {};
        if (scalar < 0x80) { output.push_back(static_cast<char>(scalar)); continue; }
        const unsigned count = scalar < 0x800 ? 2 : scalar < 0x10000 ? 3 : 4;
        output.push_back(static_cast<char>((0xFFu << (8 - count)) | (scalar >> (6 * (count - 1)))));
        for (unsigned remaining = count - 1; remaining > 0; --remaining)
            output.push_back(static_cast<char>(0x80 | ((scalar >> (6 * (remaining - 1))) & 0x3F)));
    }
    return output;
}

std::wstring Utf8ToWide(std::string_view value) {
    std::wstring output;
    size_t index = 0;
    uint32_t scalar = 0;
    while (index < value.size()) {
        if (!NextScalar(value, index, scalar)) return {};
        output.push_back(static_cast<wchar_t>(scalar));
    }
    return output;
}

bool IsValidUtf8(std::string_view value) {
    size_t index = 0;
    uint32_t scalar = 0;
    while (index < value.size()) if (!NextScalar(value, index, scalar)) return false;
    return true;
}

std::string DecodeBytesToUtf8(std::string_view value, uint32_t /*code_page*/) {
    if (IsValidUtf8(value)) return std::string(value);
    std::string output(value);
    for (char& byte : output) if (static_cast<unsigned char>(byte) >= 0x80) byte = '?';
    return output;
}

std::string JoinPath(std::string_view left, std::string_view right) {
    if (left.empty()) return ValidPath(right) ? std::string(right) : std::string{};
    if (right.empty()) return ValidPath(left) ? std::string(left) : std::string{};
    if (!ValidPath(left) || !ValidPath(right)) return {};
    return (fs::u8path(left) / fs::u8path(right)).string();
}

std::string ParentPath(std::string_view path) {
    return ValidPath(path) ? fs::u8path(path).parent_path().string() : std::string{};
}

std::string AbsolutePath(std::string_view path, std::string_view base) {
    if (!ValidPath(path) || (!base.empty() && !ValidPath(base))) return {};
    std::error_code error;
    fs::path input = fs::u8path(path);
    if (input.is_relative() && !base.empty()) input = fs::u8path(base) / input;
    input = fs::absolute(input, error);
    return error ? std::string{} : input.lexically_normal().string();
}

std::string NormalizePathForComparison(std::string path) {
    path = AbsolutePath(path);
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
}

std::string CanonicalPathForAccess(std::string_view path) {
    if (!ValidPath(path)) return {};
    std::error_code error;
    const fs::path input = fs::absolute(fs::u8path(path), error);
    if (error) return {};
    fs::path resolved = input.root_path();
    const fs::path relative = input.relative_path();
    std::deque<fs::path> pending(relative.begin(), relative.end());
    unsigned links = 0, steps = 0;
    // Resolve links before ".."; weakly_canonical alone misses dangling-link escapes.
    while (!pending.empty()) {
        if (++steps > 4096 || pending.size() > 4096) return {};
        const fs::path component = pending.front();
        pending.pop_front();
        if (component.empty() || component == ".") continue;
        if (component == "..") { resolved = resolved.parent_path(); continue; }
        const fs::path candidate = resolved / component;
        if (candidate.native().size() > kPathLimit) return {};
        const auto status = fs::symlink_status(candidate, error);
        if (error && error != std::errc::no_such_file_or_directory) return {};
        if (!fs::is_symlink(status)) {
            if (fs::exists(status) && !fs::is_directory(status) && !pending.empty()) return {};
            resolved = candidate;
            continue;
        }
        if (++links > 40) return {};
        const fs::path target = fs::read_symlink(candidate, error);
        if (error || target.empty() || target.native().size() > kPathLimit) return {};
        if (target.is_absolute()) resolved = target.root_path();
        const fs::path suffix = target.relative_path();
        pending.insert(pending.begin(), suffix.begin(), suffix.end());
    }
    return resolved.lexically_normal().string();
}

bool PathExists(std::string_view path) {
    if (!ValidPath(path)) return false;
    std::error_code error;
    return fs::exists(fs::u8path(path), error) && !error;
}

bool IsDirectory(std::string_view path) {
    if (!ValidPath(path)) return false;
    std::error_code error;
    return fs::is_directory(fs::u8path(path), error) && !error;
}

bool IsPathWithin(std::string_view child, std::string_view parent) {
    const std::string c = CanonicalPathForAccess(child), p = CanonicalPathForAccess(parent);
    if (c.empty() || p.empty()) return false;
    return c == p || p == "/" ||
           (c.size() > p.size() && c.compare(0, p.size(), p) == 0 && c[p.size()] == '/');
}

std::optional<std::string> ReadFileUtf8(std::string_view path, std::string* error) {
    if (error) error->clear();
    const auto fail = [&](std::string_view operation, int code) -> std::optional<std::string> {
        Fail(error, std::string(operation) + " '" + std::string(path) + "'", code);
        return std::nullopt;
    };
    if (!ValidPath(path)) return fail("invalid read path", EINVAL);
    const fs::path native = fs::u8path(path);
    File file{RetryInterrupted([&] { return ::open(native.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK); }), {}};
    if (file.descriptor < 0) return fail("open", errno);
    struct stat info {};
    if (RetryInterrupted([&] { return ::fstat(file.descriptor, &info); }) < 0) return fail("fstat", errno);
    if (!S_ISREG(info.st_mode)) return fail("read requires a regular file", EINVAL);
    if (info.st_size < 0 || static_cast<uint64_t>(info.st_size) > kFileLimit)
        return fail("read exceeds 64 MiB", EFBIG);
    std::string contents;
    contents.reserve(static_cast<size_t>(info.st_size));
    std::array<char, 64 * 1024> buffer;
    for (;;) {
        const size_t requested = std::min(buffer.size(), kFileLimit - contents.size() + 1);
        const auto count = RetryInterrupted([&] { return ::read(file.descriptor, buffer.data(), requested); });
        if (count < 0) return fail("read", errno);
        if (count == 0) break;
        if (static_cast<size_t>(count) > kFileLimit - contents.size()) return fail("read exceeds 64 MiB", EFBIG);
        contents.append(buffer.data(), static_cast<size_t>(count));
    }
    if (file.Close() < 0) return fail("close after read", errno);
    if (contents.compare(0, 3, "\xEF\xBB\xBF") == 0) contents.erase(0, 3);
    return contents;
}

bool WriteFileUtf8Atomic(std::string_view path, std::string_view contents, std::string* error) {
    if (error) error->clear();
    const auto fail = [&](std::string_view operation, int code) {
        return Fail(error, std::string(operation) + " '" + std::string(path) + "'", code);
    };
    if (!ValidPath(path)) return fail("invalid write path", EINVAL);
    if (contents.size() > kFileLimit) return fail("write exceeds 64 MiB", EFBIG);
    const fs::path target = fs::u8path(path);
    const std::string name = target.filename().string();
    if (name.empty() || name == "." || name == "..") return fail("invalid target filename", EINVAL);
    const fs::path parent = target.has_parent_path() ? target.parent_path() : fs::path(".");
    File directory{RetryInterrupted([&] { return ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); }), {}};
    if (directory.descriptor < 0) return fail("open parent directory", errno);
    struct stat existing {};
    const int inspected = RetryInterrupted([&] {
        return ::fstatat(directory.descriptor, name.c_str(), &existing, AT_SYMLINK_NOFOLLOW);
    });
    if (inspected < 0 && errno != ENOENT) return fail("lstat", errno);
    if (inspected == 0 && !S_ISREG(existing.st_mode) && !S_ISLNK(existing.st_mode))
        return fail("atomic write requires a regular file or symlink", EINVAL);
    static std::atomic<uint64_t> sequence{0};
    File file{-1, {}, directory.descriptor};
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto serial = sequence.fetch_add(1, std::memory_order_relaxed);
        const std::string temporary =
            ".spark-" + std::to_string(getpid()) + "-" + std::to_string(tick) + "-" + std::to_string(serial);
        file.descriptor = RetryInterrupted([&] {
            return ::openat(directory.descriptor, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        });
        if (file.descriptor >= 0) {
            file.temporary = temporary;
            break;
        }
        if (errno != EEXIST) return fail("create temporary file", errno);
    }
    if (file.descriptor < 0) return fail("temporary filename collision limit", EEXIST);
    const int descriptor = file.descriptor;
    size_t offset = 0;
    while (offset < contents.size()) {
        const size_t requested = std::min<size_t>(contents.size() - offset, 1024 * 1024);
        const auto count = RetryInterrupted([&] { return ::write(descriptor, contents.data() + offset, requested); });
        if (count < 0) return fail("write temporary file", errno);
        if (count == 0) return fail("write made no progress", EIO);
        offset += static_cast<size_t>(count);
    }
    // Keep the temp private until complete; preserve ordinary mode bits, never set-id bits.
    const mode_t mode = inspected == 0 && S_ISREG(existing.st_mode) ? existing.st_mode & 0777 : 0600;
    if (RetryInterrupted([&] { return ::fchmod(descriptor, mode); }) < 0) return fail("fchmod", errno);
    if (RetryInterrupted([&] { return ::fsync(descriptor); }) < 0) return fail("fsync", errno);
    if (file.Close() < 0) return fail("close before rename", errno);
    // Anchor creation, commit and cleanup to the same opened parent even if it is renamed.
    if (RetryInterrupted([&] {
            return ::renameat(directory.descriptor, file.temporary.c_str(), directory.descriptor, name.c_str());
        }) < 0)
        return fail("rename temporary file", errno);
    file.temporary.clear();
    return true;
}

std::string GetExecutablePath() {
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0 || size > kPathLimit) return {};
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    buffer.resize(std::strlen(buffer.c_str()));
    return CanonicalPathForAccess(buffer);
#elif defined(__linux__)
    for (size_t size = 256; size <= kPathLimit; size *= 2) {
        std::string buffer(size, '\0');
        const auto count = RetryInterrupted([&] { return ::readlink("/proc/self/exe", buffer.data(), buffer.size()); });
        if (count < 0) return {};
        if (static_cast<size_t>(count) < buffer.size()) {
            buffer.resize(static_cast<size_t>(count));
            return buffer;
        }
    }
    return {};
#else
    return {};
#endif
}

std::string CurrentDirectoryUtf8() {
    std::error_code error;
    const fs::path directory = fs::current_path(error);
    return error || directory.native().size() > kPathLimit ? std::string{} : directory.string();
}

std::string EnvironmentVariable(std::string_view name) {
    if (!ValidPath(name) || name.find('=') != std::string_view::npos) return {};
    const char* value = std::getenv(std::string(name).c_str());
    if (!value) return {};
    const size_t length = ::strnlen(value, 1024 * 1024 + 1);
    return length > 1024 * 1024 ? std::string{} : std::string(value, length);
}

std::string FormatWin32Error(uint32_t error) {
    const std::string message = error <= static_cast<uint32_t>(std::numeric_limits<int>::max())
        ? std::error_code(static_cast<int>(error), std::generic_category()).message() : "unknown error";
    return "POSIX error " + std::to_string(error) + ": " + message;
}
}  // namespace spark
#endif  // !_WIN32
