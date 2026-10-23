#pragma once

#include <windows.h>
#include <winreg.h>

#include <string>

namespace spark::theme {

struct Palette {
    COLORREF background = RGB(28, 29, 31);
    COLORREF sidebar = RGB(37, 38, 40);
    COLORREF panel = RGB(43, 44, 47);
    COLORREF input = RGB(48, 49, 52);
    COLORREF selection = RGB(61, 62, 66);
    COLORREF border = RGB(67, 68, 72);
    COLORREF text = RGB(236, 237, 240);
    COLORREF muted = RGB(150, 153, 160);
    COLORREF subtle = RGB(112, 115, 122);
    COLORREF accent = RGB(91, 99, 112);
    COLORREF accent_pressed = RGB(108, 117, 132);
    COLORREF error = RGB(242, 128, 128);
    COLORREF warning = RGB(222, 146, 75);
    COLORREF success = RGB(111, 190, 140);
    COLORREF rail_idle = RGB(74, 76, 82);
    COLORREF rail_hover = RGB(244, 245, 248);
};

inline bool SystemUsesLightTheme() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LSTATUS status = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                                           reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_DWORD && size == sizeof(value) && value != 0;
}

inline bool IsDark(const std::string& preference) {
    if (preference == "light") return false;
    if (preference == "system") return !SystemUsesLightTheme();
    return true;
}

inline Palette DarkPalette() {
    Palette palette;
    palette.background = RGB(28, 29, 31);
    palette.sidebar = RGB(37, 38, 40);
    palette.panel = RGB(43, 44, 47);
    palette.input = RGB(48, 49, 52);
    palette.selection = RGB(61, 62, 66);
    palette.border = RGB(67, 68, 72);
    palette.text = RGB(236, 237, 240);
    palette.muted = RGB(150, 153, 160);
    palette.subtle = RGB(112, 115, 122);
    palette.accent = RGB(91, 99, 112);
    palette.accent_pressed = RGB(108, 117, 132);
    palette.error = RGB(242, 128, 128);
    palette.warning = RGB(222, 146, 75);
    palette.success = RGB(111, 190, 140);
    palette.rail_idle = RGB(74, 76, 82);
    palette.rail_hover = RGB(244, 245, 248);
    return palette;
}

inline Palette LightPalette() {
    Palette palette;
    palette.background = RGB(250, 250, 250);
    palette.sidebar = RGB(242, 242, 243);
    palette.panel = RGB(255, 255, 255);
    palette.input = RGB(255, 255, 255);
    palette.selection = RGB(231, 232, 234);
    palette.border = RGB(214, 216, 219);
    palette.text = RGB(31, 32, 35);
    palette.muted = RGB(103, 106, 112);
    palette.subtle = RGB(135, 138, 144);
    palette.accent = RGB(70, 78, 92);
    palette.accent_pressed = RGB(49, 56, 68);
    palette.error = RGB(190, 54, 54);
    palette.warning = RGB(171, 99, 25);
    palette.success = RGB(39, 132, 82);
    palette.rail_idle = RGB(180, 183, 188);
    palette.rail_hover = RGB(72, 74, 79);
    return palette;
}

inline Palette For(const std::string& preference) {
    return IsDark(preference) ? DarkPalette() : LightPalette();
}

}  // namespace spark::theme
