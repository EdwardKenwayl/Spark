#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace spark {

using FindFirstFileExCallback =
    std::function<HANDLE(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD)>;

// The caller owns successful handles; failure preserves the selected API call's last error.
HANDLE FindFirstDirectoryEntry(const std::wstring& pattern, WIN32_FIND_DATAW* data,
                               const FindFirstFileExCallback& find_first = ::FindFirstFileExW);

}  // namespace spark
