#include "gui/window_geometry.h"
#include "core/common.h"
#ifdef _WIN32
#include "gui/window_geometry_windows.h"
#endif

#include <chrono>
#include <iostream>
#include <limits>

namespace {
int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << message << '\n'; }
}
bool Equal(spark::WindowSize size, int width, int height) {
    return size.width == width && size.height == height;
}
}  // namespace

int main() {
    using namespace spark;
    Check(Equal(InitialWindowSize({2560, 1440}, {980, 640}, 12), 2150, 1152),
          "100% startup uses screen coverage without the old 1360x780 cap");
    Check(Equal(RestoredWindowSize({1050, 650, 1.5, false}, 1, {2560, 1440}, {760, 520}, 12),
                1575, 975), "changing 150% to 100% retains the normal window's physical size");
    Check(Equal(RestoredWindowSize({1575, 975, 1, false}, 1.5, {1706, 960}, {760, 520}, 12),
                1050, 650), "changing 100% to 150% converts backing pixels once");
    Check(Equal(RestoredWindowSize({2150, 1152, 1, true}, 1, {800, 600}, {760, 520}, 12),
                788, 588), "saved dimensions are clamped after changing monitors");
    Check(Equal(InitialWindowSize({1, 1}, {980, 640}, 12), 1, 1),
          "even a tiny work area keeps positive bounded dimensions");
    Check(Equal(RestoredWindowSize({100, 100, 1, false}, 1, {1920, 1080}, {760, 520}, 12),
                760, 520), "small saved dimensions respect the usable minimum");
    Check(Equal(RestoredWindowSize({1000, 700, 1, false}, 0, {1920, 1080}, {760, 520}, 12),
                1612, 864), "an invalid current backing scale safely uses the screen default");
    const auto root = fs::temp_directory_path() /
        NativePath("spark-window-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto directory = PathToUtf8(root);
    const auto state_path = JoinPath(directory, "window-state.json");
    Check(!LoadWindowGeometry(directory), "a first launch does not create state or a data directory");
    Check(SaveWindowGeometry(directory, {1500, 900, 1.5, true}), "normal dimensions and maximization save");
    const auto loaded = LoadWindowGeometry(directory);
    Check(loaded && loaded->width == 1500 && loaded->height == 900 &&
              loaded->backing_scale == 1.5 && loaded->maximized,
          "a new process can read the exact persisted geometry");
    Check(WriteFileUtf8Atomic(JoinPath(directory, "config.json"), "private config") &&
              WriteFileUtf8Atomic(JoinPath(directory, "ui-state.json"), "existing chat state"),
          "prepare independent configuration and chat-state sentinels");
    Check(SaveWindowGeometry(directory, {1400, 850, 1, false}) &&
              ReadFileUtf8(JoinPath(directory, "config.json")).value_or("") == "private config" &&
              ReadFileUtf8(JoinPath(directory, "ui-state.json")).value_or("") == "existing chat state",
          "saving window state never rewrites config or existing chat state");
    for (const auto& contents : {std::string("not JSON"), std::string("[]"), std::string(4097, ' '),
             std::string(R"({"schema_version":1,"width":18446744073709551615,"height":700,"backing_scale":1,"maximized":false})"),
             std::string(R"({"schema_version":1,"width":1000,"height":700,"backing_scale":"bad","maximized":true})")}) {
        Check(WriteFileUtf8Atomic(state_path, contents) && !LoadWindowGeometry(directory),
              "malformed, oversized or out-of-range state is ignored");
    }
    Check(!SaveWindowGeometry(directory, {0, 900, 1, false}) &&
              !SaveWindowGeometry(directory, {1000, 900, std::numeric_limits<double>::infinity(), false}),
          "invalid dimensions and scales are never persisted");
#ifdef _WIN32
    HWND window = CreateWindowExW(0, L"STATIC", L"Synthetic geometry fixture", WS_OVERLAPPEDWINDOW,
        0, 0, 1000, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create an invisible Win32 placement fixture");
    if (window) {
        Check(SaveWindowsWindowGeometry(window, directory), "save Win32 normal placement");
        SetWindowPos(window, nullptr, 0, 0, 800, 600, SWP_NOACTIVATE | SWP_NOZORDER);
        Check(RestoreWindowsWindowGeometry(window, directory, SW_SHOWNORMAL, {1, 1}, 12) == SW_SHOWNORMAL,
              "restoring an ordinary window does not force maximization");
        RECT bounds{};
        GetWindowRect(window, &bounds);
        Check(bounds.right - bounds.left == 1000 && bounds.bottom - bounds.top == 700,
              "Win32 restores physical dimensions independently of system DPI");
        Check(SaveWindowGeometry(directory, {1000, 700, 1, true}) &&
                  RestoreWindowsWindowGeometry(window, directory, SW_SHOWNORMAL, {1, 1}, 12) == SW_SHOWMAXIMIZED &&
                  RestoreWindowsWindowGeometry(window, directory, SW_SHOWMINNOACTIVE, {1, 1}, 12) == SW_SHOWMINNOACTIVE,
              "saved maximization is restored while explicit minimized startup is preserved");
        DestroyWindow(window);
    }
#endif
    std::error_code error;
    fs::remove_all(root, error);
    return failures ? 1 : 0;
}
