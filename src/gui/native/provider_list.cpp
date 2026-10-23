#include "gui/native/provider_list.h"

#include "gui/native/controls.h"

#include <wx/dcclient.h>
#include <wx/settings.h>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace spark {
namespace {

bool IsDark(const wxColour& color) {
    return color.Red() < 128 && color.Green() < 128 && color.Blue() < 128;
}

wxString Ellipsize(wxDC& dc, const wxString& text, int width) {
    return wxControl::Ellipsize(text, dc, wxELLIPSIZE_END, std::max(1, width));
}

void DrawProviderAction(wxDC& dc, const wxRect& rect, int action, const wxColour& color) {
    const auto x = [&](int value) { return rect.x + value * rect.width / 24; };
    const auto y = [&](int value) { return rect.y + value * rect.height / 24; };
    const auto line = [&](int a, int b, int c, int d) { dc.DrawLine(x(a), y(b), x(c), y(d)); };
    dc.SetPen(wxPen(color, std::max(1, rect.width / 12)));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    if (action == 0) {
        line(14, 2, 22, 10);
        const wxPoint points[] = {
            {x(16), y(4)}, {x(3), y(17)}, {x(3), y(20)},
            {x(6), y(22)}, {x(9), y(21)}, {x(20), y(8)},
        };
        dc.DrawLines(static_cast<int>(std::size(points)), points);
        line(8, 12, 14, 14);
    } else if (action == 1) {
        const wxPoint points[] = {
            {x(5), y(15)}, {x(16), y(4)}, {x(20), y(8)},
            {x(9), y(19)}, {x(4), y(20)}, {x(5), y(15)},
        };
        dc.DrawLines(static_cast<int>(std::size(points)), points);
        line(14, 6, 18, 10);
        line(13, 21, 21, 21);
    } else if (action == 2) {
        wxRect copy(x(9), y(9), x(21) - x(9), y(21) - y(9));
        dc.DrawRoundedRectangle(copy, std::max(1, rect.width / 12));
        line(5, 15, 3, 15);
        line(3, 15, 3, 3);
        line(3, 3, 15, 3);
        line(15, 3, 15, 5);
    } else {
        line(3, 6, 21, 6);
        line(9, 6, 9, 3);
        line(9, 3, 15, 3);
        line(15, 3, 15, 6);
        line(6, 6, 7, 21);
        line(7, 21, 17, 21);
        line(17, 21, 18, 6);
        line(10, 10, 10, 17);
        line(14, 10, 14, 17);
    }
}

}  // namespace

NativeProviderList::NativeProviderList(wxWindow* parent)
    : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {
    SetItemCount(0);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        const bool was_compact = compact_;
        UpdateDensity();
        if (was_compact != compact_) RefreshAll();
        event.Skip();
    });
    Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
        const wxPoint point = event.GetPosition();
        if (pressed_row_ != wxNOT_FOUND && drag_armed_ && event.LeftIsDown()) {
            if (!dragging_ &&
                (std::abs(point.x - press_point_.x) >= wxSystemSettings::GetMetric(wxSYS_DRAG_X, this) ||
                 std::abs(point.y - press_point_.y) >= wxSystemSettings::GetMetric(wxSYS_DRAG_Y, this))) {
                dragging_ = true;
            }
            if (dragging_) {
                const int row = RowAt(point);
                int next = static_cast<int>(providers_.size());
                if (row >= 0) {
                    wxRect rect;
                    rect = GetItemRect(static_cast<size_t>(row));
                    next = row + (point.y >= rect.y + rect.height / 2 ? 1 : 0);
                } else if (point.y < 0) {
                    ScrollLines(-1);
                } else if (point.y >= GetClientSize().y) {
                    ScrollLines(1);
                }
                if (next != drop_index_) {
                    drop_index_ = next;
                    RefreshAll();
                }
            }
        } else {
            UpdateHover(point);
        }
        event.Skip();
    });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
        if (dragging_) return;
        UpdateHover(wxPoint(-1, -1));
    });
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
        SetFocus();
        const wxPoint point = event.GetPosition();
        const int row = RowAt(point);
        if (row >= 0) {
            wxRect rect;
            rect = GetItemRect(static_cast<size_t>(row));
            rect.height -= FromDIP(8);
            if (IsGrip(rect, point)) {
                pressed_row_ = row;
                pressed_action_ = wxNOT_FOUND;
                drag_armed_ = true;
                press_point_ = point;
                CaptureMouse();
                return;
            }
            const int action = ActionAt(rect, point);
            if (action >= 0) {
                pressed_row_ = row;
                pressed_action_ = action;
                drag_armed_ = false;
                hovered_action_ = action;
                press_point_ = point;
                CaptureMouse();
                return;
            }
        }
        event.Skip();
    });
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
        if (pressed_row_ == wxNOT_FOUND) {
            event.Skip();
            return;
        }
        FinishPointer(event.GetPosition());
    });
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        pressed_row_ = wxNOT_FOUND;
        pressed_action_ = wxNOT_FOUND;
        drag_armed_ = false;
        dragging_ = false;
        drop_index_ = wxNOT_FOUND;
        RefreshAll();
    });
}

void NativeProviderList::SetProviders(std::vector<ProviderCard> providers) {
    providers_ = std::move(providers);
    SetItemCount(providers_.size());
    hovered_row_ = hovered_action_ = pressed_row_ = pressed_action_ = drop_index_ = wxNOT_FOUND;
    drag_armed_ = dragging_ = false;
    RefreshAll();
}

void NativeProviderList::SetActionHandler(std::function<void(size_t, ProviderListAction)> handler) {
    action_handler_ = std::move(handler);
}

void NativeProviderList::SetReorderHandler(std::function<void(size_t, size_t)> handler) {
    reorder_handler_ = std::move(handler);
}

void NativeProviderList::SetActionTooltips(const std::array<wxString, 4>& tooltips,
                                           const wxString& reorder_tooltip) {
    tooltips_ = tooltips;
    reorder_tooltip_ = reorder_tooltip;
    UpdateHover(wxPoint(-1, -1));
}

void NativeProviderList::UpdateDensity() {
    const int scrollbar = std::max(0, wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, this));
    compact_ = GetClientSize().x - scrollbar < FromDIP(480);
}

int NativeProviderList::RowAt(const wxPoint& point) const {
    if (point.x < 0 || point.y < 0) return wxNOT_FOUND;
    const int row = VirtualHitTest(point.y);
    if (row < 0 || row >= static_cast<int>(providers_.size())) return wxNOT_FOUND;
    wxRect rect;
    rect = GetItemRect(static_cast<size_t>(row));
    rect.height -= FromDIP(8);
    return rect.Contains(point) ? row : wxNOT_FOUND;
}

bool NativeProviderList::IsGrip(const wxRect& row, const wxPoint& point) const {
    return point.x >= row.x && point.x < row.x + FromDIP(38) && row.Contains(point);
}

int NativeProviderList::ActionAt(const wxRect& row, const wxPoint& point) const {
    for (int action = 0; action < 4; ++action)
        if (ActionRect(row, action).Contains(point)) return action;
    return wxNOT_FOUND;
}

wxRect NativeProviderList::ActionRect(const wxRect& row, int action) const {
    if (action < 0 || action > 3) return {};
    const int size = FromDIP(32);
    const int left = row.GetRight() - FromDIP(12 + (4 - action) * 32);
    const int top = compact_ ? row.GetBottom() - FromDIP(34)
                             : row.y + (row.height - size) / 2;
    return {left, top, size, size};
}

void NativeProviderList::UpdateHover(const wxPoint& point) {
    const int row = RowAt(point);
    int action = wxNOT_FOUND;
    if (row >= 0) {
        wxRect rect;
        rect = GetItemRect(static_cast<size_t>(row));
        rect.height -= FromDIP(8);
        if (!IsGrip(rect, point)) action = ActionAt(rect, point);
    }
    if (row == hovered_row_ && action == hovered_action_) return;
    if (hovered_row_ >= 0) RefreshRow(static_cast<size_t>(hovered_row_));
    if (row >= 0 && row != hovered_row_) RefreshRow(static_cast<size_t>(row));
    hovered_row_ = row;
    hovered_action_ = action;
    wxString tooltip;
    if (row >= 0) {
        wxRect rect;
        rect = GetItemRect(static_cast<size_t>(row));
        rect.height -= FromDIP(8);
        tooltip = IsGrip(rect, point) ? reorder_tooltip_ : action >= 0 ? tooltips_[action] : wxString{};
    }
    SetToolTip(tooltip);
}

void NativeProviderList::FinishPointer(const wxPoint& point) {
    const int source = pressed_row_;
    const bool was_dragging = dragging_;
    const int destination = drop_index_;
    int action = wxNOT_FOUND;
    if (!was_dragging && pressed_action_ >= 0 &&
        source >= 0 && source < static_cast<int>(providers_.size())) {
        wxRect rect;
        rect = GetItemRect(static_cast<size_t>(source));
        rect.height -= FromDIP(8);
        if (RowAt(point) == source && ActionRect(rect, pressed_action_).Contains(point))
            action = pressed_action_;
    }
    pressed_row_ = wxNOT_FOUND;
    pressed_action_ = wxNOT_FOUND;
    drag_armed_ = false;
    dragging_ = false;
    drop_index_ = wxNOT_FOUND;
    if (HasCapture()) ReleaseMouse();
    RefreshAll();
    if (was_dragging && destination >= 0 && source >= 0) {
        const int target = destination - (destination > source ? 1 : 0);
        if (target >= 0 && target < static_cast<int>(providers_.size()) && target != source && reorder_handler_)
            reorder_handler_(static_cast<size_t>(source), static_cast<size_t>(target));
    } else if (action >= 0 && action_handler_) {
        action_handler_(static_cast<size_t>(source), static_cast<ProviderListAction>(action));
    }
    UpdateHover(point);
}

wxCoord NativeProviderList::OnMeasureItem(size_t) const {
    return FromDIP(compact_ ? 112 : 88);
}

void NativeProviderList::OnDrawBackground(wxDC& dc, const wxRect& rect, size_t) const {
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(GetBackgroundColour()));
    dc.DrawRectangle(rect);
}

void NativeProviderList::OnDrawItem(wxDC& dc, const wxRect& rect, size_t index) const {
    if (index >= providers_.size()) return;
    const auto palette = NativeColors(IsDark(GetBackgroundColour()));
    wxRect card = rect;
    card.height -= FromDIP(8);
    dc.SetPen(wxPen(palette.border, FromDIP(1)));
    dc.SetBrush(wxBrush(palette.panel));
    dc.DrawRoundedRectangle(card, FromDIP(8));

    const auto& provider = providers_[index];
    wxRect avatar(card.x + FromDIP(42), card.y + FromDIP(22), FromDIP(36), FromDIP(36));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(palette.selection));
    dc.DrawRectangle(avatar);
    dc.SetFont(GetFont().Bold());
    dc.SetTextForeground(palette.text);
    const wxString initial = provider.name.empty() ? "?" : provider.name.Left(1).Upper();
    dc.DrawLabel(initial, avatar, wxALIGN_CENTER);

    const bool hovered = hovered_row_ == static_cast<int>(index);
    const int reserved = hovered && !compact_ ? FromDIP(152) : FromDIP(16);
    wxRect name_rect(avatar.GetRight() + FromDIP(12), card.y + FromDIP(14),
                     std::max(1, card.GetRight() - avatar.GetRight() - FromDIP(12) - reserved),
                     FromDIP(26));
    dc.SetFont(GetFont());
    dc.SetTextForeground(palette.text);
    dc.DrawText(Ellipsize(dc, provider.name, name_rect.width), name_rect.x, name_rect.y);
    wxString protocol = provider.protocol;
    if (protocol == "responses") protocol = "Responses API";
    else if (protocol == "chat_completions") protocol = "Chat Completions API";
    const wxString detail = protocol + " · " + provider.endpoint;
    name_rect.y += FromDIP(30);
    name_rect.height = FromDIP(24);
    dc.SetTextForeground(palette.muted);
    dc.DrawText(Ellipsize(dc, detail, name_rect.width), name_rect.x, name_rect.y);

    if (hovered) {
        const wxColour danger = IsDark(GetBackgroundColour())
            ? wxColour(242, 128, 128) : wxColour(190, 54, 54);
        for (int action = 0; action < 4; ++action) {
            wxRect button = ActionRect(card, action);
            const bool hot = hovered_action_ == action;
            if (hot) {
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(palette.selection));
                dc.DrawRectangle(button);
            }
            DrawProviderAction(dc, button.Deflate(FromDIP(7)), action,
                               hot && action == 3 ? danger : hot ? palette.text : palette.muted);
        }
    }

    if (dragging_ && drop_index_ == static_cast<int>(index)) {
        dc.SetPen(wxPen(palette.text, FromDIP(2)));
        dc.DrawLine(card.x + FromDIP(8), card.y, card.GetRight() - FromDIP(8), card.y);
    }
    if (dragging_ && drop_index_ == static_cast<int>(providers_.size()) &&
        index + 1 == providers_.size()) {
        dc.SetPen(wxPen(palette.text, FromDIP(2)));
        dc.DrawLine(card.x + FromDIP(8), card.GetBottom(), card.GetRight() - FromDIP(8), card.GetBottom());
    }

    dc.SetPen(wxPen(palette.muted, FromDIP(1)));
    for (int row = -1; row <= 1; ++row)
        for (int column = 0; column < 2; ++column)
            dc.DrawCircle(card.x + FromDIP(19 + column * 6),
                          card.y + card.height / 2 + FromDIP(row * 6), FromDIP(1));
}

}  // namespace spark
