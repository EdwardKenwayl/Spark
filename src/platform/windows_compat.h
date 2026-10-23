#pragma once

#ifdef _WIN32
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace spark {
enum class UserDirectoryKind;

namespace windows_compat {

// Retains system-DPI awareness where supported; a missing API is a no-op.
void EnableProcessDpiAwareness() noexcept;

// Uses native 64-bit uptime when available. XP extends ticks observed in this
// process; earlier wraps and gaps of 49.7 days or longer cannot be recovered.
// Synchronization failure terminates rather than returning an unsafe sample.
uint64_t TickMilliseconds() noexcept;

// Prefers the UI locale, then the user locale. XP maps LANGIDs to app languages.
std::wstring LocaleName();

// Borrows the handle. Returns an absolute DOS/UNC final path, or no value.
// Missing or failed final-path resolution must never authorize lexical paths.
std::optional<std::wstring> FinalPathForHandle(HANDLE file);

// Uses known folders when available and legacy CSIDLs on older Windows.
std::optional<std::wstring> UserDirectoryPath(UserDirectoryKind kind);

// Reject malformed Unicode consistently, including on pre-Vista Windows.
std::string WideToUtf8(std::wstring_view value);
std::wstring Utf8ToWide(std::string_view value);
bool IsValidUtf8(std::string_view value) noexcept;

}  // namespace windows_compat
}  // namespace spark
#endif
