#include "gui/settings_window.h"
#include "gui/localization.h"
#include "gui/theme.h"
#include "core/session.h"

#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <cwctype>

namespace spark {
namespace {
void Fill(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void Icon(HDC dc, RECT rect, int action, COLORREF color) {
    const int state = SaveDC(dc);
    const auto x = [&](int value) { return rect.left + MulDiv(value, rect.right - rect.left, 24); };
    const auto y = [&](int value) { return rect.top + MulDiv(value, rect.bottom - rect.top, 24); };
    const auto line = [&](int a, int b, int c, int d) {
        MoveToEx(dc, x(a), y(b), nullptr); LineTo(dc, x(c), y(d));
    };
    HPEN pen = CreatePen(PS_SOLID, std::max(1L, (rect.right - rect.left) / 12), color);
    HBRUSH brush = CreateSolidBrush(color);
    SelectObject(dc, pen);
    SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    if (action == 0) {
        SelectObject(dc, brush);
        for (int a : {9, 15}) for (int b : {5, 12, 19}) Ellipse(dc, x(a - 1), y(b - 1), x(a + 2), y(b + 2));
    } else if (action == 1) {
        line(14, 2, 22, 10);
        const POINT tube[] = {{x(16), y(4)}, {x(3), y(17)}, {x(3), y(20)},
                              {x(6), y(22)}, {x(9), y(21)}, {x(20), y(8)}};
        Polyline(dc, tube, static_cast<int>(std::size(tube)));
        line(8, 12, 14, 14);
    } else if (action == 2) {
        const POINT points[] = {{x(5), y(15)}, {x(16), y(4)}, {x(20), y(8)},
                                {x(9), y(19)}, {x(4), y(20)}, {x(5), y(15)}};
        Polyline(dc, points, static_cast<int>(std::size(points)));
        line(14, 6, 18, 10); line(13, 21, 21, 21);
    } else if (action == 3) {
        RoundRect(dc, x(9), y(9), x(21), y(21), 2, 2);
        line(5, 15, 3, 15); line(3, 15, 3, 3); line(3, 3, 15, 3); line(15, 3, 15, 5);
    } else {
        line(3, 6, 21, 6); line(9, 6, 9, 3); line(9, 3, 15, 3); line(15, 3, 15, 6);
        line(6, 6, 7, 21); line(7, 21, 17, 21); line(17, 21, 18, 6);
        line(10, 10, 10, 17); line(14, 10, 14, 17);
    }
    RestoreDC(dc, state);
    DeleteObject(brush);
    DeleteObject(pen);
}
}  // namespace

void SettingsWindow::EditProvider(size_t index) {
    CancelProviderTest();
    CancelProviderDrag();
    const bool existing = index < config_.providers.size();
    ProviderConfig provider;
    if (existing) {
        provider = config_.providers[index];
        if (provider.label.empty()) provider.label = provider.id;
    }
    else { provider.id = "provider-" + NewSessionId(); provider.protocol = "responses"; }
    provider_editor_.Open(window_, config_.ui.theme, ui_font_, title_font_, std::move(provider), config_.context,
        [this, existing](ProviderConfig draft) -> Result<void> {
            AppConfig updated = config_;
            auto found = std::find_if(updated.providers.begin(), updated.providers.end(),
                [&](const ProviderConfig& entry) { return entry.id == draft.id; });
            if (existing && found == updated.providers.end()) return Result<void>::Failure("Provider no longer exists.");
            if (existing) *found = std::move(draft);
            else updated.providers.push_back(std::move(draft));
            const auto* active = FindProvider(updated, updated.active.provider);
            if (!active || !FindModel(*active, updated.active.model)) {
                if (!active) active = &updated.providers.back();
                updated.active.provider = active->id;
                updated.active.model = active->models.front().id;
            }
            return SaveProviders(std::move(updated));
        }, [this] { SwitchPage(Page::Providers); });
    ShowWindow(provider_list_, SW_HIDE); ShowWindow(add_provider_, SW_HIDE); ShowWindow(provider_empty_, SW_HIDE);
    ShowWindow(page_title_, SW_HIDE);
    ShowWindow(provider_status_, SW_HIDE);
    for (HWND control : {model_status_, config_path_, open_config_,
                         GetDlgItem(window_, 492), GetDlgItem(window_, 493)})
        ShowWindow(control, SW_HIDE);
    Layout();
}

void SettingsWindow::CreateProviderControls() {
    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window_, GWLP_HINSTANCE));
    provider_list_ = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL |
        LBS_NOTIFY | LBS_HASSTRINGS | LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT | LBS_NOSEL,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kProviderList), instance, nullptr);
    add_provider_ = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kAddProvider), instance, nullptr);
    provider_empty_ = CreateWindowW(L"STATIC", L"", WS_CHILD | SS_CENTER,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kProviderEmpty), instance, nullptr);
    provider_status_ = CreateWindowW(L"STATIC", L"", WS_CHILD | SS_LEFT,
        0, 0, 0, 0, window_, nullptr, instance, nullptr);
    for (HWND control : {provider_list_, add_provider_, provider_empty_, provider_status_})
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), FALSE);
    SendMessageW(provider_list_, LB_SETITEMHEIGHT, 0, Dpi(88));
    SetWindowSubclass(provider_list_, ProviderListProc, 1, reinterpret_cast<DWORD_PTR>(this));
    provider_tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP |
        TTS_NOPREFIX, 0, 0, 0, 0, provider_list_, nullptr, instance, nullptr);
    for (UINT_PTR action = 0; action < 5; ++action) {
        TOOLINFOW tool{TTTOOLINFOW_V2_SIZE};
        tool.hwnd = provider_list_;
        tool.uId = action + 1;
        tool.lpszText = const_cast<wchar_t*>(L"");
        SendMessageW(provider_tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }
    SendMessageW(provider_tooltip_, TTM_SETDELAYTIME, TTDT_INITIAL, 450);
    SendMessageW(provider_tooltip_, TTM_SETDELAYTIME, TTDT_RESHOW, 100);
    SendMessageW(provider_tooltip_, TTM_SETMAXTIPWIDTH, 0, Dpi(360));
}

void SettingsWindow::RefreshProviders() {
    if (!provider_list_) return;
    CancelProviderDrag();
    const int top = static_cast<int>(SendMessageW(provider_list_, LB_GETTOPINDEX, 0, 0));
    SendMessageW(provider_list_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(provider_list_, LB_RESETCONTENT, 0, 0);
    for (const auto& provider : config_.providers) {
        const std::wstring label = Utf8ToWide(provider.label.empty() ? provider.id : provider.label);
        SendMessageW(provider_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    if (!config_.providers.empty())
        SendMessageW(provider_list_, LB_SETTOPINDEX, std::min(top, static_cast<int>(config_.providers.size()) - 1), 0);
    provider_hover_row_ = provider_hover_action_ = -1;
    const wchar_t* tips[] = {Text(L"Reorder providers"), Text(L"Send hi test"), Text(L"Edit provider"),
                             Text(L"Duplicate provider"), Text(L"Delete provider")};
    for (UINT_PTR action = 0; action < std::size(tips); ++action) {
        TOOLINFOW tool{TTTOOLINFOW_V2_SIZE};
        tool.hwnd = provider_list_;
        tool.uId = action + 1;
        tool.lpszText = const_cast<wchar_t*>(tips[action]);
        SendMessageW(provider_tooltip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
        SendMessageW(provider_tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
    }
    SendMessageW(provider_tooltip_, TTM_POP, 0, 0);
    SendMessageW(provider_list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(provider_list_, nullptr, FALSE);
}

RECT SettingsWindow::ProviderActionRect(int row, int action) const {
    RECT rect{};
    if (row < 0 || row >= static_cast<int>(config_.providers.size()) || action < 0 || action > 4 ||
        SendMessageW(provider_list_, LB_GETITEMRECT, row, reinterpret_cast<LPARAM>(&rect)) == LB_ERR) return {};
    rect.bottom -= Dpi(8);
    const LONG center_y = (rect.top + rect.bottom) / 2;
    if (action == 0) return {rect.left + Dpi(10), center_y - Dpi(16), rect.left + Dpi(34), center_y + Dpi(16)};
    const LONG left = rect.right - Dpi(12 + (5 - action) * 32);
    const bool compact = SendMessageW(provider_list_, LB_GETITEMHEIGHT, 0, 0) == Dpi(112);
    const LONG top = compact ? rect.bottom - Dpi(34) : center_y - Dpi(16);
    return {left, top, left + Dpi(32), top + Dpi(32)};
}

int SettingsWindow::ProviderRowAt(POINT point) const {
    const LRESULT hit = SendMessageW(provider_list_, LB_ITEMFROMPOINT, 0, MAKELPARAM(point.x, point.y));
    if (HIWORD(hit) != 0 || LOWORD(hit) >= config_.providers.size()) return -1;
    RECT rect{};
    SendMessageW(provider_list_, LB_GETITEMRECT, LOWORD(hit), reinterpret_cast<LPARAM>(&rect));
    rect.bottom -= Dpi(8);
    return PtInRect(&rect, point) ? LOWORD(hit) : -1;
}

int SettingsWindow::ProviderActionAt(POINT point) const {
    const int row = ProviderRowAt(point);
    for (int action = 0; action < 5; ++action) {
        const RECT rect = ProviderActionRect(row, action);
        if (PtInRect(&rect, point)) return action;
    }
    return -1;
}

void SettingsWindow::HoverProvider(POINT point) {
    const int row = ProviderRowAt(point), action = ProviderActionAt(point);
    if (row == provider_hover_row_ && action == provider_hover_action_) return;
    for (int index : {provider_hover_row_, row}) {
        RECT rect{};
        if (index >= 0 && SendMessageW(provider_list_, LB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&rect)) != LB_ERR)
            InvalidateRect(provider_list_, &rect, FALSE);
    }
    if (row != provider_hover_row_) SendMessageW(provider_tooltip_, TTM_POP, 0, 0);
    provider_hover_row_ = row;
    provider_hover_action_ = action;
    for (UINT_PTR index = 0; index < 5; ++index) {
        TOOLINFOW tool{TTTOOLINFOW_V2_SIZE};
        tool.hwnd = provider_list_;
        tool.uId = index + 1;
        tool.rect = ProviderActionRect(row, static_cast<int>(index));
        SendMessageW(provider_tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
    }
}

void SettingsWindow::DrawProviderControl(const DRAWITEMSTRUCT& item) {
    if (item.CtlID == kProviderList && item.itemID >= config_.providers.size()) return;
    const auto palette = theme::For(config_.ui.theme);
    const int saved = SaveDC(item.hDC);
    Fill(item.hDC, item.rcItem, palette.background);
    HBRUSH brush = CreateSolidBrush(palette.panel);
    HPEN pen = CreatePen(PS_SOLID, 1, palette.border);
    SelectObject(item.hDC, brush);
    SelectObject(item.hDC, pen);
    RECT card = item.rcItem;
    if (item.CtlID == kProviderList) card.bottom -= Dpi(8);
    RoundRect(item.hDC, card.left, card.top, card.right, card.bottom, Dpi(8), Dpi(8));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, palette.text);
    SelectObject(item.hDC, ui_font_);
    if (item.CtlID == kAddProvider) {
        const int x = card.left + Dpi(20), y = (card.top + card.bottom) / 2;
        HPEN plus = CreatePen(PS_SOLID, std::max(1, Dpi(1)), palette.text);
        SelectObject(item.hDC, plus);
        MoveToEx(item.hDC, x - Dpi(5), y, nullptr); LineTo(item.hDC, x + Dpi(6), y);
        MoveToEx(item.hDC, x, y - Dpi(5), nullptr); LineTo(item.hDC, x, y + Dpi(6));
        SelectObject(item.hDC, pen);
        DeleteObject(plus);
        card.left += Dpi(38); card.right -= Dpi(10);
        DrawTextW(item.hDC, Text(L"Add provider"), -1, &card, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        if (item.itemState & ODS_FOCUS) DrawFocusRect(item.hDC, &item.rcItem);
    } else {
        const auto& provider = config_.providers[item.itemID];
        const std::wstring name = Utf8ToWide(provider.label.empty() ? provider.id : provider.label);
        const bool hovered = provider_hover_row_ == static_cast<int>(item.itemID);
        const bool compact = SendMessageW(provider_list_, LB_GETITEMHEIGHT, 0, 0) == Dpi(112);
        RECT grip = ProviderActionRect(item.itemID, 0);
        InflateRect(&grip, -Dpi(4), -Dpi(6));
        Icon(item.hDC, grip, 0, palette.subtle);
        RECT avatar{card.left + Dpi(42), card.top + Dpi(22), card.left + Dpi(78), card.top + Dpi(58)};
        Fill(item.hDC, avatar, palette.selection);
        SelectObject(item.hDC, sidebar_font_);
        const std::wstring initial = name.empty() ? L"?" : std::wstring(1, std::towupper(name.front()));
        DrawTextW(item.hDC, initial.c_str(), -1, &avatar, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RECT text{avatar.right + Dpi(12), card.top + Dpi(14),
                  card.right - Dpi(hovered && !compact ? 152 : 16), card.top + Dpi(40)};
        DrawTextW(item.hDC, name.c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(item.hDC, ui_font_);
        text.top += Dpi(30); text.bottom += Dpi(30);
        SetTextColor(item.hDC, palette.muted);
        const std::wstring protocol = provider.protocol == "responses" ? L"Responses API"
            : provider.protocol == "chat_completions" ? L"Chat Completions API" : Utf8ToWide(provider.protocol);
        const std::wstring detail = protocol + L" \u00b7 " + Utf8ToWide(provider.base_url);
        DrawTextW(item.hDC, detail.c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (hovered) for (int action = 1; action <= 4; ++action) {
            RECT rect = ProviderActionRect(item.itemID, action);
            const bool hot = provider_hover_action_ == action;
            if (hot) Fill(item.hDC, rect, palette.selection);
            InflateRect(&rect, -Dpi(7), -Dpi(7));
            Icon(item.hDC, rect, action, hot ? (action == 4 ? palette.error : palette.text) : palette.muted);
        }
    }
    RestoreDC(item.hDC, saved);
    DeleteObject(pen);
    DeleteObject(brush);
}

LRESULT CALLBACK SettingsWindow::ProviderListProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                                  UINT_PTR subclass_id, DWORD_PTR ref_data) {
    auto* self = reinterpret_cast<SettingsWindow*>(ref_data);
    if (!self) return DefSubclassProc(window, message, wparam, lparam);
    switch (message) {
        case WM_MOUSEMOVE: {
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (!self->provider_pressed_id_.empty()) {
                self->UpdateProviderDrag(point);
                if (self->provider_dragging_) SetTimer(window, 1, 80, nullptr);
            }
            if (!self->provider_dragging_) {
                self->HoverProvider(point);
                MSG event{window, message, wparam, lparam, static_cast<DWORD>(GetMessageTime()), point};
                ClientToScreen(window, &event.pt);
                SendMessageW(self->provider_tooltip_, TTM_RELAYEVENT, 0, reinterpret_cast<LPARAM>(&event));
            }
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking);
            return 0;
        }
        case WM_MOUSELEAVE:
        case WM_MOUSEWHEEL:
        case WM_VSCROLL:
            self->HoverProvider({-1, -1});
            break;
        case WM_SHOWWINDOW:
            if (!wparam) self->CancelProviderDrag();
            self->HoverProvider({-1, -1});
            break;
        case WM_LBUTTONDOWN: {
            self->CancelProviderDrag();
            SetFocus(window);
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const int row = self->ProviderRowAt(point), action = self->ProviderActionAt(point);
            if (row >= 0 && action >= 0) {
                self->provider_pressed_id_ = self->config_.providers[row].id;
                self->provider_pressed_action_ = action;
                self->provider_pressed_point_ = point;
                SetCapture(window);
            }
            SendMessageW(self->provider_tooltip_, TTM_POP, 0, 0);
            return 0;
        }
        case WM_LBUTTONUP: {
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (self->provider_dragging_) self->UpdateProviderDrag(point);
            const auto id = self->provider_pressed_id_;
            const int action = self->provider_pressed_action_, drop = self->provider_drop_;
            const bool dragged = self->provider_dragging_;
            self->CancelProviderDrag();
            const auto found = std::find_if(self->config_.providers.begin(), self->config_.providers.end(),
                [&](const ProviderConfig& provider) { return provider.id == id; });
            if (found == self->config_.providers.end()) return 0;
            const auto row = static_cast<size_t>(found - self->config_.providers.begin());
            if (dragged && drop >= 0) {
                const size_t destination = static_cast<size_t>(drop) - (static_cast<size_t>(drop) > row ? 1 : 0);
                const auto result = self->MoveProvider(id, destination);
                if (!result.ok) self->SetStatus(i18n::Format(L"Save failed: {error}",
                    {{L"error", Utf8ToWide(result.error)}}));
            } else if (!dragged && self->ProviderRowAt(point) == static_cast<int>(row) &&
                       self->ProviderActionAt(point) == action) {
                self->ActivateProviderAction(row, action);
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
            self->CancelProviderDrag();
            break;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) { self->CancelProviderDrag(); return 0; }
            break;
        case WM_TIMER:
            if (wparam == 2) { self->PollProviderTest(); return 0; }
            if (wparam == 1 && self->provider_dragging_) {
                POINT point{}; GetCursorPos(&point); ScreenToClient(window, &point);
                RECT rect{}; GetClientRect(window, &rect);
                if (point.y < self->Dpi(24)) SendMessageW(window, WM_VSCROLL, SB_LINEUP, 0);
                else if (point.y >= rect.bottom - self->Dpi(24)) SendMessageW(window, WM_VSCROLL, SB_LINEDOWN, 0);
                self->UpdateProviderDrag(point);
                return 0;
            }
            break;
        case WM_PAINT:
        case WM_PRINTCLIENT: {
            const auto result = DefSubclassProc(window, message, wparam, lparam);
            if (!self->provider_dragging_ || self->provider_drop_ < 0) return result;
            RECT rect{};
            const int count = static_cast<int>(self->config_.providers.size());
            if (count == 0) return result;
            SendMessageW(window, LB_GETITEMRECT, std::min(self->provider_drop_, count - 1),
                         reinterpret_cast<LPARAM>(&rect));
            const int y = self->provider_drop_ == count ? rect.bottom - self->Dpi(4) : rect.top + self->Dpi(1);
            HDC dc = message == WM_PAINT ? GetDC(window) : reinterpret_cast<HDC>(wparam);
            HPEN pen = CreatePen(PS_SOLID, self->Dpi(2), theme::For(self->config_.ui.theme).accent);
            const auto previous = SelectObject(dc, pen);
            MoveToEx(dc, rect.left + self->Dpi(4), y, nullptr); LineTo(dc, rect.right - self->Dpi(4), y);
            SelectObject(dc, previous); DeleteObject(pen);
            if (message == WM_PAINT) ReleaseDC(window, dc);
            return result;
        }
        case WM_LBUTTONDBLCLK:
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT && self->provider_hover_action_ >= 0) {
                SetCursor(LoadCursorW(nullptr, self->provider_hover_action_ == 0 ? IDC_SIZEALL : IDC_HAND));
                return TRUE;
            }
            break;
        case WM_NCDESTROY:
            self->CancelProviderDrag();
            self->CancelProviderTest();
            RemoveWindowSubclass(window, ProviderListProc, subclass_id);
            self->provider_list_ = nullptr;
            break;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}
}  // namespace spark
