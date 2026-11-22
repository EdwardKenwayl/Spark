#pragma once

#include <cstddef>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

class wxWindow;

namespace spark { namespace ui {

inline bool IsAttachmentPath(std::string_view path) {
    const bool drive = path.size() >= 3 &&
        ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && (path[2] == '\\' || path[2] == '/');
    return drive || (!path.empty() && path[0] == '/') ||
        (path.size() >= 2 && path[0] == '\\' && path[1] == '\\');
}

// Decode generated references for display/editing; journal entries stay unchanged.
inline std::string UserMessageText(std::string_view source, std::vector<std::string>* attachments = nullptr) {
    constexpr std::string_view marker = u8"[附件引用]";
    const auto start = source.find(marker);
    if (start == std::string_view::npos) return std::string(source);
    if (start != 0) {
        const bool snapshot = source.substr(0, std::string_view(u8"[智能快照]\n").size()) == u8"[智能快照]\n" ||
            source.substr(0, std::string_view(u8"[智能快照]\r\n").size()) == u8"[智能快照]\r\n";
        const bool boundary = (start >= 2 && source.substr(start - 2, 2) == "\n\n") ||
            (start >= 4 && source.substr(start - 4, 4) == "\r\n\r\n");
        if (!snapshot || !boundary || start > 8192) return std::string(source);
    }
    const auto line = [&](size_t pos, size_t* next) {
        const auto end = source.find('\n', pos);
        *next = end == std::string_view::npos ? source.size() : end + 1;
        auto value = source.substr(pos, (end == std::string_view::npos ? source.size() : end) - pos);
        if (!value.empty() && value.back() == '\r') value.remove_suffix(1);
        return value;
    };
    size_t pos = start, next = 0;
    if (line(pos, &next) != marker) return std::string(source);
    pos = next;
    size_t paths = 0;
    std::vector<std::string> references;
    while (pos < source.size()) {
        const auto value = line(pos, &next);
        if (value.empty()) {
            if (paths) {
                if (attachments) for (const auto& path : references) {
                    if (std::find(attachments->begin(), attachments->end(), path) == attachments->end())
                        attachments->push_back(path);
                }
                return std::string(source.substr(0, start)) + std::string(source.substr(next));
            }
            break;
        }
        if (value.size() < 3 || value.substr(0, 2) != "- " || !IsAttachmentPath(value.substr(2))) break;
        if (attachments) references.emplace_back(value.substr(2));
        ++paths;
        pos = next;
    }
    return std::string(source);
}

// Markdown source locations are not part of the on-disk filename. Call before URI decoding.
inline std::wstring FileLinkTarget(std::wstring target) {
    const auto anchor = target.rfind(L"#L");
    if (anchor != std::wstring::npos && anchor + 2 < target.size()) {
        bool location = true;
        for (size_t i = anchor + 2; i < target.size(); ++i)
            if ((target[i] < L'0' || target[i] > L'9') && target[i] != L'-' && target[i] != L'L') location = false;
        if (location && target[anchor + 2] >= L'0' && target[anchor + 2] <= L'9') target.resize(anchor);
    }
    for (int part = 0; part < 2; ++part) {
        const auto colon = target.rfind(L':');
        if (colon == std::wstring::npos || colon <= 1 || colon + 1 == target.size()) break;
        const auto suffix = target.substr(colon + 1);
        if (suffix.find_first_not_of(L"0123456789") != std::wstring::npos) break;
        target.resize(colon);
    }
    return target;
}

inline std::string FileUri(std::string_view path) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string uri = "file://";
    for (const unsigned char c : path) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
            uri.push_back(static_cast<char>(c));
        else { uri += '%'; uri += hex[c >> 4]; uri += hex[c & 15]; }
    }
    return uri;
}

enum class FileRevealPlatform { Linux, MacOS };
inline std::vector<std::string> FileRevealCommand(FileRevealPlatform platform, const std::string& path) {
    if (path.empty() || path.find('\0') != std::string::npos) return {};
    if (platform == FileRevealPlatform::Linux)
        return {"dbus-send", "--session", "--type=method_call", "--dest=org.freedesktop.FileManager1",
            "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1.ShowItems",
            "array:string:" + FileUri(path), "string:"};
    return {"open", "-R", "--", path};
}

#ifdef _WIN32
inline bool RevealFileInManager(HWND owner, const std::wstring& path) {
    if (path.empty() || path.find(L'\0') != std::wstring::npos || path.find(L'"') != std::wstring::npos) return false;
    const HRESULT initialized = CoInitialize(nullptr);
    bool selected = false;
    HMODULE shell = LoadLibraryW(L"shell32.dll");
    if (shell) {
        using Parse = HRESULT (WINAPI*)(LPCWSTR, IBindCtx*, LPITEMIDLIST*, SFGAOF, SFGAOF*);
        using Select = HRESULT (WINAPI*)(LPCITEMIDLIST, UINT, LPCITEMIDLIST*, DWORD);
        const auto parse = reinterpret_cast<Parse>(GetProcAddress(shell, "SHParseDisplayName"));
        const auto select = reinterpret_cast<Select>(GetProcAddress(shell, "SHOpenFolderAndSelectItems"));
        LPITEMIDLIST item = nullptr;
        if (parse && select && SUCCEEDED(parse(path.c_str(), nullptr, &item, 0, nullptr)))
            selected = SUCCEEDED(select(item, 0, nullptr, 0));
        if (item) CoTaskMemFree(item);
        FreeLibrary(shell);
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (selected) return true;
    const std::wstring arguments = L"/select,\"" + path + L"\"";
    return reinterpret_cast<INT_PTR>(ShellExecuteW(owner, L"open", L"explorer.exe", arguments.c_str(),
        nullptr, SW_SHOWNORMAL)) > 32;
}
#endif

// Native link metadata is owned by its wxWindow; no transcript or model state is involved.
void ClearNativeMarkdownLinks(wxWindow* owner, size_t item = static_cast<size_t>(-1));
std::wstring NativeMarkdownLinkAt(wxWindow* owner, size_t item, int x, int y, int row_anchor_y);

} }  // namespace spark::ui
