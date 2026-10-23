#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>
#include <cstdint>
#include "platform/filesystem.h"
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spark {

template <typename T>
struct Result {
    bool ok = false;
    T value{};
    std::string error;

    static Result Success(T value) { return Result{true, std::move(value), {}}; }
    static Result Failure(std::string error) { return Result{false, {}, std::move(error)}; }
};

template <>
struct Result<void> {
    bool ok = false;
    std::string error;

    static Result Success() { return Result{true, {}}; }
    static Result Failure(std::string error) { return Result{false, std::move(error)}; }
};

class CancellationToken {
public:
    void Cancel() { cancelled_.store(true, std::memory_order_release); }
    bool IsCancelled() const { return cancelled_.load(std::memory_order_acquire); }
    void Reset() { cancelled_.store(false, std::memory_order_release); }

private:
    std::atomic<bool> cancelled_{false};
};

std::string WideToUtf8(std::wstring_view value);
std::wstring Utf8ToWide(std::string_view value);
bool IsValidUtf8(std::string_view value);
std::string DecodeBytesToUtf8(std::string_view value, uint32_t code_page);
spark::fs::path NativePath(std::string_view value);
std::string PathToUtf8(const spark::fs::path& value);
// Removes terminal control sequences and control characters that would otherwise
// render as stray glyphs in Win32 text controls. Newlines and tabs are preserved.
std::string SanitizeDisplayText(std::string_view value);
std::string TruncateUtf8(std::string value, size_t max_bytes);
std::string Trim(std::string value);
std::wstring Trim(std::wstring value);
std::string LowerAscii(std::string value);
bool EqualsIgnoreCaseAscii(std::string_view left, std::string_view right);
bool IsQuotaExhaustedError(std::string_view text);
std::string JoinPath(std::string_view left, std::string_view right);
std::string ParentPath(std::string_view path);
std::string AbsolutePath(std::string_view path, std::string_view base = {});
std::string NormalizePathForComparison(std::string path);
std::string CanonicalPathForAccess(std::string_view path);
bool PathExists(std::string_view path);
bool IsDirectory(std::string_view path);
bool IsPathWithin(std::string_view child, std::string_view parent);
std::optional<std::string> ReadFileUtf8(std::string_view path, std::string* error = nullptr);
bool WriteFileUtf8Atomic(std::string_view path, std::string_view contents, std::string* error = nullptr);
// Rewrites a UTF-8 text file for legacy Windows editors: CRLF endings plus a UTF-8
// BOM, so Windows 7 Notepad shows correct lines and Chinese text.
std::string WindowsTextFileContents(std::string_view utf8);
bool WriteWindowsTextFileAtomic(std::string_view path, std::string_view utf8, std::string* error = nullptr);
std::string GetExecutablePath();
std::string GetExecutableDirectory();
std::string CurrentDirectoryUtf8();
std::string EnvironmentVariable(std::string_view name);
std::string FormatWin32Error(uint32_t error);
std::string RedactSecrets(std::string text, const std::vector<std::string>& secrets);
uint64_t UnixTimeSeconds();
std::string IsoUtcTime();
std::string JsonEscape(std::string_view value);

}  // namespace spark
