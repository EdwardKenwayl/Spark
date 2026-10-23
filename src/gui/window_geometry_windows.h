#pragma once

#include "gui/window_geometry.h"

#include <windows.h>

namespace spark {

inline RECT WindowsWindowWorkArea(HWND window) {
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
        return monitor.rcWork;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    return work;
}

inline int RestoreWindowsWindowGeometry(HWND window, const std::string& data_directory,
    int show_command, WindowSize minimum, int margin) {
    const auto saved = LoadWindowGeometry(data_directory);
    if (!saved) return show_command;
    const auto work = WindowsWindowWorkArea(window);
    // DPI-aware Win32 placement already uses physical screen pixels.
    const auto size = RestoredWindowSize(*saved, 1,
        {work.right - work.left, work.bottom - work.top}, minimum, margin);
    SetWindowPos(window, nullptr, work.left + (work.right - work.left - size.width) / 2,
        work.top + (work.bottom - work.top - size.height) / 2, size.width, size.height,
        SWP_NOACTIVATE | SWP_NOZORDER);
    if (saved->maximized && (show_command == SW_SHOWNORMAL || show_command == SW_SHOW ||
                            show_command == SW_SHOWDEFAULT))
        return SW_SHOWMAXIMIZED;
    return show_command;
}

inline bool SaveWindowsWindowGeometry(HWND window, const std::string& data_directory) {
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (!GetWindowPlacement(window, &placement)) return false;
    const auto& normal = placement.rcNormalPosition;
    return SaveWindowGeometry(data_directory, {normal.right - normal.left, normal.bottom - normal.top,
        1, IsZoomed(window) != FALSE ||
            (IsIconic(window) && (placement.flags & WPF_RESTORETOMAXIMIZED) != 0)});
}

}  // namespace spark
