#include "core/common.h"

#ifdef _WIN32
#include "platform/windows_compat.h"
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <iomanip>
#include <memory>
#include <sstream>

namespace spark {
namespace {

#ifdef _WIN32
std::string FormatSystemError(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer),
        0,
        nullptr);
    std::wstring message;
    if (length != 0 && buffer != nullptr) {
        message.assign(buffer, length);
        LocalFree(buffer);
    } else {
        message = L"Windows error " + std::to_wstring(error);
    }
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }
    return WideToUtf8(message);
}

spark::fs::path SafePath(std::string_view path) {
    return spark::fs::path(Utf8ToWide(path));
}
#endif

}  // namespace

spark::fs::path NativePath(std::string_view value) {
#ifdef _WIN32
    return spark::fs::path(Utf8ToWide(value));
#else
    return spark::fs::u8path(value);
#endif
}

std::string PathToUtf8(const spark::fs::path& value) {
    return value.u8string();
}

#ifdef _WIN32
std::string WideToUtf8(std::wstring_view value) {
    return windows_compat::WideToUtf8(value);
}

std::wstring Utf8ToWide(std::string_view value) {
    return windows_compat::Utf8ToWide(value);
}

bool IsValidUtf8(std::string_view value) {
    return windows_compat::IsValidUtf8(value);
}

std::string DecodeBytesToUtf8(std::string_view value, uint32_t code_page) {
    if (value.empty() || IsValidUtf8(value)) return std::string(value);
    UINT flags = MB_ERR_INVALID_CHARS;
    int required = MultiByteToWideChar(code_page, flags, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        flags = 0;
        required = MultiByteToWideChar(code_page, flags, value.data(), static_cast<int>(value.size()), nullptr, 0);
    }
    if (required <= 0) {
        std::string fallback;
        fallback.reserve(value.size());
        for (const unsigned char character : value) fallback.push_back(character < 0x80 ? static_cast<char>(character) : '?');
        return fallback;
    }
    std::wstring wide(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(code_page, flags, value.data(), static_cast<int>(value.size()), wide.data(), required);
    return WideToUtf8(wide);
}
#endif

std::string SanitizeDisplayText(std::string_view value) {
    if (value.empty()) return {};

    std::string output;
    output.reserve(value.size());
    size_t index = 0;
    while (index < value.size()) {
        const unsigned char character = static_cast<unsigned char>(value[index]);

        // Windows console/terminal escape sequences are common in captured command
        // output. Drop the whole CSI/OSC sequence instead of showing its parameters.
        if (character == 0x1B) {
            if (index + 1 >= value.size()) break;
            const unsigned char kind = static_cast<unsigned char>(value[index + 1]);
            if (kind == '[') {
                index += 2;
                while (index < value.size()) {
                    const unsigned char current = static_cast<unsigned char>(value[index++]);
                    if (current >= 0x40 && current <= 0x7E) break;
                }
                continue;
            }
            if (kind == ']') {
                index += 2;
                while (index < value.size()) {
                    const unsigned char current = static_cast<unsigned char>(value[index++]);
                    if (current == 0x07) break;
                    if (current == 0x1B && index < value.size() && value[index] == '\\') {
                        ++index;
                        break;
                    }
                }
                continue;
            }
            ++index;
            continue;
        }

        // U+0080..U+009F are C1 control characters in UTF-8.
        if (character == 0xC2 && index + 1 < value.size()) {
            const unsigned char next = static_cast<unsigned char>(value[index + 1]);
            if (next >= 0x80 && next <= 0x9F) {
                // C1 CSI (U+009B) is followed by parameters and a final byte.
                if (next == 0x9B) {
                    index += 2;
                    while (index < value.size()) {
                        const unsigned char current = static_cast<unsigned char>(value[index++]);
                        if (current >= 0x40 && current <= 0x7E) break;
                    }
                } else {
                    index += 2;
                }
                continue;
            }
        }

        if ((character < 0x20 && character != '\n' && character != '\t') || character == 0x7F) {
            ++index;
            continue;
        }

        output.push_back(value[index]);
        ++index;
    }
    return output;
}

std::string TruncateUtf8(std::string value, size_t max_bytes) {
    if (max_bytes >= value.size()) return value;
    if (max_bytes == 0) return {};
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0) == 0x80) --cut;
    if (cut == 0) return {};
    const unsigned char lead = static_cast<unsigned char>(value[cut]);
    size_t sequence_length = 1;
    if ((lead & 0xE0) == 0xC0) sequence_length = 2;
    else if ((lead & 0xF0) == 0xE0) sequence_length = 3;
    else if ((lead & 0xF8) == 0xF0) sequence_length = 4;
    if (cut + sequence_length > max_bytes) return value.substr(0, cut);
    return value.substr(0, max_bytes);
}

std::string Trim(std::string value) {
    auto not_space = [](unsigned char character) { return std::isspace(character) == 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::wstring Trim(std::wstring value) {
    auto not_space = [](wchar_t character) { return std::iswspace(character) == 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool EqualsIgnoreCaseAscii(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

bool IsQuotaExhaustedError(std::string_view text) {
    const std::string normalized = LowerAscii(std::string(text));
    constexpr std::string_view patterns[] = {
        "insufficient balance",
        "insufficient_quota",
        "insufficient quota",
        "quota exceeded",
        "exceeded your current quota",
        "credit balance is too low",
        "billing hard limit",
        "payment required",
        "http 402",
        "status 402",
        "\"status\": 402",
        "\"status\":402",
        "余额不足",
        "额度不足",
        "账户余额不足",
    };
    for (const std::string_view pattern : patterns) {
        if (normalized.find(pattern) != std::string::npos) return true;
    }
    return false;
}

#ifdef _WIN32
std::string JoinPath(std::string_view left, std::string_view right) {
    if (left.empty()) {
        return std::string(right);
    }
    if (right.empty()) {
        return std::string(left);
    }
    std::string result(left);
    if (result.back() != '\\' && result.back() != '/') {
        result.push_back('\\');
    }
    result.append(right);
    return result;
}

std::string ParentPath(std::string_view path) {
    return WideToUtf8(SafePath(path).parent_path().wstring());
}

std::string AbsolutePath(std::string_view path, std::string_view base) {
    if (path.find('\0') != std::string_view::npos || base.find('\0') != std::string_view::npos ||
        !IsValidUtf8(path) || !IsValidUtf8(base)) return {};
    const std::wstring wide = Utf8ToWide(path);
    if (!path.empty() && wide.empty()) return {};
    spark::fs::path input(wide);
    if (input.is_relative()) {
        std::error_code error;
        const std::wstring wide_base = Utf8ToWide(base);
        if (!base.empty() && wide_base.empty()) return {};
        const spark::fs::path root = base.empty() ? spark::fs::current_path(error)
                                                      : spark::fs::path(wide_base);
        if (error) return {};
        input = root / input;
    }
    return WideToUtf8(input.lexically_normal().wstring());
}

std::string NormalizePathForComparison(std::string path) {
    if (path.empty() || path.find('\0') != std::string::npos || !IsValidUtf8(path)) return {};
    path = AbsolutePath(path);
    std::replace(path.begin(), path.end(), '/', '\\');
    while (path.size() > 3 && path.back() == '\\') {
        path.pop_back();
    }
    return LowerAscii(path);
}

std::string CanonicalPathForAccess(std::string_view path) {
    if (path.empty() || path.find('\0') != std::string_view::npos || !IsValidUtf8(path)) return {};
    try {
        const std::wstring wide = Utf8ToWide(path);
        if (wide.empty() || wide.size() >= 32768) return {};
        spark::fs::path input(wide);
        if (input.is_relative()) {
            std::error_code error;
            const auto directory = spark::fs::current_path(error);
            if (error) return {};
            input = directory / input;
        }
        input = input.lexically_normal();
        if (!input.is_absolute() || input.native().size() >= 32768) return {};

        spark::fs::path current = input;
        std::vector<spark::fs::path> missing_parts;
        for (;;) {
            const HANDLE raw = CreateFileW(current.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (raw != INVALID_HANDLE_VALUE) {
                const std::unique_ptr<void, decltype(&CloseHandle)> file(raw, &CloseHandle);
                if (!missing_parts.empty()) {
                    BY_HANDLE_FILE_INFORMATION info{};
                    if (!GetFileInformationByHandle(file.get(), &info) ||
                        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return {};
                }
                const auto canonical = windows_compat::FinalPathForHandle(file.get());
                if (!canonical) return {};
                spark::fs::path resolved(*canonical);
                for (auto iterator = missing_parts.rbegin(); iterator != missing_parts.rend(); ++iterator)
                    resolved /= *iterator;
                return WideToUtf8(resolved.lexically_normal().wstring());
            }
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return {};
            // An existing but unresolvable entry (including a dangling reparse
            // point) must not be treated as a missing, safe descendant.
            if (GetFileAttributesW(current.c_str()) != INVALID_FILE_ATTRIBUTES) return {};
            const DWORD attribute_error = GetLastError();
            if (attribute_error != ERROR_FILE_NOT_FOUND && attribute_error != ERROR_PATH_NOT_FOUND) return {};
            const spark::fs::path parent = current.parent_path();
            if (parent.empty() || parent == current) return {};
            missing_parts.push_back(current.filename());
            current = parent;
        }
    } catch (const spark::fs::filesystem_error&) {
        return {};
    }
}

bool PathExists(std::string_view path) {
    return GetFileAttributesW(Utf8ToWide(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory(std::string_view path) {
    const DWORD attributes = GetFileAttributesW(Utf8ToWide(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool IsPathWithin(std::string_view child, std::string_view parent) {
    const std::string canonical_child = CanonicalPathForAccess(child);
    const std::string canonical_parent = CanonicalPathForAccess(parent);
    if (canonical_child.empty() || canonical_parent.empty()) return false;
    const std::string normalized_child = NormalizePathForComparison(canonical_child);
    const std::string normalized_parent = NormalizePathForComparison(canonical_parent);
    if (normalized_child.empty() || normalized_parent.empty()) return false;
    if (normalized_child == normalized_parent) {
        return true;
    }
    const bool parent_has_separator = !normalized_parent.empty() && normalized_parent.back() == '\\';
    return normalized_child.size() > normalized_parent.size() &&
           normalized_child.compare(0, normalized_parent.size(), normalized_parent) == 0 &&
           (parent_has_separator || normalized_child[normalized_parent.size()] == '\\');
}

std::optional<std::string> ReadFileUtf8(std::string_view path, std::string* error) {
    HANDLE file = CreateFileW(Utf8ToWide(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = FormatSystemError(GetLastError());
        }
        return std::nullopt;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(file);
        if (error != nullptr) {
            *error = "file is too large or its size could not be read";
        }
        return std::nullopt;
    }
    std::string contents(static_cast<size_t>(size.QuadPart), '\0');
    DWORD total = 0;
    while (total < contents.size()) {
        DWORD read = 0;
        const DWORD requested = static_cast<DWORD>(std::min<size_t>(contents.size() - total, 1u << 20));
        if (!ReadFile(file, contents.data() + total, requested, &read, nullptr)) {
            if (error != nullptr) {
                *error = FormatSystemError(GetLastError());
            }
            CloseHandle(file);
            return std::nullopt;
        }
        if (read == 0) {
            break;
        }
        total += read;
    }
    CloseHandle(file);
    contents.resize(total);
    if (contents.size() >= 3 && static_cast<unsigned char>(contents[0]) == 0xEF &&
        static_cast<unsigned char>(contents[1]) == 0xBB && static_cast<unsigned char>(contents[2]) == 0xBF) {
        contents.erase(0, 3);
    }
    return contents;
}

bool WriteFileUtf8Atomic(std::string_view path, std::string_view contents, std::string* error) {
    const std::wstring target = Utf8ToWide(path);
    const std::wstring temporary = target + L".tmp." + std::to_wstring(GetCurrentProcessId());
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = FormatSystemError(GetLastError());
        }
        return false;
    }
    DWORD total = 0;
    while (total < contents.size()) {
        DWORD written = 0;
        const DWORD requested = static_cast<DWORD>(std::min<size_t>(contents.size() - total, 1u << 20));
        if (!WriteFile(file, contents.data() + total, requested, &written, nullptr)) {
            if (error != nullptr) {
                *error = FormatSystemError(GetLastError());
            }
            CloseHandle(file);
            DeleteFileW(temporary.c_str());
            return false;
        }
        total += written;
    }
    FlushFileBuffers(file);
    CloseHandle(file);
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (error != nullptr) {
            *error = FormatSystemError(GetLastError());
        }
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}
#endif

std::string WindowsTextFileContents(std::string_view utf8) {
    std::string normalized;
    normalized.reserve(utf8.size() + 3 + 16);
    normalized.append("\xEF\xBB\xBF");
    for (size_t index = 0; index < utf8.size(); ++index) {
        const char character = utf8[index];
        if (character == '\r') {
            normalized.push_back('\r');
            normalized.push_back('\n');
            if (index + 1 < utf8.size() && utf8[index + 1] == '\n') {
                ++index;
            }
        } else if (character == '\n') {
            normalized.push_back('\r');
            normalized.push_back('\n');
        } else {
            normalized.push_back(character);
        }
    }
    return normalized;
}

bool WriteWindowsTextFileAtomic(std::string_view path, std::string_view utf8, std::string* error) {
    const std::string contents = WindowsTextFileContents(utf8);
    return WriteFileUtf8Atomic(path, contents, error);
}

#ifdef _WIN32
std::string GetExecutablePath() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
        return {};
    }
    buffer.resize(length);
    return WideToUtf8(buffer);
}
#endif

std::string GetExecutableDirectory() {
    return ParentPath(GetExecutablePath());
}

#ifdef _WIN32
std::string CurrentDirectoryUtf8() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (length == 0) {
        return {};
    }
    buffer.resize(length);
    return WideToUtf8(buffer);
}

std::string EnvironmentVariable(std::string_view name) {
    if (name.empty()) return {};
    const std::wstring wide_name = Utf8ToWide(name);
    const DWORD size = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (size == 0) return {};
    std::wstring buffer(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(wide_name.c_str(), buffer.data(), size);
    if (written == 0 || written >= size) return {};
    buffer.resize(written);
    return WideToUtf8(buffer);
}

std::string FormatWin32Error(uint32_t error) {
    return FormatSystemError(error);
}
#endif

std::string RedactSecrets(std::string text, const std::vector<std::string>& secrets) {
    for (const std::string& secret : secrets) {
        if (secret.size() < 4) {
            continue;
        }
        size_t position = 0;
        while ((position = text.find(secret, position)) != std::string::npos) {
            text.replace(position, secret.size(), "[REDACTED]");
            position += 10;
        }
    }
    return text;
}

uint64_t UnixTimeSeconds() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count());
}

std::string IsoUtcTime() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

std::string JsonEscape(std::string_view value) {
    std::string output;
    output.reserve(value.size() + 16);
    for (const unsigned char character : value) {
        switch (character) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (character < 0x20) {
                    const char* hex = "0123456789abcdef";
                    output += "\\u00";
                    output.push_back(hex[(character >> 4) & 0x0F]);
                    output.push_back(hex[character & 0x0F]);
                } else {
                    output.push_back(static_cast<char>(character));
                }
        }
    }
    return output;
}

}  // namespace spark
