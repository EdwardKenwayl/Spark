#include "core/common.h"
#include "core/config.h"
#include "platform/http.h"

#include <commctrl.h>
#include <windows.h>

#include <string>

namespace {

constexpr int kProbeList = 100;
constexpr int kRunChecks = 101;
constexpr int kCloseProbe = 102;

HWND g_list = nullptr;
HMODULE g_module = nullptr;

std::wstring OSVersionText() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    const auto function = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(module, "RtlGetVersion"));
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (function != nullptr && function(&version) == 0) {
        return L"Windows " + std::to_wstring(version.dwMajorVersion) + L"." +
               std::to_wstring(version.dwMinorVersion) + L" build " + std::to_wstring(version.dwBuildNumber);
    }
    return L"Windows version unavailable";
}

void AddResult(const std::wstring& text) {
    const LRESULT index = SendMessageW(g_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(g_list, LB_SETTOPINDEX, static_cast<WPARAM>(index), 0);
}

void RunChecks(HWND window) {
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    AddResult(L"OS: " + OSVersionText());
    AddResult(L"CPU architecture: " + std::to_wstring(sizeof(void*) * 8) + L"-bit process");
    const auto executable = spark::Utf8ToWide(spark::GetExecutableDirectory());
    AddResult(L"Unicode path: " + executable);

    const std::string config_path = spark::JoinPath(spark::GetExecutableDirectory(), "probe-config.json");
    const auto config = spark::LoadConfig(config_path, true);
    AddResult(config.ok ? L"JSON/config load: PASS" : L"JSON/config load: FAIL");
    DeleteFileW(spark::Utf8ToWide(config_path).c_str());

    spark::HttpRequest request;
    request.method = "GET";
    request.url = "https://example.com/";
    request.timeout_seconds = 20;
    request.stream = false;
    size_t streamed = 0;
    const auto response = spark::HttpRequestStreaming(request, [&](const char*, size_t size) {
        streamed += size;
        return spark::StreamAction::Continue;
    });
    if (response.ok && response.value.status == 200 && streamed > 0) {
        AddResult(L"Verified HTTPS + streaming read: PASS (" + std::to_wstring(streamed) + L" bytes)");
    } else {
        AddResult(L"Verified HTTPS + streaming read: FAIL " + spark::Utf8ToWide(response.ok ? "HTTP error" : response.error));
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    PROCESS_INFORMATION process{};
    const wchar_t command[] = L"cmd.exe /D /S /C echo SparkPipeProbe";
    if (CreateProcessW(nullptr, const_cast<wchar_t*>(command), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                       &startup, &process)) {
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        AddResult(L"Subprocess creation: PASS");
    } else {
        AddResult(L"Subprocess creation: FAIL");
    }
    spark::CancellationToken cancellation;
    cancellation.Cancel();
    AddResult(cancellation.IsCancelled() ? L"Cancellation token: PASS" : L"Cancellation token: FAIL");
    AddResult(L"Close this probe with the Close button after recording the results.");
    EnableWindow(GetDlgItem(window, kRunChecks), TRUE);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE: {
            g_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
                                     16, 16, 760, 360, window, reinterpret_cast<HMENU>(kProbeList), g_module, nullptr);
            CreateWindowW(L"BUTTON", L"重新检测", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                          16, 390, 110, 32, window, reinterpret_cast<HMENU>(kRunChecks), g_module, nullptr);
            CreateWindowW(L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                          666, 390, 110, 32, window, reinterpret_cast<HMENU>(kCloseProbe), g_module, nullptr);
            SetTimer(window, 1, 100, nullptr);
            return 0;
        }
        case WM_TIMER:
            KillTimer(window, 1);
            RunChecks(window);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wparam) == kRunChecks) {
                RunChecks(window);
                return 0;
            }
            if (LOWORD(wparam) == kCloseProbe) {
                DestroyWindow(window);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    g_module = instance;
    INITCOMMONCONTROLSEX common_controls{sizeof(common_controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common_controls);
    const wchar_t* class_name = L"SparkWin7ProbeWindow";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = class_name;
    if (!RegisterClassExW(&window_class)) return 1;
    HWND window = CreateWindowExW(0, class_name, L"Spark Windows 7 compatibility probe", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 820, 480, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) return 1;
    ShowWindow(window, show_command);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
