#pragma once

#include <wx/control.h>

namespace spark {
class NativeList;

class NativeSidebarRail : public wxControl {
public:
    NativeSidebarRail(wxWindow* parent, NativeList* projects, NativeList* history);
    void Sync();
    void ScrollBy(int lines);
private:
    struct Metrics {
        int project_max = 0, history_max = 0, top = 0, count = 0, visible = 0;
        int maximum = 0, track = 0, thumb_top = 0, thumb_height = 0;
    };
    Metrics Measure() const;
    void ScrollTo(int top);
    void ScrollAt(int y);
    void Paint(wxPaintEvent&);
    NativeList *projects_, *history_;
    bool dragging_ = false, hovered_ = false;
    int last_top_ = -1, last_maximum_ = -1, last_height_ = -1, last_visible_ = -1;
};
}  // namespace spark
