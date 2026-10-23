#include "platform/directory_enumeration.h"

#include <iostream>

int main() {
    struct Case {
        DWORD first_error;
        DWORD retry_error;
        int expected_calls;
    };
    const Case cases[] = {
        {ERROR_SUCCESS, ERROR_SUCCESS, 1},
        {ERROR_INVALID_PARAMETER, ERROR_SUCCESS, 2},
        {ERROR_INVALID_FLAGS, ERROR_SUCCESS, 2},
        {ERROR_CALL_NOT_IMPLEMENTED, ERROR_SUCCESS, 2},
        {ERROR_NOT_SUPPORTED, ERROR_SUCCESS, 2},
        {ERROR_INVALID_PARAMETER, ERROR_ACCESS_DENIED, 2},
        {ERROR_INVALID_FLAGS, ERROR_FILE_NOT_FOUND, 2},
        {ERROR_CALL_NOT_IMPLEMENTED, ERROR_NOT_SUPPORTED, 2},
        {ERROR_NOT_SUPPORTED, ERROR_PATH_NOT_FOUND, 2},
        {ERROR_ACCESS_DENIED, ERROR_SUCCESS, 1},
        {ERROR_FILE_NOT_FOUND, ERROR_SUCCESS, 1},
        {ERROR_PATH_NOT_FOUND, ERROR_SUCCESS, 1},
        {ERROR_NO_MORE_FILES, ERROR_SUCCESS, 1},
        {ERROR_INVALID_NAME, ERROR_SUCCESS, 1},
    };
    int failures = 0;
    for (const auto& test : cases) {
        const auto check = [&](bool condition, const char* message) {
            if (!condition) {
                ++failures;
                std::cerr << "FAIL (" << test.first_error << ", " << test.retry_error << "): "
                          << message << '\n';
            }
        };
        const std::wstring pattern = L"C:\\fixture-\u6d4b\u8bd5\\*";
        WIN32_FIND_DATAW data{};
        const HANDLE expected_handle = reinterpret_cast<HANDLE>(&data);
        int calls = 0;
        const spark::FindFirstFileExCallback find_first =
            [&](LPCWSTR name, FINDEX_INFO_LEVELS level, LPVOID output, FINDEX_SEARCH_OPS search,
                LPVOID filter, DWORD flags) -> HANDLE {
                ++calls;
                check(name == pattern.c_str() && output == &data, "pattern and output are forwarded");
                check(search == FindExSearchNameMatch && filter == nullptr, "search options are unchanged");
                // Official values: FindExInfoBasic = 1, FIND_FIRST_EX_LARGE_FETCH = 2.
                check(static_cast<int>(level) == (calls == 1 ? 1 : 0) &&
                          flags == (calls == 1 ? 2u : 0u), "fast or legacy options match the call");
                const DWORD error = calls == 1 ? test.first_error : test.retry_error;
                if (error == ERROR_SUCCESS) {
                    data.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
                    // A stale compatibility error must never trigger another call after success.
                    SetLastError(ERROR_NOT_SUPPORTED);
                    return expected_handle;
                }
                SetLastError(error);
                return INVALID_HANDLE_VALUE;
            };
        const HANDLE result = spark::FindFirstDirectoryEntry(pattern, &data, find_first);
        const DWORD error = GetLastError();
        const DWORD expected_error = test.expected_calls == 1 ? test.first_error : test.retry_error;
        check(calls == test.expected_calls, "exact call count (no retry after success or legacy failure)");
        check(result == (expected_error == ERROR_SUCCESS ? expected_handle : INVALID_HANDLE_VALUE),
              "the selected API result is returned unchanged");
        if (expected_error == ERROR_SUCCESS) {
            check(data.dwFileAttributes == FILE_ATTRIBUTE_DIRECTORY, "successful output is preserved");
        } else {
            check(error == expected_error, "the selected API error is preserved");
        }
    }
    if (failures == 0) std::cout << "Directory enumeration compatibility checks passed.\n";
    return failures == 0 ? 0 : 1;
}
