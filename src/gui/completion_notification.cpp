#include "gui/completion_notification.h"

#include <algorithm>
#include <windowsx.h>

namespace spark {

struct CompletionNotifications::Card {
    HWND window = nullptr;
    CompletionNotice notice;
    Activate activate;
    theme::Palette palette;
    HFONT font = nullptr;
    int dpi = 96;
    int Scale(int value) const { return MulDiv(value, dpi, 96); }
    ~Card() {
        if (window != nullptr) DestroyWindow(window);
        if (font != nullptr) DeleteObject(font);
    }
    void Dismiss(bool open) {
        const auto callback = activate;
        const auto path = notice.path;
        DestroyWindow(window);
        if (open && callback) callback(path);
    }
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* card = reinterpret_cast<Card*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            card = static_cast<Card*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            card->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(card));
        }
        if (card == nullptr) return DefWindowProcW(window, message, wparam, lparam);
        switch (message) {
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
            case WM_ERASEBKGND: return 1;
            case WM_TIMER:
                card->Dismiss(false);
                return 0;
            case WM_LBUTTONUP: {
                RECT client{};
                GetClientRect(window, &client);
                const bool close = GET_X_LPARAM(lparam) >= client.right - card->Scale(38) &&
                                   GET_Y_LPARAM(lparam) <= card->Scale(38);
                card->Dismiss(!close);
                return 0;
            }
            case WM_KEYDOWN:
                if (wparam == VK_ESCAPE) card->Dismiss(false);
                else if (wparam == VK_RETURN || wparam == VK_SPACE) card->Dismiss(true);
                return 0;
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                HDC dc = BeginPaint(window, &paint);
                RECT client{};
                GetClientRect(window, &client);
                HBRUSH background = CreateSolidBrush(card->palette.panel);
                FillRect(dc, &client, background);
                DeleteObject(background);
                HBRUSH border = CreateSolidBrush(card->palette.border);
                FrameRect(dc, &client, border);
                DeleteObject(border);
                SetBkMode(dc, TRANSPARENT);
                const auto previous = SelectObject(dc, card->font);
                SetTextColor(dc, card->palette.muted);
                RECT brand{card->Scale(16), card->Scale(10), client.right - card->Scale(40), card->Scale(31)};
                DrawTextW(dc, L"Spark", -1, &brand, DT_SINGLELINE | DT_NOPREFIX);
                RECT close{client.right - card->Scale(36), card->Scale(8),
                           client.right - card->Scale(8), card->Scale(32)};
                DrawTextW(dc, L"\u00d7", -1, &close, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
                SetTextColor(dc, card->palette.text);
                RECT title{card->Scale(16), card->Scale(38),
                           client.right - card->Scale(16), card->Scale(60)};
                DrawTextW(dc, card->notice.title.c_str(), -1, &title,
                          DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                RECT body{card->Scale(16), card->Scale(65),
                          client.right - card->Scale(16), client.bottom - card->Scale(12)};
                DrawTextW(dc, card->notice.body.c_str(), -1, &body,
                          DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX | DT_EDITCONTROL);
                SelectObject(dc, previous);
                EndPaint(window, &paint);
                return 0;
            }
            case WM_NCDESTROY:
                KillTimer(window, 1);
                card->window = nullptr;
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
};

CompletionNotifications::CompletionNotifications() = default;
CompletionNotifications::~CompletionNotifications() = default;
void CompletionNotifications::Close() { cards_.clear(); }

bool CompletionNotifications::IsBackground(HWND owner) {
    if (owner == nullptr || !IsWindowVisible(owner) || IsIconic(owner)) return true;
    const HWND foreground = GetForegroundWindow();
    return foreground == nullptr || GetAncestor(foreground, GA_ROOTOWNER) != GetAncestor(owner, GA_ROOTOWNER);
}

bool CompletionNotifications::Show(HWND owner, CompletionNotice notice, theme::Palette palette, Activate activate) {
    cards_.erase(std::remove_if(cards_.begin(), cards_.end(),
                                [](const auto& card) { return card->window == nullptr; }), cards_.end());
    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    WNDCLASSW cls{};
    cls.lpfnWndProc = Card::Procedure;
    cls.hInstance = instance;
    cls.lpszClassName = kCompletionNotificationClass;
    cls.hCursor = LoadCursorW(nullptr, IDC_HAND);
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    auto card = std::make_unique<Card>();
    card->notice = std::move(notice);
    card->activate = std::move(activate);
    card->palette = palette;
    HDC screen = GetDC(owner);
    card->dpi = std::max(96, GetDeviceCaps(screen, LOGPIXELSY));
    ReleaseDC(owner, screen);
    card->font = CreateFontW(-card->Scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor))
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &monitor.rcWork, 0);
    const int width = std::min<int>(card->Scale(380), monitor.rcWork.right - monitor.rcWork.left - card->Scale(24));
    const int height = card->Scale(154);
    const int step = height + card->Scale(10);
    const size_t capacity = static_cast<size_t>(std::max(1, std::min<int>(3,
        (monitor.rcWork.bottom - monitor.rcWork.top - card->Scale(24)) / step)));
    while (cards_.size() >= capacity) cards_.erase(cards_.begin());
    for (size_t index = 0; index < cards_.size(); ++index) {
        SetWindowPos(cards_[index]->window, HWND_TOPMOST,
                     monitor.rcWork.right - width - card->Scale(12),
                     monitor.rcWork.bottom - height - card->Scale(12) -
                         static_cast<int>(cards_.size() - index) * step,
                     width, height, SWP_NOACTIVATE);
    }
    const std::wstring accessible = L"Spark - " + card->notice.title + L"\n" + card->notice.body;
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                                  kCompletionNotificationClass, accessible.c_str(), WS_POPUP,
                                  monitor.rcWork.right - width - card->Scale(12),
                                  monitor.rcWork.bottom - height - card->Scale(12),
                                  width, height, nullptr, nullptr, instance, card.get());
    if (window == nullptr) return false;
    SetTimer(window, 1, 10000, nullptr);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    cards_.push_back(std::move(card));
    return true;
}

}  // namespace spark
