#include "platform/windows_compat.h"

#ifdef _WIN32
#include "platform/user_directories.h"

#include <shlobj.h>

#include <climits>
#include <cstdint>
#include <cwchar>
#include <exception>
#include <memory>
#include <vector>

namespace spark::windows_compat {
namespace {

// Keep system modules loaded for the lifetime of their cached function pointers.
HMODULE LoadSystemModule(const wchar_t* name) noexcept {
    wchar_t path[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return nullptr;
    const size_t name_length = std::wcslen(name);
    if (length + 1 + name_length >= MAX_PATH) return nullptr;
    path[length] = L'\\';
    for (size_t index = 0; index <= name_length; ++index) path[length + 1 + index] = name[index];
    return LoadLibraryW(path);
}

using SetProcessDpiAwareFn = BOOL(WINAPI*)();
using GetTickCount64Fn = ULONGLONG(WINAPI*)();
using LcidToLocaleNameFn = int(WINAPI*)(LCID, LPWSTR, int, DWORD);
using GetUserDefaultLocaleNameFn = int(WINAPI*)(LPWSTR, int);
using GetFinalPathNameByHandleFn = DWORD(WINAPI*)(HANDLE, LPWSTR, DWORD, DWORD);
using GetKnownFolderPathFn = HRESULT(WINAPI*)(const GUID&, DWORD, HANDLE, PWSTR*);
using FreeTaskMemoryFn = void(WINAPI*)(void*);

// Local SDK identifiers avoid adding a uuid.lib dependency to spark_foundation.
constexpr GUID kDocuments{0xFDD39AD0, 0x238F, 0x46AF, {0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7}};
constexpr GUID kRoamingAppData{0x3EB685DB, 0x65F9, 0x4CF6, {0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D}};
constexpr GUID kLocalAppData{0xF1B32785, 0x6FBA, 0x4FCF, {0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91}};
constexpr DWORD kPathCapacity = 32768;

#ifdef SPARK_WINDOWS_XP
std::optional<std::wstring> LegacyDiskPath(HANDLE file) {
    if (GetFileType(file) != FILE_TYPE_DISK) return std::nullopt;
    struct NativeString { USHORT length, maximum; PWSTR data; };
    using QueryObject = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<QueryObject>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject"));
    if (!query) return std::nullopt;
    std::vector<unsigned char> bytes(sizeof(NativeString) + kPathCapacity * sizeof(wchar_t));
    ULONG used = 0;
    if (query(file, 1, bytes.data(), static_cast<ULONG>(bytes.size()), &used) < 0) return std::nullopt;
    const auto* name = reinterpret_cast<const NativeString*>(bytes.data());
    const auto begin = reinterpret_cast<uintptr_t>(bytes.data());
    const auto address = reinterpret_cast<uintptr_t>(name->data);
    if (!name->data || name->length == 0 || name->length % sizeof(wchar_t) ||
        address < begin + sizeof(NativeString) || address > begin + bytes.size() ||
        name->length > begin + bytes.size() - address) return std::nullopt;
    const std::wstring path(name->data, name->length / sizeof(wchar_t));
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
        wchar_t drive[] = {letter, L':', L'\0'};
        wchar_t target[kPathCapacity]{};
        if (!QueryDosDeviceW(drive, target, kPathCapacity)) continue;
        const size_t length = std::wcslen(target);
        if (path.size() >= length && _wcsnicmp(path.c_str(), target, length) == 0 &&
            (path.size() == length || path[length] == L'\\'))
            return std::wstring(drive) + (path.size() == length ? L"\\" : path.substr(length));
    }
    const std::wstring prefix = L"\\Device\\Mup\\";
    if (path.compare(0, prefix.size(), prefix) == 0) return L"\\\\" + path.substr(prefix.size());
    return std::nullopt;
}
#endif

}  // namespace

void EnableProcessDpiAwareness() noexcept {
    static const HMODULE module = LoadSystemModule(L"user32.dll");
    static const auto set_awareness = module
        ? reinterpret_cast<SetProcessDpiAwareFn>(GetProcAddress(module, "SetProcessDPIAware")) : nullptr;
    if (set_awareness) set_awareness();
}

uint64_t TickMilliseconds() noexcept {
    static const HMODULE module = GetModuleHandleW(L"kernel32.dll");
    static const auto get_tick_count64 = module
        ? reinterpret_cast<GetTickCount64Fn>(GetProcAddress(module, "GetTickCount64")) : nullptr;
    if (get_tick_count64) return static_cast<uint64_t>(get_tick_count64());

    struct TickState {
        HANDLE mutex;
        DWORD previous;
        uint64_t extended;
        bool sampled;
    };
    // Keep the unnamed XP mutex until process exit, including late shutdown calls.
    static TickState state{CreateMutexW(nullptr, FALSE, nullptr), 0, 0, false};
    if (!state.mutex || WaitForSingleObject(state.mutex, INFINITE) != WAIT_OBJECT_0)
        std::terminate();

    // Sample under the mutex: out-of-order samples could otherwise look like wraps.
    const DWORD sample = GetTickCount();
    if (state.sampled) state.extended += static_cast<DWORD>(sample - state.previous);
    else state.extended = sample;
    state.previous = sample;
    state.sampled = true;
    const uint64_t result = state.extended;
    if (!ReleaseMutex(state.mutex)) std::terminate();
    return result;
}

std::wstring LocaleName() {
    static const HMODULE module = GetModuleHandleW(L"kernel32.dll");
    static const auto lcid_to_name = module
        ? reinterpret_cast<LcidToLocaleNameFn>(GetProcAddress(module, "LCIDToLocaleName")) : nullptr;
    static const auto user_locale_name = module
        ? reinterpret_cast<GetUserDefaultLocaleNameFn>(GetProcAddress(module, "GetUserDefaultLocaleName")) : nullptr;
    const LANGID language = GetUserDefaultUILanguage();
    // LOCALE_NAME_MAX_LENGTH is absent from XP SDK headers.
    constexpr int capacity = 85;
    wchar_t name[capacity]{};
    if (lcid_to_name) {
        const int length = lcid_to_name(MAKELCID(language, SORT_DEFAULT), name, capacity, 0);
        if (length > 1 && length <= capacity && name[length - 1] == L'\0')
            return std::wstring(name, static_cast<size_t>(length - 1));
    }
    if (user_locale_name) {
        const int length = user_locale_name(name, capacity);
        if (length > 1 && length <= capacity && name[length - 1] == L'\0')
            return std::wstring(name, static_cast<size_t>(length - 1));
    }

    switch (PRIMARYLANGID(language)) {
        case LANG_CHINESE:
            switch (SUBLANGID(language)) {
                case SUBLANG_CHINESE_TRADITIONAL:
                case SUBLANG_CHINESE_HONGKONG:
                case SUBLANG_CHINESE_MACAU:
                case 0x1f:  // Neutral zh-Hant; absent from older SDKs.
                    return L"zh-TW";
                default: return L"zh-CN";
            }
        case LANG_ENGLISH: return L"en-US";
        case LANG_JAPANESE: return L"ja-JP";
        case LANG_KOREAN: return L"ko-KR";
        case LANG_GERMAN: return L"de-DE";
        case LANG_FRENCH: return L"fr-FR";
        case LANG_SPANISH: return L"es-ES";
        case LANG_PORTUGUESE: return L"pt-BR";
        case LANG_RUSSIAN: return L"ru-RU";
        default: return L"en-US";
    }
}

std::optional<std::wstring> FinalPathForHandle(HANDLE file) {
    if (file == nullptr || file == INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return std::nullopt;
    }
    static const HMODULE module = GetModuleHandleW(L"kernel32.dll");
    static const auto get_final_path = module
        ? reinterpret_cast<GetFinalPathNameByHandleFn>(GetProcAddress(module, "GetFinalPathNameByHandleW")) : nullptr;
    if (!get_final_path) {
#ifdef SPARK_WINDOWS_XP
        // Resolve the kernel's object name; never authorize a lexical substitute.
        if (const auto path = LegacyDiskPath(file)) return path;
#endif
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return std::nullopt;
    }
    // Zero requests FILE_NAME_NORMALIZED | VOLUME_NAME_DOS, even with XP SDK headers.
    DWORD capacity = get_final_path(file, nullptr, 0, 0);
    if (capacity == 0) return std::nullopt;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        if (capacity > kPathCapacity) {
            SetLastError(ERROR_FILENAME_EXCED_RANGE);
            return std::nullopt;
        }
        std::wstring path(capacity, L'\0');
        const DWORD length = get_final_path(file, path.data(), capacity, 0);
        if (length == 0) return std::nullopt;
        if (length >= capacity) {
            if (length >= kPathCapacity) break;
            capacity = length + 1;
            continue;
        }
        path.resize(length);
        if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) path = L"\\\\" + path.substr(8);
        else if (path.rfind(L"\\\\?\\", 0) == 0) path.erase(0, 4);
        const bool drive_absolute = path.size() >= 3 &&
            ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
            path[1] == L':' && path[2] == L'\\';
        const size_t share = path.rfind(L"\\\\", 0) == 0 ? path.find(L'\\', 2) : std::wstring::npos;
        const bool unc_absolute = share != std::wstring::npos && share > 2 &&
            share + 1 < path.size() && path[share + 1] != L'\\' && path[2] != L'.' && path[2] != L'?';
        if ((!drive_absolute && !unc_absolute) || path.find(L'\0') != std::wstring::npos) {
            SetLastError(ERROR_INVALID_NAME);
            return std::nullopt;
        }
        return path;
    }
    SetLastError(ERROR_RETRY);
    return std::nullopt;
}

std::optional<std::wstring> UserDirectoryPath(UserDirectoryKind kind) {
    const GUID* folder = nullptr;
    int csidl = 0;
    switch (kind) {
        case UserDirectoryKind::Documents: folder = &kDocuments; csidl = CSIDL_PERSONAL; break;
        case UserDirectoryKind::AppData: folder = &kRoamingAppData; csidl = CSIDL_APPDATA; break;
        case UserDirectoryKind::LocalAppData: folder = &kLocalAppData; csidl = CSIDL_LOCAL_APPDATA; break;
    }
    if (!folder) return std::nullopt;
    static const HMODULE shell = LoadSystemModule(L"shell32.dll");
    static const HMODULE ole = LoadSystemModule(L"ole32.dll");
    static const auto get_known_folder = shell
        ? reinterpret_cast<GetKnownFolderPathFn>(GetProcAddress(shell, "SHGetKnownFolderPath")) : nullptr;
    static const auto free_memory = ole
        ? reinterpret_cast<FreeTaskMemoryFn>(GetProcAddress(ole, "CoTaskMemFree")) : nullptr;
    if (get_known_folder && free_memory) {
        PWSTR path = nullptr;
        const HRESULT result = get_known_folder(*folder, 0, nullptr, &path);
        const std::unique_ptr<wchar_t, FreeTaskMemoryFn> memory(path, free_memory);
        if (SUCCEEDED(result) && path && *path) return std::wstring(path);
    }
    wchar_t path[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, path)) && *path)
        return std::wstring(path);
    return std::nullopt;
}

bool IsValidUtf8(std::string_view value) noexcept {
    size_t index = 0;
    while (index < value.size()) {
        const auto lead = static_cast<unsigned char>(value[index++]);
        if (lead < 0x80) continue;
        unsigned extra = 0;
        uint32_t scalar = 0;
        uint32_t minimum = 0;
        if (lead >= 0xC2 && lead <= 0xDF) { extra = 1; scalar = lead & 0x1F; minimum = 0x80; }
        else if (lead >= 0xE0 && lead <= 0xEF) { extra = 2; scalar = lead & 0x0F; minimum = 0x800; }
        else if (lead >= 0xF0 && lead <= 0xF4) { extra = 3; scalar = lead & 0x07; minimum = 0x10000; }
        else return false;
        if (value.size() - index < extra) return false;
        for (unsigned count = 0; count < extra; ++count) {
            const auto byte = static_cast<unsigned char>(value[index++]);
            if ((byte & 0xC0) != 0x80) return false;
            scalar = (scalar << 6) | (byte & 0x3F);
        }
        if (scalar < minimum || scalar > 0x10FFFF || (scalar >= 0xD800 && scalar <= 0xDFFF)) return false;
    }
    return true;
}

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty() || value.size() > INT_MAX) return {};
    for (size_t index = 0; index < value.size(); ++index) {
        const auto unit = static_cast<uint16_t>(value[index]);
        if (unit >= 0xDC00 && unit <= 0xDFFF) return {};
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (++index == value.size() || value[index] < 0xDC00 || value[index] > 0xDFFF) return {};
        }
    }
    DWORD flags = 0x80;  // WC_ERR_INVALID_CHARS; unavailable on XP.
    const int length = static_cast<int>(value.size());
    int required = WideCharToMultiByte(CP_UTF8, flags, value.data(), length, nullptr, 0, nullptr, nullptr);
    if (required == 0 && GetLastError() == ERROR_INVALID_FLAGS) {
        // UTF-16 was validated above, so XP cannot replace isolated surrogates.
        flags = 0;
        required = WideCharToMultiByte(CP_UTF8, flags, value.data(), length, nullptr, 0, nullptr, nullptr);
    }
    if (required <= 0) return {};
    std::string output(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, flags, value.data(), length, output.data(), required, nullptr, nullptr) != required)
        return {};
    return output;
}

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty() || value.size() > INT_MAX || !IsValidUtf8(value)) return {};
    const int length = static_cast<int>(value.size());
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
    if (required <= 0) return {};
    std::wstring output(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, output.data(), required) != required)
        return {};
    return output;
}

}  // namespace spark::windows_compat
#endif
