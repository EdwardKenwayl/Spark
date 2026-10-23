#pragma once

#include "gui/window_geometry.h"

#include <wx/display.h>
#include <wx/frame.h>
#include <wx/settings.h>

#include <algorithm>
#include <functional>
#include <memory>

namespace spark {

struct NativeStartupGeometry {
    wxRect bounds;
    wxSize minimum;
};

inline NativeStartupGeometry NativeStartupGeometryFor(const wxWindow& window, const wxRect& work,
    const std::optional<SavedWindowGeometry>& saved = std::nullopt) {
    const auto lower = window.FromDIP(wxSize(980, 640));
    const auto minimum = window.FromDIP(wxSize(760, 520));
    const int margin = window.FromDIP(12);
    const WindowSize available{work.width, work.height};
    const auto measured = saved
        ? RestoredWindowSize(*saved, window.GetContentScaleFactor(), available,
                             {minimum.x, minimum.y}, margin)
        : InitialWindowSize(available, {lower.x, lower.y}, margin);
    const wxSize size(measured.width, measured.height);
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

inline void ApplyNativeStartupDpi(wxFrame& frame,
    const std::optional<SavedWindowGeometry>& saved = std::nullopt) {
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
    const auto geometry = NativeStartupGeometryFor(frame, work, saved);
    frame.SetMinSize(geometry.minimum);
    frame.SetSize(geometry.bounds);
}

inline void UpdateNativeWindowGeometry(wxFrame& frame, SavedWindowGeometry& state) {
    if (frame.IsIconized()) return;
    state.maximized = frame.IsMaximized();
    if (!state.maximized && frame.GetSize().x > 0 && frame.GetSize().y > 0) {
        state.width = frame.GetSize().x;
        state.height = frame.GetSize().y;
        state.backing_scale = frame.GetContentScaleFactor();
    }
}

inline void BindNativeWindowGeometry(wxFrame& frame, const std::string& data_directory,
    const std::optional<SavedWindowGeometry>& restored, std::function<void(wxCloseEvent&)> close) {
    auto state = std::make_shared<SavedWindowGeometry>(SavedWindowGeometry{
        frame.GetSize().x, frame.GetSize().y, frame.GetContentScaleFactor(),
        restored && restored->maximized});
    const auto capture = [&frame, state] { UpdateNativeWindowGeometry(frame, *state); };
#ifdef __WXOSX__
    // Cocoa emits intermediate sizes while zooming, before IsMaximized() changes.
    frame.Bind(wxEVT_SIZING, [capture](wxSizeEvent& event) { capture(); event.Skip(); });
    const auto observe = [&frame, state] {
        if (!frame.IsIconized()) state->maximized = frame.IsMaximized();
    };
#else
    const auto observe = capture;
#endif
    frame.Bind(wxEVT_SIZE, [observe](wxSizeEvent& event) { observe(); event.Skip(); });
    frame.Bind(wxEVT_MOVE, [observe](wxMoveEvent& event) { observe(); event.Skip(); });
    frame.Bind(wxEVT_DPI_CHANGED, [observe](wxDPIChangedEvent& event) {
        observe(); event.Skip();
    });
    frame.Bind(wxEVT_CLOSE_WINDOW, [capture, state, data_directory, close](wxCloseEvent& event) {
        capture();
        SaveWindowGeometry(data_directory, *state);
        close(event);
    });
    if (state->maximized) frame.Maximize();
}

}  // namespace spark
