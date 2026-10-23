#include "gui/summary_view.h"
#include "gui/localization.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include "platform/filesystem.h"
#include <set>

namespace spark {
namespace {
constexpr wchar_t kSummaryClass[] = L"SparkSummaryView";
bool Contains(const RECT& rect, POINT point) { return PtInRect(&rect, point) != FALSE; }

void Fill(HDC device, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(device, &rect, brush);
    DeleteObject(brush);
}

std::wstring RelativeTime(uint64_t timestamp) {
    if (timestamp == 0) return {};
    const uint64_t now = UnixTimeSeconds();
    const uint64_t seconds = now > timestamp ? now - timestamp : 0;
    if (seconds < 60) return i18n::Text(L"Just now");
    if (seconds < 3600) return i18n::Format(L"{count} min ago", {{L"count", std::to_wstring(seconds / 60)}});
    if (seconds < 86400) return i18n::Format(L"{count} hr ago", {{L"count", std::to_wstring(seconds / 3600)}});
    const uint64_t days = seconds / 86400;
    return i18n::Format(days == 1 ? L"{count} day ago" : L"{count} days ago",
                        {{L"count", std::to_wstring(days)}});
}
}  // namespace

void DrawSubAgentGlyph(HDC device, const RECT& rect, const std::string& id) {
    // A stable local avatar: no model-generated asset or platform-specific font.
    uint32_t hash = 2166136261u;
    for (const unsigned char value : id) hash = (hash ^ value) * 16777619u;
    const COLORREF colors[] = {RGB(218, 180, 44), RGB(78, 168, 179), RGB(159, 119, 212), RGB(75, 164, 135)};
    const COLORREF color = colors[hash % std::size(colors)];
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ old_brush = SelectObject(device, brush);
    HGDIOBJ old_pen = SelectObject(device, GetStockObject(NULL_PEN));
    const int x = (rect.left + rect.right) / 2;
    const int y = (rect.top + rect.bottom) / 2;
    const int radius = std::max(2L, std::min(rect.right - rect.left, rect.bottom - rect.top) / 2 - 1);
    const auto point = [&](double angle, double length) {
        return POINT{x + static_cast<LONG>(std::lround(std::cos(angle) * length)),
                     y + static_cast<LONG>(std::lround(std::sin(angle) * length))};
    };
    const uint32_t shape = (hash >> 8) % 4;
    if (shape == 0) {
        POINT star[16]{};
        for (size_t index = 0; index < std::size(star); ++index) {
            star[index] = point(index * 3.141592653589793 / 8, radius * (index % 2 == 0 ? 1.0 : 0.4));
        }
        Polygon(device, star, static_cast<int>(std::size(star)));
    } else if (shape == 1 || shape == 2) {
        const int petal = std::max(1, radius / 3);
        const int petals = shape == 1 ? 7 : 6;
        for (int index = 0; index < petals; ++index) {
            const POINT center = point(index * 6.283185307179586 / petals, radius - petal);
            Ellipse(device, center.x - petal, center.y - petal, center.x + petal + 1, center.y + petal + 1);
        }
        Ellipse(device, x - petal, y - petal, x + petal + 1, y + petal + 1);
    } else {
        const POINT top[] = {{x - radius, y - radius}, {x + radius, y - radius}, {x, y}};
        const POINT bottom[] = {{x, y}, {x + radius, y + radius}, {x - radius, y + radius}};
        Polygon(device, top, 3);
        Polygon(device, bottom, 3);
    }
    SelectObject(device, old_pen);
    SelectObject(device, old_brush);
    DeleteObject(brush);
}

std::vector<SourceReference> CollectSummarySources(const SessionState& state) {
    std::vector<SourceReference> sources;
    std::set<std::string> seen;
    const auto add = [&](SourceReference source) {
        if (sources.size() >= 512 || source.kind == "subagent") return;
        source.label = TruncateUtf8(source.label, 2048);
        source.detail = TruncateUtf8(source.detail, 4096);
        const bool path = source.kind == "image" || source.kind == "file";
        const bool web_url = source.kind == "url_citation" || source.kind == "web_source" ||
                             source.kind == "web_open" || source.kind == "web_find" || source.kind == "web";
        const std::string key = (web_url ? "web" : source.kind) + "\n" +
                                (path ? NormalizePathForComparison(source.detail) : source.detail);
        if (seen.insert(key).second) sources.push_back(std::move(source));
    };
    for (const ChatMessage& message : state.messages) {
        for (const std::string& path : message.image_paths) {
            add({"image", WideToUtf8(spark::fs::path(Utf8ToWide(path)).filename().wstring()), path});
        }
        for (const SourceReference& source : message.sources) add(source);
    }
    return sources;
}

void DrawSummaryGlyph(HDC device, const RECT& rect, COLORREF color, int dpi) {
    const int x = (rect.left + rect.right) / 2;
    const int y = (rect.top + rect.bottom) / 2;
    const auto scale = [dpi](int value) { return MulDiv(value, dpi, 96); };
    HPEN pen = CreatePen(PS_SOLID, std::max(1, scale(1)), color);
    HGDIOBJ old_pen = SelectObject(device, pen);
    HGDIOBJ old_brush = SelectObject(device, GetStockObject(HOLLOW_BRUSH));
    for (int offset : {-5, 5}) {
        Ellipse(device, x - scale(10), y + scale(offset - 3), x - scale(4), y + scale(offset + 3));
        MoveToEx(device, x, y + scale(offset), nullptr);
        LineTo(device, x + scale(9), y + scale(offset));
    }
    SelectObject(device, old_brush);
    SelectObject(device, old_pen);
    DeleteObject(pen);
}

SummaryView::~SummaryView() { if (IsWindow(window_)) DestroyWindow(window_); }

bool SummaryView::Create(HWND owner, HINSTANCE instance, HFONT font, bool popup) {
    font_ = font;
    popup_ = popup;
    HDC dc = GetDC(owner);
    dpi_ = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(owner, dc);
    WNDCLASSW cls{};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = kSummaryClass;
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    window_ = CreateWindowExW(popup ? WS_EX_TOOLWINDOW : 0, kSummaryClass, L"",
                              popup ? WS_POPUP : WS_CHILD | WS_CLIPCHILDREN,
                              0, 0, 0, 0, owner, nullptr, instance, this);
    return window_ != nullptr;
}

void SummaryView::SetActions(Action action, Thumbnail thumbnail) {
    action_ = std::move(action);
    thumbnail_ = std::move(thumbnail);
}

void SummaryView::SetAppearance(const std::string& preference) {
    palette_ = theme::For(preference);
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void SummaryView::SetContent(SummaryTab tab, std::vector<SummaryRow> rows,
                             const std::wstring& title, const std::wstring& model, bool detail,
                             const std::string& agent_id) {
    if (tab != tab_ || detail != detail_) scroll_ = 0;
    tab_ = tab;
    rows_ = std::move(rows);
    title_ = title;
    model_ = model;
    agent_id_ = agent_id;
    detail_ = detail;
    hits_.clear();
    if (popup_ && IsWindowVisible(window_)) ShowPopup(popup_anchor_);
    Scroll(0);
}

int SummaryView::HeaderHeight() const { return popup_ ? 0 : Scale(detail_ ? 88 : 44); }
int SummaryView::RowHeight(const SummaryRow& row) const {
    if (row.kind == SummaryRowKind::Divider) return Scale(12);
    if (row.kind == SummaryRowKind::Source) return Scale(112);
    if (row.kind == SummaryRowKind::Agent) return Scale(popup_ ? 36 : 46);
    return Scale(row.kind == SummaryRowKind::Section ? 40 : 38);
}

void SummaryView::ShowPopup(POINT anchor) {
    popup_anchor_ = anchor;
    int height = Scale(20);
    for (const auto& row : rows_) height += RowHeight(row);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &monitor);
    height = std::min(height, std::max(1, static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top) - Scale(24)));
    const int width = std::min(Scale(320), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
    const int x = std::clamp<int>(anchor.x - width, static_cast<int>(monitor.rcWork.left),
                             std::max(static_cast<int>(monitor.rcWork.left), static_cast<int>(monitor.rcWork.right) - width));
    const int y = std::clamp<int>(anchor.y + Scale(6), static_cast<int>(monitor.rcWork.top),
                             std::max(static_cast<int>(monitor.rcWork.top), static_cast<int>(monitor.rcWork.bottom) - height));
    SetWindowPos(window_, HWND_TOP, x, y, width, height, SWP_SHOWWINDOW);
    SetWindowRgn(window_, CreateRoundRectRgn(0, 0, width + 1, height + 1, Scale(8), Scale(8)), TRUE);
    SetFocus(window_);
}

void SummaryView::Hide() { if (window_) ShowWindow(window_, SW_HIDE); }

void SummaryView::Scroll(int delta) {
    RECT client{};
    GetClientRect(window_, &client);
    int height = Scale(12);
    for (const auto& row : rows_) height += RowHeight(row);
    content_height_ = height;
    scroll_ = std::clamp(scroll_ + delta, 0, std::max<int>(0, height - (client.bottom - HeaderHeight())));
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void SummaryView::DragScroll(int y) {
    RECT client{};
    GetClientRect(window_, &client);
    const int viewport = client.bottom - HeaderHeight();
    const int track = viewport - (scroll_thumb_.bottom - scroll_thumb_.top);
    if (track <= 0) return;
    const int top = std::clamp(y - scroll_drag_offset_ - HeaderHeight(), 0, track);
    const int desired = static_cast<int>(static_cast<int64_t>(top) *
                                          std::max(0, content_height_ - viewport) / track);
    Scroll(desired - scroll_);
}

void SummaryView::Paint(HDC device) {
    RECT client{};
    GetClientRect(window_, &client);
    Fill(device, client, palette_.background);
    if (popup_) {
        HPEN pen = CreatePen(PS_SOLID, 1, palette_.border);
        HGDIOBJ old_pen = SelectObject(device, pen);
        HGDIOBJ old_brush = SelectObject(device, GetStockObject(HOLLOW_BRUSH));
        RoundRect(device, 0, 0, client.right, client.bottom, Scale(8), Scale(8));
        SelectObject(device, old_brush);
        SelectObject(device, old_pen);
        DeleteObject(pen);
    }
    HGDIOBJ old_font = SelectObject(device, font_);
    SetBkMode(device, TRANSPARENT);
    hits_.clear();
    const auto text = [&](const std::wstring& value, RECT rect, COLORREF color, UINT flags) {
        SetTextColor(device, color);
        DrawTextW(device, value.c_str(), static_cast<int>(value.size()), &rect, flags | DT_NOPREFIX);
    };
    const auto line = [&](int y) {
        RECT border{Scale(16), y, client.right - Scale(16), y + 1};
        Fill(device, border, palette_.border);
    };
    if (!popup_) {
        for (int index = 0; index < 2; ++index) {
            const SummaryTab tab = index == 0 ? SummaryTab::Agents : SummaryTab::Sources;
            RECT rect{Scale(10 + index * 112), Scale(4), Scale(116 + index * 112), Scale(40)};
            const std::wstring label = index == 0 ? i18n::Text(L"Subagents") : i18n::Text(L"Sources");
            if (tab_ == tab || Contains(rect, mouse_)) Fill(device, rect, palette_.selection);
            text(label, rect, tab_ == tab ? palette_.text : palette_.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            hits_.push_back({rect, index == 0 ? SummaryAction::Agents : SummaryAction::Sources, {}});
        }
        RECT close{client.right - Scale(38), Scale(4), client.right - Scale(6), Scale(40)};
        text(L"×", close, palette_.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        hits_.push_back({close, SummaryAction::Close, {}});
        line(Scale(43));
        if (detail_) {
            RECT back{Scale(8), Scale(48), Scale(40), Scale(84)};
            text(L"←", back, palette_.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            hits_.push_back({back, SummaryAction::Back, {}});
            const int model_width = std::min<int>(Scale(190), client.right * 42 / 100);
            DrawSubAgentGlyph(device, {Scale(42), Scale(56), Scale(62), Scale(76)}, agent_id_);
            text(title_, {Scale(70), Scale(48), client.right - model_width - Scale(12), Scale(84)},
                 palette_.text, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            text(model_, {client.right - model_width, Scale(48), client.right - Scale(14), Scale(84)},
                 palette_.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            line(Scale(87));
        }
    }
    const int saved = SaveDC(device);
    IntersectClipRect(device, 0, HeaderHeight(), client.right, client.bottom);
    int y = HeaderHeight() + Scale(8) - scroll_;
    for (const auto& row : rows_) {
        const int height = RowHeight(row);
        RECT rect{Scale(12), y, client.right - Scale(12), y + height};
        y += height;
        if (rect.bottom <= HeaderHeight() || rect.top >= client.bottom) continue;
        const bool enabled = (row.kind != SummaryRowKind::Section && row.kind != SummaryRowKind::Empty &&
                              row.kind != SummaryRowKind::Divider) || row.action == SummaryAction::ToggleAgents;
        if (enabled) {
            RECT hit = rect;
            hit.top = std::max<LONG>(hit.top, HeaderHeight());
            hit.bottom = std::min(hit.bottom, client.bottom);
            hits_.push_back({hit, row.action, row.id});
            if (Contains(hit, mouse_)) Fill(device, rect, palette_.selection);
        }
        if (row.kind == SummaryRowKind::Divider) {
            line((rect.top + rect.bottom) / 2);
        } else if (row.kind == SummaryRowKind::Source) {
            const int icon_left = rect.left + Scale(10);
            const int icon_top = rect.top + Scale(14);
            HBITMAP bitmap = row.source_kind == "image" && thumbnail_ ? thumbnail_(row.id) : nullptr;
            if (bitmap) {
                BITMAP info{};
                GetObjectW(bitmap, sizeof(info), &info);
                HDC source = CreateCompatibleDC(device);
                HGDIOBJ old_bitmap = SelectObject(source, bitmap);
                const int edge = Scale(28);
                const double ratio = static_cast<double>(edge) / std::max(1L, std::max(info.bmWidth, info.bmHeight));
                const int w = std::max(1, static_cast<int>(info.bmWidth * ratio));
                const int h = std::max(1, static_cast<int>(info.bmHeight * ratio));
                StretchBlt(device, icon_left, icon_top, w, h, source, 0, 0, info.bmWidth, info.bmHeight, SRCCOPY);
                SelectObject(source, old_bitmap);
                DeleteDC(source);
            } else {
                text(row.source_kind.rfind("web", 0) == 0 || row.source_kind == "url_citation" ? L"◎" : L"▤",
                     {icon_left, icon_top, icon_left + Scale(28), icon_top + Scale(28)},
                     palette_.muted, DT_CENTER | DT_SINGLELINE);
            }
            const int left = rect.left + Scale(50);
            text(row.title, {left, rect.top + Scale(10), rect.right - Scale(10), rect.top + Scale(34)},
                 palette_.text, DT_SINGLELINE | DT_END_ELLIPSIS);
            text(row.detail, {left, rect.top + Scale(36), rect.right - Scale(12), rect.top + Scale(75)},
                 palette_.muted, DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS);
            text(row.note, {left, rect.top + Scale(81), rect.right - Scale(12), rect.bottom - Scale(8)},
                 palette_.muted, DT_SINGLELINE | DT_END_ELLIPSIS);
            line(rect.bottom - Scale(1));
        } else if (row.kind == SummaryRowKind::Agent) {
            const int center = (rect.top + rect.bottom) / 2;
            DrawSubAgentGlyph(device, {rect.left + Scale(12), center - Scale(10),
                                      rect.left + Scale(32), center + Scale(10)}, row.id);
            text(row.title, {rect.left + Scale(46), rect.top, rect.right - Scale(popup_ ? 12 : 85), rect.bottom},
                 palette_.text, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (!popup_) {
                text(RelativeTime(row.timestamp), {rect.right - Scale(84), rect.top, rect.right - Scale(8), rect.bottom},
                     palette_.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        } else {
            int left = rect.left + Scale(12);
            if (row.kind == SummaryRowKind::AgentGroup) {
                for (const auto& id : row.agent_ids) {
                    const int top = (rect.top + rect.bottom - Scale(20)) / 2;
                    DrawSubAgentGlyph(device, {left, top, left + Scale(20), top + Scale(20)}, id);
                    left += Scale(28);
                }
            }
            RECT title{left, rect.top, rect.right - Scale(12), rect.bottom};
            if (row.kind == SummaryRowKind::Section && row.action == SummaryAction::ToggleAgents) {
                RECT measured = title;
                DrawTextW(device, row.title.c_str(), -1, &measured, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
                const int x = std::min(title.right - Scale(8), measured.right + Scale(12));
                const int center = (rect.top + rect.bottom) / 2;
                HPEN pen = CreatePen(PS_SOLID, std::max(1, Scale(1)), palette_.muted);
                HGDIOBJ old_pen = SelectObject(device, pen);
                MoveToEx(device, x - Scale(4), center + Scale(row.expanded ? 2 : -2), nullptr);
                LineTo(device, x, center + Scale(row.expanded ? -2 : 2));
                LineTo(device, x + Scale(4), center + Scale(row.expanded ? 2 : -2));
                SelectObject(device, old_pen);
                DeleteObject(pen);
            }
            text(row.title, title, row.kind == SummaryRowKind::Link ? palette_.text : palette_.muted,
                 DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    RestoreDC(device, saved);
    scroll_thumb_ = {};
    const int viewport = client.bottom - HeaderHeight();
    if (!detail_ && content_height_ > viewport && viewport > 0) {
        const int height = std::max(Scale(28), viewport * viewport / content_height_);
        const int top = HeaderHeight() + static_cast<int>(static_cast<int64_t>(scroll_) *
                                                          (viewport - height) / (content_height_ - viewport));
        scroll_thumb_ = {client.right - Scale(7), top, client.right - Scale(3), top + height};
        Fill(device, scroll_thumb_, palette_.rail_idle);
    }
    SelectObject(device, old_font);
}

LRESULT CALLBACK SummaryView::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* view = reinterpret_cast<SummaryView*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        view = static_cast<SummaryView*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        view->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(view));
    }
    return view ? view->HandleMessage(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT SummaryView::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_NCDESTROY: {
            HWND window = window_;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            window_ = nullptr;
            return DefWindowProcW(window, message, wparam, lparam);
        }
        case WM_ERASEBKGND: return 1;
        case WM_PRINTCLIENT: Paint(reinterpret_cast<HDC>(wparam)); return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC target = BeginPaint(window_, &paint);
            RECT client{};
            GetClientRect(window_, &client);
            HDC buffer = CreateCompatibleDC(target);
            HBITMAP bitmap = CreateCompatibleBitmap(target, std::max(1L, client.right), std::max(1L, client.bottom));
            HGDIOBJ old = SelectObject(buffer, bitmap);
            Paint(buffer);
            BitBlt(target, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
            SelectObject(buffer, old);
            DeleteObject(bitmap);
            DeleteDC(buffer);
            EndPaint(window_, &paint);
            return 0;
        }
        case WM_MOUSEMOVE:
            mouse_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (scroll_dragging_) { DragScroll(mouse_.y); return 0; }
            if (!tracking_) {
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
                TrackMouseEvent(&track);
                tracking_ = true;
            }
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_MOUSELEAVE:
            tracking_ = false;
            mouse_ = {-1, -1};
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL: Scroll(-GET_WHEEL_DELTA_WPARAM(wparam) * Scale(48) / WHEEL_DELTA); return 0;
        case WM_SIZE: Scroll(0); return 0;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                if (popup_) Hide();
                else if (action_) action_(SummaryAction::Close, {});
            } else if (wparam == VK_NEXT || wparam == VK_DOWN) Scroll(Scale(wparam == VK_NEXT ? 240 : 48));
            else if (wparam == VK_PRIOR || wparam == VK_UP) Scroll(-Scale(wparam == VK_PRIOR ? 240 : 48));
            return 0;
        case WM_ACTIVATE:
            if (popup_ && LOWORD(wparam) == WA_INACTIVE) Hide();
            break;
        case WM_LBUTTONDOWN: {
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            RECT target = scroll_thumb_;
            target.left -= Scale(5);
            if (scroll_thumb_.bottom > scroll_thumb_.top && Contains(target, point)) {
                scroll_dragging_ = true;
                scroll_drag_offset_ = point.y - scroll_thumb_.top;
                SetCapture(window_);
                return 0;
            }
            break;
        }
        case WM_CAPTURECHANGED: scroll_dragging_ = false; return 0;
        case WM_LBUTTONUP: {
            if (scroll_dragging_) {
                scroll_dragging_ = false;
                ReleaseCapture();
                return 0;
            }
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            for (const Hit& hit : hits_) {
                if (!Contains(hit.rect, point)) continue;
                const auto action = hit.action;
                const auto id = hit.id;
                if (popup_ && action != SummaryAction::ToggleAgents) Hide();
                if (action_) action_(action, id);
                break;
            }
            return 0;
        }
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}
}  // namespace spark
