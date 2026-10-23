#include "gui/native/controls.h"
#include "platform/clock.h"

#include <wx/dc.h>

#include <algorithm>
#include <cmath>

namespace spark {

void NativeList::DrawSidebarRow(wxDC& dc, const wxRect& rect, size_t index) const {
    if (index >= labels_.size()) return;
    const bool dark = GetBackgroundColour().Red() < 128;
    const auto palette = NativeColors(dark);
    const bool hovered = static_cast<int>(index) == hovered_;
    const bool active = hovered || IsSelected(index);
    const auto kind = kinds_[index];
    const bool project = kind == NativeListKind::ProjectExpanded || kind == NativeListKind::ProjectCollapsed ||
                         kind == NativeListKind::ProjectEmpty;
    const bool more = kind == NativeListKind::More;
    const bool branch = kind == NativeListKind::ProjectChat;
    const wxColour ink = active ? wxColour(250, 250, 250) : more ? palette.muted : palette.text;
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(active ? dark ? palette.selection : wxColour(42, 43, 46) : GetBackgroundColour()));
    dc.DrawRectangle(rect);
    dc.SetFont(GetFont());
    dc.SetTextForeground(ink);
    dc.SetPen(wxPen(active ? ink : palette.muted));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    const int y = rect.y + rect.height / 2, right = rect.x + rect.width;
    const auto line = [&](int x1, int y1, int x2, int y2) {
        dc.DrawLine(rect.x + FromDIP(x1), y + FromDIP(y1), rect.x + FromDIP(x2), y + FromDIP(y2));
    };
    int left = rect.x + FromDIP(10), reserve = FromDIP(8);
    if (project) {
        if (kind == NativeListKind::ProjectExpanded) { line(6, -2, 9, 1); line(9, 1, 12, -2); }
        else { line(7, -3, 10, 0); line(10, 0, 7, 3); }
        line(17, 6, 17, -6); line(17, -6, 22, -6); line(22, -6, 25, -3);
        line(25, -3, 32, -3); line(32, -3, 32, 6); line(32, 6, 17, 6);
        left = rect.x + FromDIP(38);
        reserve = FromDIP(hovered ? 66 : 10);
    } else if (branch || (more && GetName() == "spark.projects.list")) {
        left = rect.x + FromDIP(30);
        if (branch) {
            dc.SetPen(wxPen(active ? ink : dark ? wxColour(112, 115, 122) : wxColour(135, 138, 144)));
            line(16, 0, 24, 0);
        }
        reserve = FromDIP(hovered && !more ? 96 : 10);
    } else if (more) reserve = FromDIP(10);
    else if (hovered) reserve = FromDIP(96);
    if (running_[index]) reserve += FromDIP(26);
    const auto text = wxControl::Ellipsize(labels_[index], dc, wxELLIPSIZE_END,
                                          std::max(1, right - left - reserve));
    dc.DrawLabel(text, {left, rect.y, std::max(1, right - left - reserve), rect.height},
                 wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
    if (running_[index]) {
        const int x = right - FromDIP(hovered ? 104 : 20), radius = FromDIP(5);
        const double angle = MonotonicMillis() / 160.0;
        dc.SetPen(wxPen(ink, std::max(1, FromDIP(1))));
        dc.DrawArc(x + static_cast<int>(radius * std::cos(angle)), y + static_cast<int>(radius * std::sin(angle)),
                   x + static_cast<int>(radius * std::cos(angle + 4.7)),
                   y + static_cast<int>(radius * std::sin(angle + 4.7)), x, y);
    }
    if (!hovered || more) return;
    dc.SetPen(wxPen(ink));
    if (project) {
        const int dots = right - FromDIP(58), pencil = right - FromDIP(27);
        dc.SetBrush(wxBrush(ink));
        for (int offset : {-5, 0, 5}) dc.DrawCircle(dots, y + FromDIP(offset), std::max(1, FromDIP(1)));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(pencil - FromDIP(8), y - FromDIP(7), FromDIP(13), FromDIP(14));
        dc.DrawLine(pencil - FromDIP(1), y + FromDIP(5), pencil + FromDIP(9), y - FromDIP(5));
        dc.DrawLine(pencil + FromDIP(9), y - FromDIP(5), pencil + FromDIP(6), y - FromDIP(8));
        dc.DrawLine(pencil + FromDIP(6), y - FromDIP(8), pencil - FromDIP(4), y + FromDIP(2));
        dc.DrawLine(pencil - FromDIP(4), y + FromDIP(2), pencil - FromDIP(1), y + FromDIP(5));
    } else {
        const int trash = right - FromDIP(78), pin = right - FromDIP(50), archive = right - FromDIP(22);
        dc.DrawRectangle(trash - FromDIP(5), y - FromDIP(4), FromDIP(10), FromDIP(10));
        dc.DrawLine(trash - FromDIP(7), y - FromDIP(5), trash + FromDIP(7), y - FromDIP(5));
        dc.DrawLine(trash - FromDIP(2), y - FromDIP(7), trash + FromDIP(2), y - FromDIP(7));
        for (const int offset : {-2, 2})
            dc.DrawLine(trash + FromDIP(offset), y - FromDIP(1), trash + FromDIP(offset), y + FromDIP(3));
        dc.DrawEllipse(pin - FromDIP(5), y - FromDIP(7), FromDIP(10), FromDIP(6));
        dc.DrawLine(pin - FromDIP(7), y - FromDIP(1), pin + FromDIP(7), y - FromDIP(1));
        dc.DrawLine(pin, y - FromDIP(1), pin, y + FromDIP(7));
        dc.DrawLine(pin - FromDIP(3), y + FromDIP(7), pin + FromDIP(3), y + FromDIP(7));
        dc.DrawRectangle(archive - FromDIP(7), y - FromDIP(6), FromDIP(14), FromDIP(4));
        dc.DrawRectangle(archive - FromDIP(5), y - FromDIP(2), FromDIP(10), FromDIP(9));
        dc.DrawLine(archive - FromDIP(3), y + FromDIP(1), archive + FromDIP(3), y + FromDIP(1));
    }
}

}  // namespace spark
