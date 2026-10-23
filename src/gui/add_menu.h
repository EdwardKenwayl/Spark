#pragma once

#include "gui/theme.h"

#include <functional>

namespace spark {

enum class AddAction { Files, Snapshot, Goal, Plan };
inline constexpr UINT kAddActionMessage = WM_APP + 9;
inline constexpr UINT kSnapshotMessage = WM_APP + 10;
inline constexpr UINT_PTR kSnapshotPollTimer = 5;

class AddMenu {
public:
    ~AddMenu();
    bool Create(HWND owner, HINSTANCE instance, HFONT font);
    void Show(const RECT& composer, const std::string& theme,
              const std::wstring& snapshot_title, const std::wstring& goal, bool plan);
    void Hide();
    HWND hwnd() const { return window_; }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    void Paint(HDC device);
    int Scale(int pixels) const { return MulDiv(pixels, dpi_, 96); }
    RECT RowRect(int row) const;
    int Hit(POINT point) const;
    void Select(int row);
    HWND window_ = nullptr;
    HWND owner_ = nullptr;
    HFONT font_ = nullptr;
    int dpi_ = 96;
    int hovered_ = -1;
    bool plan_ = false;
    std::wstring snapshot_title_;
    std::wstring goal_;
    theme::Palette palette_;
};

}  // namespace spark
