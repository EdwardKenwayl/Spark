#include "gui/model_picker.h"

#include <windowsx.h>
#include <algorithm>

namespace spark {

UINT ModelPicker::Track(HWND owner, HFONT font, POINT anchor, const std::string& preference,
                       std::vector<ModelPickerRow> rows, UINT checked) {
    rows_ = std::move(rows); font_ = font; palette_ = theme::For(preference); checked_ = checked;
    if (!rows_.empty() && rows_.back().kind == ModelPickerKind::Reasoning) {
        footer_ = std::move(rows_.back()); rows_.pop_back();
    }
    if (rows_.empty()) return 0;
    HDC dc = GetDC(owner); dpi_ = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(owner, dc);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT work = monitor.rcWork;
    const int footer_height = footer_.command ? Scale(40) : 0;
    visible_ = std::max(1, std::min<int>({12, static_cast<int>(rows_.size()),
        (work.bottom - work.top - Scale(24) - footer_height) / Scale(32)}));
    const int height = visible_ * Scale(32) + Scale(8) + footer_height;
    const int width = std::min(Scale(380), static_cast<int>(work.right - work.left));
    for (int index = 0; index < static_cast<int>(rows_.size()); ++index)
        if (rows_[index].command == checked_) cursor_ = index;
    top_ = std::clamp(cursor_ - visible_ / 2, 0, static_cast<int>(rows_.size()) - visible_);
    WNDCLASSW cls{}; cls.lpfnWndProc = WindowProc; cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = L"SparkModelPicker";
    RegisterClassW(&cls);
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, L"", WS_POPUP,
        std::clamp<int>(anchor.x, work.left, work.right - width),
        std::clamp<int>(anchor.y - height, work.top, work.bottom - height), width, height,
        owner, nullptr, cls.hInstance, this);
    if (!window_) return 0;
    ShowWindow(window_, SW_SHOW); SetFocus(window_); SetCapture(window_);
    MSG message{};
    while (!done_) {
        const int status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) { if (status == 0) PostQuitMessage(static_cast<int>(message.wParam)); break; }
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    if (GetCapture() == window_) ReleaseCapture();
    DestroyWindow(window_); window_ = nullptr;
    if (GetForegroundWindow() == owner) SetFocus(owner);
    return result_;
}

RECT ModelPicker::Thumb() const {
    if (rows_.size() <= static_cast<size_t>(visible_)) return {};
    RECT rect{}; GetClientRect(window_, &rect);
    const int track = visible_ * Scale(32);
    const int height = std::max(Scale(24), MulDiv(track, visible_, static_cast<int>(rows_.size())));
    const int y = Scale(4) + MulDiv(top_, track - height, static_cast<int>(rows_.size()) - visible_);
    return {rect.right - Scale(12), y, rect.right - Scale(4), y + height};
}

void ModelPicker::Scroll(int position) {
    const int next = std::clamp(position, 0, static_cast<int>(rows_.size()) - visible_);
    if (next != top_) { top_ = next; hovered_ = -1; InvalidateRect(window_, nullptr, FALSE); }
}

int ModelPicker::Hit(POINT point) const {
    RECT client{}; GetClientRect(window_, &client);
    if (!PtInRect(&client, point)) return -1;
    if (point.y >= Scale(4) + visible_ * Scale(32))
        return footer_.command ? static_cast<int>(rows_.size()) : -1;
    if (point.y < Scale(4) || (Thumb().right > 0 && point.x >= client.right - Scale(16))) return -1;
    return top_ + (point.y - Scale(4)) / Scale(32);
}

void ModelPicker::Paint(HDC dc) {
    RECT rect{}; GetClientRect(window_, &rect);
    HDC buffer = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, rect.right, rect.bottom);
    HGDIOBJ previous = SelectObject(buffer, bitmap), old_font = SelectObject(buffer, font_);
    HBRUSH panel = CreateSolidBrush(palette_.panel), hover = CreateSolidBrush(palette_.selection);
    FillRect(buffer, &rect, panel);
    HBRUSH border = CreateSolidBrush(palette_.border); FrameRect(buffer, &rect, border);
    SetBkMode(buffer, TRANSPARENT);
    for (int index = top_; index < top_ + visible_; ++index) {
        const auto& row = rows_[index];
        RECT item{Scale(4), Scale(4 + (index - top_) * 32), rect.right - Scale(16),
                  Scale(4 + (index - top_ + 1) * 32)};
        const bool heading = row.kind == ModelPickerKind::Header;
        if (!heading && (index == hovered_ || (hovered_ < 0 && index == cursor_))) FillRect(buffer, &item, hover);
        item.left += Scale(14); item.right -= Scale(26);
        SetTextColor(buffer, heading ? palette_.muted : palette_.text);
        DrawTextW(buffer, row.label.c_str(), -1, &item,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (!heading && row.command == checked_) {
            HPEN pen = CreatePen(PS_SOLID, Scale(2), palette_.text);
            HGDIOBJ old = SelectObject(buffer, pen);
            const int x = rect.right - Scale(36), y = (item.top + item.bottom) / 2;
            MoveToEx(buffer, x - Scale(4), y, nullptr); LineTo(buffer, x, y + Scale(4));
            LineTo(buffer, x + Scale(7), y - Scale(6)); SelectObject(buffer, old); DeleteObject(pen);
        }
    }
    RECT thumb = Thumb();
    HBRUSH rail = CreateSolidBrush(palette_.subtle); FillRect(buffer, &thumb, rail); DeleteObject(rail);
    if (footer_.command) {
        RECT footer{Scale(4), Scale(8) + visible_ * Scale(32), rect.right - Scale(4), rect.bottom - Scale(4)};
        if (hovered_ == static_cast<int>(rows_.size())) FillRect(buffer, &footer, hover);
        RECT line{Scale(8), footer.top - Scale(4), rect.right - Scale(8), footer.top - Scale(3)};
        FillRect(buffer, &line, border); footer.left += Scale(14);
        SetTextColor(buffer, palette_.text);
        DrawTextW(buffer, footer_.label.c_str(), -1, &footer, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    BitBlt(dc, 0, 0, rect.right, rect.bottom, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, old_font); SelectObject(buffer, previous); DeleteObject(bitmap); DeleteDC(buffer);
    DeleteObject(panel); DeleteObject(hover); DeleteObject(border);
}

LRESULT CALLBACK ModelPicker::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<ModelPicker*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<ModelPicker*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->HandleMessage(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT ModelPicker::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{}; HDC dc = BeginPaint(window_, &paint); Paint(dc); EndPaint(window_, &paint); return 0;
        }
        case WM_PRINTCLIENT: Paint(reinterpret_cast<HDC>(wparam)); return 0;
        case WM_MOUSEWHEEL: {
            wheel_ += GET_WHEEL_DELTA_WPARAM(wparam);
            UINT lines = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            Scroll(top_ - wheel_ / WHEEL_DELTA * static_cast<int>(std::min<UINT>(lines, visible_)));
            wheel_ %= WHEEL_DELTA; return 0;
        }
        case WM_LBUTTONDOWN: {
            RECT bounds{}; GetClientRect(window_, &bounds);
            if (!PtInRect(&bounds, point)) { done_ = true; return 0; }
            const RECT thumb = Thumb();
            if (thumb.right > 0 && point.x >= thumb.left - Scale(4) &&
                point.y < Scale(4) + visible_ * Scale(32)) {
                if (PtInRect(&thumb, point)) { drag_y_ = point.y; drag_top_ = top_; }
                else Scroll(top_ + (point.y < thumb.top ? -visible_ : visible_));
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (drag_y_ >= 0) {
                const RECT thumb = Thumb();
                Scroll(drag_top_ + MulDiv(point.y - drag_y_, static_cast<int>(rows_.size()) - visible_,
                    std::max(1L, visible_ * Scale(32) - thumb.bottom + thumb.top)));
            } else if (const int hit = Hit(point); hit != hovered_) {
                hovered_ = hit; InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP: {
            if (drag_y_ >= 0) { drag_y_ = -1; return 0; }
            const int hit = Hit(point);
            if (hit == static_cast<int>(rows_.size())) { result_ = footer_.command; done_ = true; }
            else if (hit >= 0 && rows_[hit].kind == ModelPickerKind::Model) {
                result_ = rows_[hit].command; done_ = true;
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) done_ = true;
            else if (wparam == VK_RETURN && cursor_ >= 0) { result_ = rows_[cursor_].command; done_ = true; }
            else if (wparam == VK_UP || wparam == VK_DOWN || wparam == VK_HOME || wparam == VK_END ||
                     wparam == VK_PRIOR || wparam == VK_NEXT) {
                const int direction = wparam == VK_UP || wparam == VK_HOME || wparam == VK_PRIOR ? -1 : 1;
                int next = wparam == VK_HOME ? 0 : wparam == VK_END ? static_cast<int>(rows_.size()) - 1 :
                    std::clamp(cursor_ + direction * (wparam == VK_PRIOR || wparam == VK_NEXT ? visible_ : 1),
                               0, static_cast<int>(rows_.size()) - 1);
                while (next >= 0 && next < static_cast<int>(rows_.size()) && rows_[next].kind != ModelPickerKind::Model)
                    next += direction;
                if (next >= 0 && next < static_cast<int>(rows_.size())) {
                    cursor_ = next;
                    if (cursor_ < top_) Scroll(cursor_);
                    else if (cursor_ >= top_ + visible_) Scroll(cursor_ - visible_ + 1);
                    hovered_ = -1; InvalidateRect(window_, nullptr, FALSE);
                }
            }
            return 0;
        case WM_ACTIVATE: if (LOWORD(wparam) == WA_INACTIVE) done_ = true; return 0;
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
        case WM_CLOSE: done_ = true; return 0;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

}  // namespace spark
