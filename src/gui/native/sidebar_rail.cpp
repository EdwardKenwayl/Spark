#include "gui/native/sidebar_rail.h"
#include "gui/native/controls.h"

#include <wx/dcbuffer.h>
#include <algorithm>

namespace spark {

NativeSidebarRail::NativeSidebarRail(wxWindow* parent, NativeList* projects, NativeList* history)
    : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
      projects_(projects), history_(history) {
    SetName("spark.sidebar.scrollbar");
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &NativeSidebarRail::Paint, this);
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
        dragging_ = true; CaptureMouse(); ScrollAt(event.GetY()); Refresh();
    });
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        dragging_ = false; if (HasCapture()) ReleaseMouse(); Refresh();
    });
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { dragging_ = false; Refresh(); });
    Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
        if (!hovered_) { hovered_ = true; Refresh(); }
        if (dragging_) ScrollAt(event.GetY());
    });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hovered_ = false; Refresh(); });
    const auto wheel = [this](wxMouseEvent& event) {
        const int delta = event.GetWheelDelta();
        if (delta) ScrollBy(-event.GetWheelRotation() / delta * std::max(1, event.GetLinesPerAction()));
    };
    Bind(wxEVT_MOUSEWHEEL, wheel);
    projects_->Bind(wxEVT_MOUSEWHEEL, wheel);
    history_->Bind(wxEVT_MOUSEWHEEL, wheel);
}

NativeSidebarRail::Metrics NativeSidebarRail::Measure() const {
    Metrics result;
    const auto measure = [&](NativeList* list, int& maximum, int& top) {
        if (!list->IsShown() || list->GetClientSize().y <= 0) return;
        const int count = static_cast<int>(list->GetItemCount());
        const int visible = std::max(1, list->GetClientSize().y / FromDIP(34));
        maximum = std::max(0, count - visible);
        top = std::min(maximum, static_cast<int>(list->GetVisibleRowsBegin()));
        result.count += count; result.visible += visible;
    };
    int project_top = 0, history_top = 0;
    measure(projects_, result.project_max, project_top);
    measure(history_, result.history_max, history_top);
    result.maximum = result.project_max + result.history_max;
    result.top = history_top > 0 ? result.project_max + history_top : project_top;
    result.track = std::max(1, GetClientSize().y - FromDIP(8));
    result.thumb_height = std::min(result.track, std::max(FromDIP(26),
        result.track * std::max(1, result.visible) / std::max(1, result.count)));
    result.thumb_top = FromDIP(4) + (result.maximum ?
        (result.track - result.thumb_height) * result.top / result.maximum : 0);
    return result;
}

void NativeSidebarRail::Sync() {
    const auto m = Measure();
    const bool needed = m.maximum > 0;
    if (IsShown() != needed) Show(needed);
    if (m.top != last_top_ || m.maximum != last_maximum_ ||
        GetSize().y != last_height_ || m.visible != last_visible_) {
        last_top_ = m.top; last_maximum_ = m.maximum;
        last_height_ = GetSize().y; last_visible_ = m.visible;
        Refresh();
    }
}

void NativeSidebarRail::ScrollTo(int top) {
    const auto m = Measure();
    top = std::clamp(top, 0, m.maximum);
    if (projects_->GetItemCount()) projects_->ScrollToRow(std::min(top, m.project_max));
    if (history_->GetItemCount())
        history_->ScrollToRow(std::min(m.history_max, std::max(0, top - m.project_max)));
    Sync();
}
void NativeSidebarRail::ScrollBy(int lines) { ScrollTo(Measure().top + lines); }
void NativeSidebarRail::ScrollAt(int y) {
    const auto m = Measure();
    ScrollTo(std::max(0, y - FromDIP(4) - m.thumb_height / 2) * m.maximum /
        std::max(1, m.track - m.thumb_height));
}
void NativeSidebarRail::Paint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour())); dc.Clear();
    const auto m = Measure();
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(hovered_ || dragging_ ? wxColour(135, 135, 135) : wxColour(92, 92, 92)));
    dc.DrawRoundedRectangle(FromDIP(2), m.thumb_top, std::max(1, GetSize().x - FromDIP(4)),
                            m.thumb_height, FromDIP(3));
}
}  // namespace spark
