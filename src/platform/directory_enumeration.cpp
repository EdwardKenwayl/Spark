#include "platform/directory_enumeration.h"

namespace spark {

HANDLE FindFirstDirectoryEntry(const std::wstring& pattern, WIN32_FIND_DATAW* data,
                               const FindFirstFileExCallback& find_first) {
    // Official Win32 values; older SDK headers may not name these Win7+ options.
    constexpr auto basic_info = static_cast<FINDEX_INFO_LEVELS>(1);  // FindExInfoBasic
    constexpr DWORD large_fetch = 2;  // FIND_FIRST_EX_LARGE_FETCH
    const HANDLE find = find_first(pattern.c_str(), basic_info, data, FindExSearchNameMatch, nullptr,
                                   large_fetch);
    if (find != INVALID_HANDLE_VALUE) return find;
    const DWORD error = GetLastError();
    if (error != ERROR_INVALID_PARAMETER && error != ERROR_INVALID_FLAGS &&
        error != ERROR_CALL_NOT_IMPLEMENTED && error != ERROR_NOT_SUPPORTED) return find;
    return find_first(pattern.c_str(), FindExInfoStandard, data, FindExSearchNameMatch, nullptr, 0);
}

}  // namespace spark
