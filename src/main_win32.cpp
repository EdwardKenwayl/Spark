#include "gui/app.h"
#include "core/common.h"
#include "platform/windows_compat.h"

#include <windows.h>
#include <dbghelp.h>

namespace {

LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS* crash_context) {
    wchar_t executable_path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable_path,
                                            static_cast<DWORD>(std::size(executable_path)));
    if (length == 0 || length >= std::size(executable_path)) return EXCEPTION_EXECUTE_HANDLER;

    std::wstring directory(executable_path, length);
    const size_t slash = directory.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return EXCEPTION_EXECUTE_HANDLER;
    directory.resize(slash);
    const std::wstring data_directory = directory + L"\\data";
    const std::wstring dump_directory = data_directory + L"\\crash-dumps";
    CreateDirectoryW(data_directory.c_str(), nullptr);
    CreateDirectoryW(dump_directory.c_str(), nullptr);

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t file_name[128]{};
    swprintf_s(file_name, L"crash-%04u%02u%02u-%02u%02u%02u-%lu.dmp", now.wYear, now.wMonth, now.wDay,
               now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId());
    const std::wstring dump_path = dump_directory + L"\\" + file_name;
    HANDLE file = CreateFileW(dump_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return EXCEPTION_EXECUTE_HANDLER;

    MINIDUMP_EXCEPTION_INFORMATION information{};
    information.ThreadId = GetCurrentThreadId();
    information.ExceptionPointers = crash_context;
    information.ClientPointers = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, &information,
                      nullptr, nullptr);
    CloseHandle(file);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    if (const auto helper = spark::RunSnapshotHelper()) return *helper;
    SetUnhandledExceptionFilter(WriteCrashDump);
    spark::windows_compat::EnableProcessDpiAwareness();
    HANDLE single_instance = CreateMutexW(nullptr, FALSE, spark::kAppMutexName);
    if (single_instance == nullptr) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = nullptr;
        for (int attempt = 0; attempt < 40 && existing == nullptr; ++attempt) {
            existing = FindWindowW(spark::kAppWindowClass, nullptr);
            if (existing == nullptr) Sleep(50);
        }
        if (existing != nullptr) {
            PostMessageW(existing, spark::kSingleInstanceMessage, 0, 0);
            SetForegroundWindow(existing);
        }
        CloseHandle(single_instance);
        return 0;
    }
    spark::AppWindow app;
    const int result = app.Run(instance, show_command);
    CloseHandle(single_instance);
    return result;
}
