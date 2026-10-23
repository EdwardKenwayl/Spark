#pragma once

#include <wx/display.h>
#include <wx/frame.h>
#include <wx/settings.h>

#include <algorithm>

namespace spark {

struct NativeStartupGeometry {
    wxRect bounds;
    wxSize minimum;
};

inline NativeStartupGeometry NativeStartupGeometryFor(const wxWindow& window, const wxRect& work) {
    const auto lower = window.FromDIP(wxSize(980, 640));
    const auto upper = window.FromDIP(wxSize(1360, 780));
    const auto minimum = window.FromDIP(wxSize(760, 520));
    const int margin = window.FromDIP(12);
    const wxSize available(std::max(1, work.width - margin),
                           std::max(1, work.height - margin));
    // Work-area coordinates are already logical pixels, including on Retina.
    const wxSize size(
        std::min(available.x, std::clamp(static_cast<int>(work.width * 84LL / 100), lower.x, upper.x)),
        std::min(available.y, std::clamp(static_cast<int>(work.height * 80LL / 100), lower.y, upper.y)));
    return {
        wxRect(work.x + (work.width - size.x) / 2, work.y + (work.height - size.y) / 2, size.x, size.y),
        wxSize(std::min(minimum.x, size.x), std::min(minimum.y, size.y))
    };
}

inline wxFont NativeStartupFont(wxFont system_font) {
    if (!system_font.IsOk()) return wxFont(wxFontInfo(10));
    // Point sizes already follow toolkit DPI and desktop text-size preferences.
    if (system_font.GetFractionalPointSize() < 10)
        system_font.SetFractionalPointSize(10);
    return system_font;
}

inline void ApplyNativeStartupDpi(wxFrame& frame) {
    frame.SetFont(NativeStartupFont(wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT)));
    const wxDisplay display(&frame);
    wxRect work;
    if (display.IsOk()) {
        work = display.GetClientArea();
        if (work.IsEmpty()) work = display.GetGeometry();
    }
    if (work.IsEmpty()) {
        frame.SetMinSize(frame.FromDIP(wxSize(760, 520)));
        frame.SetSize(frame.FromDIP(wxSize(1220, 820)));
        return;
    }
    const auto geometry = NativeStartupGeometryFor(frame, work);
    frame.SetMinSize(geometry.minimum);
    frame.SetSize(geometry.bounds);
}

}  // namespace spark
