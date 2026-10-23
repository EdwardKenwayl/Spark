#include "gui/native/model_picker.h"
#include "gui/native/controls.h"

#include <wx/dcbuffer.h>
#include <wx/display.h>
#include <wx/evtloop.h>
#include <wx/nonownedwnd.h>
#include <wx/popupwin.h>
#include <wx/thread.h>
#include <wx/utils.h>
#include <wx/weakref.h>

#include <algorithm>
#include <memory>
#include <utility>

namespace spark {
namespace {
struct PickerSelection {
    wxEventLoop* loop = nullptr;
    int command = 0;
    bool done = false, running = false;
    void Finish(int selected) {
        if (done) return;
        done = true;
        command = selected;
        if (running && loop) loop->ScheduleExit();
    }
};
class ModelPopup final : public wxPopupTransientWindow {
public:
    ModelPopup(wxWindow* owner, wxPoint anchor, const wxRect& work,
               std::vector<NativeModelPickerRow> rows, int checked,
               std::shared_ptr<PickerSelection> selection)
        : wxPopupTransientWindow(owner, wxBORDER_NONE | wxFRAME_SHAPED), owner_(owner), rows_(std::move(rows)),
          selection_(std::move(selection)), colors_(NativeColors(owner->GetForegroundColour().Red() > 180)),
          checked_(checked) {
        SetName("spark.model-picker");
        canvas_ = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS);
        canvas_->SetName("spark.model-picker.canvas");
        canvas_->SetFont(owner->GetFont());
        canvas_->SetBackgroundStyle(wxBG_STYLE_PAINT);
        canvas_->SetBackgroundColour(colors_.panel);
        if (!rows_.empty() && rows_.back().kind == NativeModelPickerKind::Reasoning) {
            footer_ = std::move(rows_.back());
            rows_.pop_back();
        }
        const int footer_height = footer_.command ? Dip(40) : 0;
        visible_ = std::max(1, std::min({12, static_cast<int>(rows_.size()),
            (work.height - Dip(24) - footer_height) / Dip(32)}));
        const int width = std::min(Dip(380), work.width);
        const int height = visible_ * Dip(32) + Dip(8) + footer_height;
        for (int index = 0; index < Count(); ++index) {
            if (rows_[index].kind == NativeModelPickerKind::Model && cursor_ < 0) cursor_ = index;
            if (rows_[index].command == checked_) { cursor_ = index; break; }
        }
        top_ = std::clamp(cursor_ - visible_ / 2, 0, std::max(0, Count() - visible_));
        SetSize(std::clamp(anchor.x, work.x, work.x + work.width - width),
                std::clamp(anchor.y - height, work.y, std::max(work.y, work.y + work.height - height)),
                width, height);
        ApplyRoundedPopupShape(this, 8);
        canvas_->SetSize(GetClientRect());
        canvas_->Bind(wxEVT_PAINT, &ModelPopup::Paint, this);
        canvas_->Bind(wxEVT_LEFT_DOWN, &ModelPopup::Press, this);
        canvas_->Bind(wxEVT_LEFT_UP, &ModelPopup::Release, this);
        canvas_->Bind(wxEVT_MOTION, &ModelPopup::Motion, this);
        canvas_->Bind(wxEVT_RIGHT_DOWN, [this](wxMouseEvent&) { Close(0); });
        canvas_->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
            if (hovered_ != -1) { hovered_ = -1; canvas_->Refresh(false); }
        });
        canvas_->Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) {
            const int delta = event.GetWheelDelta();
            if (!delta) return;
            wheel_ += event.GetWheelRotation();
            Scroll(top_ - wheel_ / delta * std::min(visible_, std::max(1, event.GetLinesPerAction())));
            wheel_ %= delta;
        });
        canvas_->Bind(wxEVT_KEY_DOWN, &ModelPopup::Key, this);
        canvas_->Bind(wxEVT_CHAR, [](wxKeyEvent&) {});
        Bind(wxEVT_KEY_DOWN, &ModelPopup::Key, this);
        Bind(wxEVT_CHAR, [](wxKeyEvent&) {});
        owner->Bind(wxEVT_DESTROY, &ModelPopup::OwnerDestroyed, this);
    }
    ~ModelPopup() override {
        if (owner_) owner_->Unbind(wxEVT_DESTROY, &ModelPopup::OwnerDestroyed, this);
        selection_->Finish(0);
    }
    wxWindow* Canvas() const { return canvas_; }
    bool ProcessLeftDown(wxMouseEvent& event) override { Press(event); return true; }
private:
    int Dip(int value) const { return canvas_->FromDIP(value); }
    int Count() const { return static_cast<int>(rows_.size()); }
    wxPoint Point(const wxMouseEvent& event) const {
        auto* source = wxDynamicCast(event.GetEventObject(), wxWindow);
        return source ? canvas_->ScreenToClient(source->ClientToScreen(event.GetPosition())) : event.GetPosition();
    }
    wxRect Thumb() const {
        if (Count() <= visible_) return {};
        const int track = visible_ * Dip(32);
        const int height = std::max(Dip(24), track * visible_ / Count());
        const int y = Dip(4) + top_ * (track - height) / (Count() - visible_);
        return {canvas_->GetClientSize().x - Dip(12), y, Dip(8), height};
    }
    int Hit(wxPoint point) const {
        if (!canvas_->GetClientRect().Contains(point)) return -1;
        if (point.y >= Dip(4) + visible_ * Dip(32)) return footer_.command ? Count() : -1;
        if (point.y < Dip(4) || (!Thumb().IsEmpty() && point.x >= canvas_->GetClientSize().x - Dip(16)))
            return -1;
        const int index = top_ + (point.y - Dip(4)) / Dip(32);
        return index < Count() ? index : -1;
    }
    void Scroll(int position) {
        const int next = std::clamp(position, 0, std::max(0, Count() - visible_));
        if (top_ == next) return;
        top_ = next;
        hovered_ = -1;
        canvas_->Refresh(false);
    }
    void Press(wxMouseEvent& event) {
        const auto point = Point(event);
        if (!canvas_->GetClientRect().Contains(point)) { Close(0); return; }
        const auto thumb = Thumb();
        rail_press_ = !thumb.IsEmpty() && point.x >= thumb.x - Dip(4) &&
                      point.y < Dip(4) + visible_ * Dip(32);
        if (!rail_press_) return;
        if (thumb.Contains(point)) {
            drag_y_ = point.y;
            drag_top_ = top_;
        } else Scroll(top_ + (point.y < thumb.y ? -visible_ : visible_));
    }
    void Release(wxMouseEvent& event) {
        const bool rail = rail_press_;
        rail_press_ = false;
        drag_y_ = -1;
        if (rail) return;
        const int hit = Hit(Point(event));
        if (hit == Count() && footer_.command) Close(footer_.command);
        else if (hit >= 0 && hit < Count() && rows_[hit].kind == NativeModelPickerKind::Model)
            Close(rows_[hit].command);
    }
    void Motion(wxMouseEvent& event) {
        const auto point = Point(event);
        if (drag_y_ >= 0) {
            Scroll(drag_top_ + (point.y - drag_y_) * (Count() - visible_) /
                std::max(1, visible_ * Dip(32) - Thumb().height));
            return;
        }
        const int hit = Hit(point);
        if (hovered_ != hit) { hovered_ = hit; canvas_->Refresh(false); }
    }
    void Key(wxKeyEvent& event) {
        const int key = event.GetKeyCode();
        if (key == WXK_ESCAPE) { Close(0); return; }
        if (key == WXK_RETURN) {
            if (cursor_ >= 0 && cursor_ < Count() && rows_[cursor_].kind == NativeModelPickerKind::Model)
                Close(rows_[cursor_].command);
            return;
        }
        if (key != WXK_UP && key != WXK_DOWN && key != WXK_HOME && key != WXK_END &&
            key != WXK_PAGEUP && key != WXK_PAGEDOWN) return;
        const int direction = key == WXK_UP || key == WXK_HOME || key == WXK_PAGEUP ? -1 : 1;
        int next = key == WXK_HOME ? 0 : key == WXK_END ? Count() - 1 :
            std::clamp(cursor_ + direction * (key == WXK_PAGEUP || key == WXK_PAGEDOWN ? visible_ : 1),
                       0, Count() - 1);
        while (next >= 0 && next < Count() && rows_[next].kind != NativeModelPickerKind::Model)
            next += direction;
        if (next < 0 || next >= Count()) return;
        cursor_ = next;
        if (cursor_ < top_) Scroll(cursor_);
        else if (cursor_ >= top_ + visible_) Scroll(cursor_ - visible_ + 1);
        hovered_ = -1;
        canvas_->Refresh(false);
    }
    void Close(int command) {
        // The transient popup owns the platform grab, including during scrollbar drags.
        drag_y_ = -1;
        rail_press_ = false;
        selection_->Finish(command);
        Dismiss();
    }
    void OnDismiss() override {
        drag_y_ = -1;
        rail_press_ = false;
        selection_->Finish(0);
    }
    void OwnerDestroyed(wxWindowDestroyEvent& event) {
        if (event.GetEventObject() == owner_.get()) Close(0);
        event.Skip();
    }
    void Paint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(canvas_);
        dc.SetBackground(wxBrush(colors_.panel)); dc.Clear();
        dc.SetFont(canvas_->GetFont());
        dc.SetPen(wxPen(colors_.border)); dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(canvas_->GetClientRect());
        const auto text = [&](const wxString& value, const wxRect& rect, wxColour ink) {
            dc.SetTextForeground(ink);
            dc.DrawLabel(wxControl::Ellipsize(value, dc, wxELLIPSIZE_END, std::max(1, rect.width)),
                         rect, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
        };
        const int width = canvas_->GetClientSize().x;
        for (int index = top_; index < std::min(Count(), top_ + visible_); ++index) {
            const auto& row = rows_[index];
            const bool heading = row.kind == NativeModelPickerKind::Header;
            const wxRect bounds(Dip(4), Dip(4) + (index - top_) * Dip(32), width - Dip(20), Dip(32));
            if (!heading && (index == hovered_ || (hovered_ < 0 && index == cursor_))) {
                dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush(colors_.selection));
                dc.DrawRectangle(bounds);
            }
            text(row.label, {bounds.x + Dip(14), bounds.y, std::max(1, bounds.width - Dip(40)), bounds.height},
                 heading ? colors_.muted : colors_.text);
            if (!heading && row.command == checked_) {
                dc.SetPen(wxPen(colors_.text, std::max(1, Dip(2))));
                const int x = width - Dip(36), y = bounds.y + bounds.height / 2;
                dc.DrawLine(x - Dip(4), y, x, y + Dip(4));
                dc.DrawLine(x, y + Dip(4), x + Dip(7), y - Dip(6));
            }
        }
        const auto thumb = Thumb();
        if (!thumb.IsEmpty()) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(colors_.text.Red() > 180 ? wxColour(112, 115, 122) : wxColour(135, 138, 144)));
            dc.DrawRectangle(thumb);
        }
        if (footer_.command) {
            const wxRect footer(Dip(4), Dip(8) + visible_ * Dip(32), width - Dip(8), Dip(36));
            if (hovered_ == Count()) {
                dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush(colors_.selection));
                dc.DrawRectangle(footer);
            }
            dc.SetPen(wxPen(colors_.border));
            dc.DrawLine(Dip(8), footer.y - Dip(4), width - Dip(8), footer.y - Dip(4));
            text(footer_.label, {footer.x + Dip(14), footer.y, std::max(1, footer.width - Dip(14)), footer.height},
                 colors_.text);
        }
    }
    wxWeakRef<wxWindow> owner_;
    wxPanel* canvas_ = nullptr;
    std::vector<NativeModelPickerRow> rows_;
    NativeModelPickerRow footer_{0, {}, NativeModelPickerKind::Reasoning};
    std::shared_ptr<PickerSelection> selection_;
    NativePalette colors_;
    int checked_, visible_ = 1, cursor_ = -1, top_ = 0, hovered_ = -1, wheel_ = 0;
    int drag_y_ = -1, drag_top_ = 0;
    bool rail_press_ = false;
};
}  // namespace

int ShowNativeModelPicker(wxWindow* owner, const wxPoint& anchor,
                          std::vector<NativeModelPickerRow> rows, int checked) {
    if (!wxIsMainThread() || !owner || owner->IsBeingDeleted() || rows.empty()) return 0;
    // Up to 4096 model targets, one heading per target, and the reasoning footer.
    if (rows.size() > 8193) rows.resize(8193);
    for (auto& row : rows) {
        row.label = row.label.Left(512);
        row.label.Replace("\r", " "); row.label.Replace("\n", " "); row.label.Replace("\t", " ");
    }
    if (std::none_of(rows.begin(), rows.end(), [](const auto& row) { return row.kind == NativeModelPickerKind::Model; }))
        return 0;
    int display = wxDisplay::GetFromPoint(anchor);
    if (display == wxNOT_FOUND) display = wxDisplay::GetFromWindow(owner);
    if (display == wxNOT_FOUND) return 0;
    const auto work = wxDisplay(display).GetClientArea();
    if (work.width <= 0 || work.height <= 0) return 0;
    wxWeakRef<wxWindow> focus(wxWindow::FindFocus()), owner_guard(owner);
    auto selection = std::make_shared<PickerSelection>();
    wxEventLoop loop;
    selection->loop = &loop;
    auto* popup = new ModelPopup(owner, anchor, work, std::move(rows), checked, selection);
    wxWeakRef<ModelPopup> guard(popup);
    popup->Popup(popup->Canvas());
    if (!selection->done) {
        selection->running = true;
        loop.Run();
    }
    selection->running = false;
    selection->loop = nullptr;
    selection->Finish(0);
    if (guard && !guard->IsBeingDeleted()) { guard->Dismiss(); guard->Destroy(); }
    if (owner_guard && !owner_guard->IsBeingDeleted() && focus && !focus->IsBeingDeleted() &&
        focus->IsShownOnScreen() && focus->IsEnabled() &&
        wxGetActiveWindow() == wxGetTopLevelParent(owner_guard.get()))
        focus->SetFocus();
    return selection->command;
}

}  // namespace spark
