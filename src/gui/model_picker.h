#pragma once

#include "gui/theme.h"

#include <vector>

namespace spark {

enum class ModelPickerKind { Header, Model, Reasoning };
struct ModelPickerRow {
    UINT command;
    std::wstring label;
    ModelPickerKind kind;
};

class ModelPicker {
public:
    UINT Track(HWND owner, HFONT font, POINT anchor, const std::string& preference,
               std::vector<ModelPickerRow> rows, UINT checked);

private:
    friend struct AppWindowComposerTest;
    static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT, WPARAM, LPARAM);
    void Paint(HDC dc);
    void Scroll(int position);
    RECT Thumb() const;
    int Scale(int value) const { return MulDiv(value, dpi_, 96); }
    int Hit(POINT point) const;
    HWND window_ = nullptr;
    HFONT font_ = nullptr;
    theme::Palette palette_;
    std::vector<ModelPickerRow> rows_;
    ModelPickerRow footer_{0, {}, ModelPickerKind::Reasoning};
    UINT checked_ = 0, result_ = 0;
    int dpi_ = 96, top_ = 0, visible_ = 0, hovered_ = -1, cursor_ = -1, wheel_ = 0;
    int drag_y_ = -1, drag_top_ = 0;
    bool done_ = false;
};

}  // namespace spark
