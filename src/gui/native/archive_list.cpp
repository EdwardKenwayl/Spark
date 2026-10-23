#include "gui/native/archive_list.h"

#include "gui/native/controls.h"

#include <wx/dcclient.h>

#include <algorithm>

namespace spark {

NativeArchiveList::NativeArchiveList(wxWindow* parent)
    : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {
    // Initialize visible rows before a book control can transfer focus to this list.
    SetItemCount(0);
    SetName("spark.archives.list");
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_MOTION, [this](wxMouseEvent& event) { Hover(event.GetPosition()); event.Skip(); });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { Hover({-1, -1}); });
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
        if (!IsEnabled()) return;
        SetFocus();
        pressed_ = RowAt(event.GetPosition());
        pressed_action_ = 0;
        if (pressed_ >= 0 && !rows_[pressed_].header) {
            wxRect rect;
            rect = GetItemRect(pressed_);
            pressed_action_ = ActionAt(rect, event.GetPosition());
            if (pressed_action_) CaptureMouse();
        }
    });
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
        const int row = pressed_, action = pressed_action_;
        pressed_ = wxNOT_FOUND;
        pressed_action_ = 0;
        if (HasCapture()) ReleaseMouse();
        if (!IsEnabled() || row < 0 || RowAt(event.GetPosition()) != row || !action) return;
        if (ActionAt(GetItemRect(row), event.GetPosition()) == action) Activate(row, action == 1);
    });
    Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent& event) {
        const int row = RowAt(event.GetPosition());
        if (row >= 0 && !ActionAt(GetItemRect(row), event.GetPosition())) Activate(row, true);
    });
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        pressed_ = wxNOT_FOUND;
        pressed_action_ = 0;
    });
    Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
        switch (event.GetKeyCode()) {
        case WXK_HOME:
        case WXK_NUMPAD_HOME:
            if (GetItemCount() > 0) ScrollToRow(0);
            break;
        case WXK_END:
        case WXK_NUMPAD_END:
            if (GetItemCount() > 0) ScrollRows(static_cast<int>(GetItemCount()));
            break;
        case WXK_DOWN:
        case WXK_NUMPAD_DOWN:
            ScrollRows(1);
            break;
        case WXK_UP:
        case WXK_NUMPAD_UP:
            ScrollRows(-1);
            break;
        case WXK_PAGEDOWN:
        case WXK_NUMPAD_PAGEDOWN:
            ScrollRowPages(1);
            break;
        case WXK_PAGEUP:
        case WXK_NUMPAD_PAGEUP:
            ScrollRowPages(-1);
            break;
        case WXK_RETURN:
        case WXK_NUMPAD_ENTER:
        case WXK_DELETE:
        case WXK_SPACE:
            break;
        default:
            event.Skip();
            return;
        }
    });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { Hover({-1, -1}); RefreshAll(); event.Skip(); });
    Bind(wxEVT_SCROLLWIN_THUMBTRACK, [this](wxScrollWinEvent& event) { Hover({-1, -1}); event.Skip(); });
    Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) { Hover({-1, -1}); event.Skip(); });
}

void NativeArchiveList::SetRows(std::vector<ArchiveListRow> rows) {
    if (HasCapture()) ReleaseMouse();
    rows_ = std::move(rows);
    hovered_ = pressed_ = wxNOT_FOUND;
    hovered_action_ = pressed_action_ = 0;
    UnsetToolTip();
    SetSelection(wxNOT_FOUND);
    SetItemCount(rows_.size());
    RefreshAll();
}

void NativeArchiveList::SetLabels(const wxString& unarchive, const wxString& remove) {
    unarchive_ = unarchive;
    remove_ = remove;
    wxClientDC dc(this);
    dc.SetFont(GetFont());
    restore_width_ = std::max(FromDIP(92), dc.GetTextExtent(unarchive_).x + FromDIP(20));
    RefreshAll();
}

void NativeArchiveList::SetActionHandler(std::function<void(size_t, bool)> handler) {
    action_ = std::move(handler);
}

wxCoord NativeArchiveList::OnMeasureItem(size_t index) const {
    return FromDIP(index < rows_.size() && rows_[index].header ? 42 : 64);
}

int NativeArchiveList::RowAt(const wxPoint& point) const {
    return GetClientRect().Contains(point) ? VirtualHitTest(point.y) : wxNOT_FOUND;
}

wxRect NativeArchiveList::ActionRect(const wxRect& row, bool restore) const {
    const int right = row.x + row.width - FromDIP(12);
    const int left = right - restore_width_;
    return restore ? wxRect(left, row.y + FromDIP(11), restore_width_, row.height - FromDIP(22))
                   : wxRect(left - FromDIP(36), row.y, FromDIP(36), row.height);
}

int NativeArchiveList::ActionAt(const wxRect& row, const wxPoint& point) const {
    const int restore_right = row.x + row.width - FromDIP(12);
    const int restore_left = restore_right - restore_width_;
    if (point.x >= restore_left && point.x <= restore_right) return 1;
    return ActionRect(row, false).Contains(point) ? 2 : 0;
}

void NativeArchiveList::Hover(const wxPoint& point) {
    const int row = RowAt(point);
    int action = 0;
    if (row >= 0 && !rows_[row].header) {
        wxRect rect;
        rect = GetItemRect(row);
        action = ActionAt(rect, point);
    }
    if (row == hovered_ && action == hovered_action_) return;
    const int previous = hovered_;
    hovered_ = row;
    hovered_action_ = action;
    if (previous >= 0) RefreshRow(previous);
    if (row >= 0) {
        RefreshRow(row);
        SetToolTip(action == 1 ? unarchive_ : action == 2 ? remove_ : rows_[row].tooltip);
    } else UnsetToolTip();
    SetCursor(action && IsEnabled() ? wxCURSOR_HAND : wxCURSOR_ARROW);
}

void NativeArchiveList::Activate(int row, bool restore) {
    if (!IsEnabled() || row < 0 || static_cast<size_t>(row) >= rows_.size() || rows_[row].header) return;
    const auto callback = action_;
    const size_t entry = rows_[row].entry;
    if (callback) callback(entry, restore);
}

void NativeArchiveList::OnDrawBackground(wxDC& dc, const wxRect& rect, size_t index) const {
    const auto palette = NativeColors(GetBackgroundColour().Red() < 128);
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(index < rows_.size() && !rows_[index].header &&
                       static_cast<int>(index) == hovered_ ? palette.input : palette.background));
    dc.DrawRectangle(rect);
}

void NativeArchiveList::OnDrawItem(wxDC& dc, const wxRect& rect, size_t index) const {
    if (index >= rows_.size()) return;
    const auto& row = rows_[index];
    const auto palette = NativeColors(GetBackgroundColour().Red() < 128);
    dc.SetFont(GetFont());
    dc.SetTextForeground(IsEnabled() ? palette.text : palette.muted);
    const auto text = [&](const wxString& value, const wxRect& area, int alignment) {
        if (area.width <= 0) return;
        dc.DrawLabel(wxControl::Ellipsize(value, dc, wxELLIPSIZE_END, area.width), area, alignment);
    };
    if (row.header) {
        wxRect icon(rect.x + FromDIP(10), rect.y + FromDIP(12), FromDIP(18), FromDIP(18));
        dc.SetPen(wxPen(palette.muted));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        const int left = icon.x + FromDIP(1);
        const int right = icon.x + icon.width - FromDIP(1);
        const int top = icon.y + FromDIP(2);
        const int bottom = icon.y + icon.height - FromDIP(2);
        dc.DrawLine(left, top + FromDIP(3), left + FromDIP(5), top + FromDIP(3));
        dc.DrawLine(left + FromDIP(5), top + FromDIP(3), left + FromDIP(7), top);
        dc.DrawLine(left + FromDIP(7), top, right - FromDIP(5), top);
        dc.DrawLine(right - FromDIP(5), top, right - FromDIP(3), top + FromDIP(3));
        dc.DrawLine(right - FromDIP(3), top + FromDIP(3), right, top + FromDIP(3));
        dc.DrawLine(right, top + FromDIP(3), right, bottom);
        dc.DrawLine(right, bottom, left, bottom);
        dc.DrawLine(left, bottom, left, top + FromDIP(3));
        const int count_width = dc.GetTextExtent(row.detail).x;
        text(row.title, {rect.x + FromDIP(36), rect.y,
                         rect.width - count_width - FromDIP(60), rect.height},
             wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
        dc.SetTextForeground(palette.muted);
        text(row.detail, {rect.GetRight() - count_width - FromDIP(8), rect.y, count_width, rect.height},
             wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL);
        return;
    }
    const auto restore = ActionRect(rect, true);
    const auto remove = ActionRect(rect, false);
    const int title_width = remove.x - rect.x - FromDIP(32);
    text(row.title, {rect.x + FromDIP(16), rect.y + FromDIP(8), title_width, FromDIP(24)},
         wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
    dc.SetTextForeground(palette.muted);
    text(row.detail, {rect.x + FromDIP(16), rect.y + FromDIP(33), title_width, FromDIP(24)},
         wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
    const bool hovered = static_cast<int>(index) == hovered_ && IsEnabled();
    dc.SetPen(wxPen(hovered && hovered_action_ == 2 ? wxColour(224, 90, 90) : palette.muted));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    const int x = remove.x + FromDIP(8), y = rect.y + FromDIP(20);
    const int left = x + FromDIP(3), right = x + FromDIP(17), top = y + FromDIP(4);
    dc.DrawLine(left - FromDIP(2), top, right + FromDIP(2), top);
    dc.DrawLine(left + FromDIP(2), top - FromDIP(2), right - FromDIP(2), top - FromDIP(2));
    dc.DrawLine(left, top + FromDIP(1), left + FromDIP(1), y + FromDIP(18));
    dc.DrawLine(left + FromDIP(1), y + FromDIP(18), right - FromDIP(1), y + FromDIP(18));
    dc.DrawLine(right - FromDIP(1), y + FromDIP(18), right, top + FromDIP(1));
    dc.DrawLine((left + right) / 2, top + FromDIP(2), (left + right) / 2, y + FromDIP(16));
    dc.SetPen(wxPen(palette.border));
    dc.SetBrush(wxBrush(hovered && hovered_action_ == 1 ? palette.selection : palette.input));
    dc.DrawRoundedRectangle(restore, FromDIP(5));
    dc.SetTextForeground(IsEnabled() ? palette.text : palette.muted);
    text(unarchive_, restore, wxALIGN_CENTER);
    dc.SetPen(wxPen(palette.border));
    dc.DrawLine(rect.x + FromDIP(12), rect.GetBottom(), rect.GetRight() - FromDIP(12), rect.GetBottom());
}

}  // namespace spark
