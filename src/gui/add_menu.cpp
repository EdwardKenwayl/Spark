#include "gui/add_menu.h"
#include "gui/localization.h"

#include <windowsx.h>

#include <algorithm>

namespace spark {
namespace {
constexpr wchar_t kClass[] = L"SparkAddMenu";

void Fill(HDC device, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(device, &rect, brush);
    DeleteObject(brush);
}
}  // namespace

AddMenu::~AddMenu() { if (IsWindow(window_)) DestroyWindow(window_); }

bool AddMenu::Create(HWND owner, HINSTANCE instance, HFONT font) {
    owner_ = owner;
    font_ = font;
    HDC device = GetDC(owner);
    dpi_ = GetDeviceCaps(device, LOGPIXELSX);
    ReleaseDC(owner, device);
    WNDCLASSW cls{};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = kClass;
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW, kClass, i18n::Text(L"Add"), WS_POPUP, 0, 0, 0, 0,
                              owner, nullptr, instance, this);
    return window_ != nullptr;
}

void AddMenu::Show(const RECT& composer, const std::string& preference,
                   const std::wstring& snapshot_title, const std::wstring& goal, bool plan) {
    palette_ = theme::For(preference);
    SetWindowTextW(window_, i18n::Text(L"Add"));
    snapshot_title_ = snapshot_title;
    goal_ = goal;
    plan_ = plan;
    hovered_ = -1;
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromRect(&composer, MONITOR_DEFAULTTONEAREST), &monitor);
    const int width = std::min<int>({composer.right - composer.left,
                                   monitor.rcWork.right - monitor.rcWork.left});
    const int height = Scale(32 + 4 * 42 + 8);
    const int x = std::clamp<int>(composer.left, monitor.rcWork.left, monitor.rcWork.right - width);
    const int y = std::clamp<int>(composer.top - height - Scale(6), monitor.rcWork.top,
                                 std::max<LONG>(monitor.rcWork.top, monitor.rcWork.bottom - height));
    SetWindowPos(window_, HWND_TOP, x, y, width, height, SWP_SHOWWINDOW);
    SetWindowRgn(window_, CreateRoundRectRgn(0, 0, width + 1, height + 1, Scale(8), Scale(8)), TRUE);
    SetFocus(window_);
    SetCapture(window_);
    InvalidateRect(window_, nullptr, FALSE);
}

void AddMenu::Hide() {
    if (window_ == nullptr) return;
    ShowWindow(window_, SW_HIDE);
    if (GetCapture() == window_) ReleaseCapture();
}

RECT AddMenu::RowRect(int row) const {
    RECT client{};
    GetClientRect(window_, &client);
    return {Scale(7), Scale(32 + row * 42), client.right - Scale(7), Scale(32 + (row + 1) * 42)};
}

int AddMenu::Hit(POINT point) const {
    for (int row = 0; row < 4; ++row) {
        const RECT rect = RowRect(row);
        if (PtInRect(&rect, point)) return row == 1 && snapshot_title_.empty() ? -1 : row;
    }
    return -1;
}

void AddMenu::Select(int row) {
    if (row == 1 && snapshot_title_.empty()) return;
    Hide();
    if (row < 0 || row > 3) return;
    SetFocus(owner_);
    PostMessageW(owner_, kAddActionMessage, static_cast<WPARAM>(row), 0);
}

void AddMenu::Paint(HDC device) {
    RECT client{};
    GetClientRect(window_, &client);
    Fill(device, client, palette_.panel);
    HPEN pen = CreatePen(PS_SOLID, 1, palette_.border);
    HGDIOBJ old_pen = SelectObject(device, pen);
    HGDIOBJ old_brush = SelectObject(device, GetStockObject(HOLLOW_BRUSH));
    RoundRect(device, 0, 0, client.right, client.bottom, Scale(8), Scale(8));
    SelectObject(device, old_pen);
    DeleteObject(pen);
    HGDIOBJ old_font = SelectObject(device, font_);
    SetBkMode(device, TRANSPARENT);
    SetTextColor(device, palette_.muted);
    RECT header{Scale(16), Scale(7), client.right - Scale(16), Scale(29)};
    DrawTextW(device, i18n::Text(L"Add"), -1, &header, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    const std::wstring titles[] = {
        i18n::Text(L"Files and folders"),
        i18n::Text(L"Attach smart snapshot"),
        i18n::Text(L"Goal"),
        i18n::Text(L"Plan mode"),
    };
    const std::wstring descriptions[] = {
        L"",
        snapshot_title_,
        goal_.empty() ? i18n::Text(L"Set a continuing objective") : goal_,
        plan_ ? i18n::Text(L"Enabled") : i18n::Text(L"Enable plan mode"),
    };
    for (int row = 0; row < 4; ++row) {
        const RECT rect = RowRect(row);
        const bool disabled = row == 1 && snapshot_title_.empty();
        const bool hover = row == hovered_ && !disabled;
        if (hover) {
            HBRUSH brush = CreateSolidBrush(palette_.selection);
            SelectObject(device, brush);
            SelectObject(device, GetStockObject(NULL_PEN));
            RoundRect(device, rect.left, rect.top, rect.right, rect.bottom, Scale(8), Scale(8));
            SelectObject(device, old_brush);
            SelectObject(device, old_pen);
            DeleteObject(brush);
        }
        const int x = rect.left + Scale(21);
        const int y = (rect.top + rect.bottom) / 2;
        const COLORREF foreground = disabled
            ? RGB((GetRValue(palette_.text) + 3 * GetRValue(palette_.panel)) / 4,
                  (GetGValue(palette_.text) + 3 * GetGValue(palette_.panel)) / 4,
                  (GetBValue(palette_.text) + 3 * GetBValue(palette_.panel)) / 4)
            : palette_.text;
        pen = CreatePen(PS_SOLID, std::max(1, Scale(1)), foreground);
        SelectObject(device, pen);
        SelectObject(device, GetStockObject(HOLLOW_BRUSH));
        if (row == 0) {
            // A paperclip, independent of symbol-font availability on Win7.
            RoundRect(device, x - Scale(5), y - Scale(10), x + Scale(5), y + Scale(8), Scale(8), Scale(8));
            MoveToEx(device, x + Scale(1), y - Scale(6), nullptr);
            LineTo(device, x + Scale(1), y + Scale(3));
            LineTo(device, x - Scale(2), y + Scale(3));
            LineTo(device, x - Scale(2), y - Scale(4));
        } else if (row == 1) {
            Rectangle(device, x - Scale(9), y - Scale(7), x + Scale(9), y + Scale(7));
            MoveToEx(device, x - Scale(5), y + Scale(3), nullptr);
            LineTo(device, x, y - Scale(2));
            LineTo(device, x + Scale(6), y + Scale(3));
            Ellipse(device, x + Scale(3), y - Scale(5), x + Scale(6), y - Scale(2));
        } else if (row == 2) {
            Ellipse(device, x - Scale(9), y - Scale(9), x + Scale(9), y + Scale(9));
            Ellipse(device, x - Scale(5), y - Scale(5), x + Scale(5), y + Scale(5));
            MoveToEx(device, x, y, nullptr);
            LineTo(device, x + Scale(10), y - Scale(10));
        } else {
            Ellipse(device, x - Scale(6), y - Scale(10), x + Scale(6), y + Scale(2));
            Rectangle(device, x - Scale(3), y + Scale(2), x + Scale(3), y + Scale(7));
            MoveToEx(device, x - Scale(2), y + Scale(10), nullptr);
            LineTo(device, x + Scale(2), y + Scale(10));
        }
        SelectObject(device, old_brush);
        SelectObject(device, old_pen);
        DeleteObject(pen);
        RECT label{rect.left + Scale(42), rect.top, rect.right - Scale(38), rect.bottom};
        SetTextColor(device, foreground);
        SIZE title_size{};
        GetTextExtentPoint32W(device, titles[row].c_str(), static_cast<int>(titles[row].size()), &title_size);
        DrawTextW(device, titles[row].c_str(), -1, &label, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        label.left += title_size.cx + Scale(12);
        if (label.left < label.right) {
            SetTextColor(device, palette_.muted);
            DrawTextW(device, descriptions[row].c_str(), -1, &label,
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        if ((row == 2 && !goal_.empty()) || (row == 3 && plan_)) {
            pen = CreatePen(PS_SOLID, std::max(1, Scale(2)), palette_.text);
            SelectObject(device, pen);
            MoveToEx(device, rect.right - Scale(24), y, nullptr);
            LineTo(device, rect.right - Scale(20), y + Scale(4));
            LineTo(device, rect.right - Scale(13), y - Scale(5));
            SelectObject(device, old_pen);
            DeleteObject(pen);
        }
    }
    SelectObject(device, old_font);
    SelectObject(device, old_brush);
    SelectObject(device, old_pen);
}

LRESULT CALLBACK AddMenu::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<AddMenu*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<AddMenu*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->HandleMessage(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT AddMenu::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC device = BeginPaint(window_, &paint);
            Paint(device);
            EndPaint(window_, &paint);
            return 0;
        }
        case WM_PRINTCLIENT: Paint(reinterpret_cast<HDC>(wparam)); return 0;
        case WM_MOUSEMOVE: {
            const int hover = Hit({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            if (hover != hovered_) {
                hovered_ = hover;
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: Select(Hit({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})); return 0;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) { Hide(); SetFocus(owner_); }
            else if (wparam == VK_UP || wparam == VK_DOWN) {
                do {
                    hovered_ = (hovered_ + (wparam == VK_UP ? 3 : 1) + 4) % 4;
                } while (hovered_ == 1 && snapshot_title_.empty());
                InvalidateRect(window_, nullptr, FALSE);
            } else if (wparam == VK_RETURN || wparam == VK_SPACE) Select(hovered_ < 0 ? 0 : hovered_);
            return 0;
        case WM_KILLFOCUS:
        case WM_CANCELMODE:
        case WM_CAPTURECHANGED: Hide(); return 0;
        default: break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

}  // namespace spark
