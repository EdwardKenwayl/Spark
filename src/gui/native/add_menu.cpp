#include "gui/native/add_menu.h"
#include "gui/native/controls.h"
#include "gui/localization.h"

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
wxString T(const wchar_t* text) { return wxString(i18n::Text(text)); }
wxString SingleLine(wxString text) {
    text = text.Left(512);
    text.Replace("\r", " "); text.Replace("\n", " "); text.Replace("\t", " ");
    return text;
}
struct Selection {
    wxEventLoop* loop = nullptr;
    NativeAddAction action = NativeAddAction::None;
    bool done = false, running = false;
    void Finish(NativeAddAction selected) {
        if (done) return;
        done = true;
        action = selected;
        if (running && loop) loop->ScheduleExit();
    }
};

class AddPopup final : public wxPopupTransientWindow {
public:
    AddPopup(wxWindow* owner, const wxRect& composer, const wxRect& work,
             wxString snapshot, wxString goal, bool plan, std::shared_ptr<Selection> selection)
        : wxPopupTransientWindow(owner, wxBORDER_NONE | wxFRAME_SHAPED), owner_(owner),
          snapshot_(SingleLine(std::move(snapshot))), goal_(SingleLine(std::move(goal))), plan_(plan),
          selection_(std::move(selection)),
          colors_(NativeColors(owner->GetForegroundColour().Red() > 180)) {
        SetName("spark.add-menu");
        canvas_ = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS);
        canvas_->SetName("spark.add-menu.canvas");
        canvas_->SetFont(owner->GetFont());
        canvas_->SetForegroundColour(colors_.text);
        canvas_->SetBackgroundColour(colors_.panel);
        canvas_->SetBackgroundStyle(wxBG_STYLE_PAINT);
        canvas_->Bind(wxEVT_PAINT, &AddPopup::Paint, this);
        canvas_->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) { Hover(Hit(event.GetPosition())); });
        canvas_->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { Hover(-1); });
        canvas_->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) { Select(Hit(event.GetPosition())); });
        canvas_->Bind(wxEVT_RIGHT_DOWN, [this](wxMouseEvent&) { Close(NativeAddAction::None); });
        canvas_->Bind(wxEVT_KEY_DOWN, &AddPopup::Key, this);
        canvas_->Bind(wxEVT_CHAR, [](wxKeyEvent&) {});
        Bind(wxEVT_KEY_DOWN, &AddPopup::Key, this);
        Bind(wxEVT_CHAR, [](wxKeyEvent&) {});
        owner->Bind(wxEVT_DESTROY, &AddPopup::OwnerDestroyed, this);
        const int width = std::clamp(composer.width, 1, work.width);
        const int height = Dip(208);
        SetSize(std::clamp(composer.x, work.x, work.x + work.width - width),
                std::clamp(composer.y - height - Dip(6), work.y,
                           std::max(work.y, work.y + work.height - height)), width, height);
        ApplyRoundedPopupShape(this, 8);
        canvas_->SetSize(GetClientRect());
    }
    ~AddPopup() override {
        if (owner_) owner_->Unbind(wxEVT_DESTROY, &AddPopup::OwnerDestroyed, this);
        selection_->Finish(NativeAddAction::None);
    }
    wxWindow* Canvas() const { return canvas_; }
    bool ProcessLeftDown(wxMouseEvent& event) override {
        auto* source = wxDynamicCast(event.GetEventObject(), wxWindow);
        const auto point = source ? canvas_->ScreenToClient(source->ClientToScreen(event.GetPosition()))
                                  : event.GetPosition();
        if (!canvas_->GetClientRect().Contains(point)) Close(NativeAddAction::None);
        return true;
    }
private:
    int Dip(int value) const { return canvas_->FromDIP(value); }
    wxRect Row(int index) const {
        return {Dip(7), Dip(32 + index * 42), std::max(1, canvas_->GetClientSize().x - Dip(14)), Dip(42)};
    }
    int Hit(wxPoint point) const {
        for (int index = 0; index < 4; ++index)
            if (Row(index).Contains(point)) return index == 1 && snapshot_.empty() ? -1 : index;
        return -1;
    }
    void Hover(int index) {
        if (hovered_ == index) return;
        const int old = hovered_;
        hovered_ = index;
        if (old >= 0) canvas_->RefreshRect(Row(old), false);
        if (index >= 0) canvas_->RefreshRect(Row(index), false);
    }
    void Select(int index) {
        if (index < 0 || index > 3 || (index == 1 && snapshot_.empty())) return;
        Close(static_cast<NativeAddAction>(index));
    }
    void Key(wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_ESCAPE) Close(NativeAddAction::None);
        else if (event.GetKeyCode() == WXK_UP || event.GetKeyCode() == WXK_DOWN) {
            const int step = event.GetKeyCode() == WXK_UP ? 3 : 1;
            int next = hovered_;
            do { next = (next + step + 4) % 4; } while (next == 1 && snapshot_.empty());
            Hover(next);
        } else if (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_SPACE)
            Select(hovered_ < 0 ? 0 : hovered_);
    }
    void Close(NativeAddAction action) { selection_->Finish(action); Dismiss(); }
    void OnDismiss() override { selection_->Finish(NativeAddAction::None); }
    void OwnerDestroyed(wxWindowDestroyEvent& event) {
        if (event.GetEventObject() == owner_.get()) Close(NativeAddAction::None);
        event.Skip();
    }
    void Paint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(canvas_);
        dc.SetBackground(wxBrush(colors_.panel)); dc.Clear();
        dc.SetBrush(*wxTRANSPARENT_BRUSH); dc.SetPen(wxPen(colors_.border));
        dc.DrawRoundedRectangle(canvas_->GetClientRect(), Dip(8));
        dc.SetFont(canvas_->GetFont());
        const auto text = [&](const wxString& label, const wxRect& bounds, wxColour ink) {
            dc.SetTextForeground(ink);
            dc.DrawLabel(wxControl::Ellipsize(label, dc, wxELLIPSIZE_END, std::max(1, bounds.width)),
                         bounds, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
        };
        text(T(L"Add"), {Dip(16), Dip(7), std::max(1, canvas_->GetClientSize().x - Dip(32)), Dip(22)},
             colors_.muted);
        const wxString titles[] = {T(L"Files and folders"), T(L"Attach smart snapshot"),
                                   T(L"Goal"), T(L"Plan mode")};
        const wxString descriptions[] = {wxString{}, snapshot_,
            goal_.empty() ? T(L"Set a continuing objective") : goal_,
            plan_ ? T(L"Enabled") : T(L"Enable plan mode")};
        for (int index = 0; index < 4; ++index) {
            const wxRect row = Row(index);
            if (index == hovered_) {
                dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush(colors_.selection));
                dc.DrawRoundedRectangle(row, Dip(4));
            }
            const bool disabled = index == 1 && snapshot_.empty();
            const wxColour ink = disabled ? wxColour(
                (colors_.text.Red() + 3 * colors_.panel.Red()) / 4,
                (colors_.text.Green() + 3 * colors_.panel.Green()) / 4,
                (colors_.text.Blue() + 3 * colors_.panel.Blue()) / 4) : colors_.text;
            dc.SetPen(wxPen(ink, std::max(1, Dip(1)))); dc.SetBrush(*wxTRANSPARENT_BRUSH);
            const int x = row.x + Dip(21), y = row.y + row.height / 2;
            const auto line = [&](int a, int b, int c, int d) {
                dc.DrawLine(x + Dip(a), y + Dip(b), x + Dip(c), y + Dip(d));
            };
            if (index == 0) {
                dc.DrawRoundedRectangle(x - Dip(5), y - Dip(10), Dip(10), Dip(18), Dip(4));
                line(1, -6, 1, 3); line(1, 3, -2, 3); line(-2, 3, -2, -4);
            } else if (index == 1) {
                dc.DrawRectangle(x - Dip(9), y - Dip(7), Dip(18), Dip(14));
                line(-5, 3, 0, -2); line(0, -2, 6, 3);
                dc.DrawEllipse(x + Dip(3), y - Dip(5), Dip(3), Dip(3));
            } else if (index == 2) {
                dc.DrawEllipse(x - Dip(9), y - Dip(9), Dip(18), Dip(18));
                dc.DrawEllipse(x - Dip(5), y - Dip(5), Dip(10), Dip(10));
                line(0, 0, 10, -10);
            } else {
                dc.DrawEllipse(x - Dip(6), y - Dip(10), Dip(12), Dip(12));
                dc.DrawRectangle(x - Dip(3), y + Dip(2), Dip(6), Dip(5));
                line(-2, 10, 2, 10);
            }
            wxRect label(row.x + Dip(42), row.y, std::max(1, row.width - Dip(80)), row.height);
            text(titles[index], label, ink);
            const int title_width = dc.GetTextExtent(titles[index]).x + Dip(12);
            label.x += title_width;
            label.width -= title_width;
            if (label.width > 0) text(descriptions[index], label, colors_.muted);
            if ((index == 2 && !goal_.empty()) || (index == 3 && plan_)) {
                dc.SetPen(wxPen(colors_.text, std::max(1, Dip(2))));
                const int right = row.x + row.width;
                dc.DrawLine(right - Dip(24), y, right - Dip(20), y + Dip(4));
                dc.DrawLine(right - Dip(20), y + Dip(4), right - Dip(13), y - Dip(5));
            }
        }
    }
    wxWeakRef<wxWindow> owner_;
    wxPanel* canvas_ = nullptr;
    wxString snapshot_, goal_;
    bool plan_;
    int hovered_ = -1;
    std::shared_ptr<Selection> selection_;
    NativePalette colors_;
};
}  // namespace

NativeAddAction ShowNativeAddMenu(wxWindow* owner, const wxRect& composer,
                                  const wxString& snapshot_title, const wxString& goal, bool plan) {
    if (!wxIsMainThread() || !owner || owner->IsBeingDeleted()) return NativeAddAction::None;
    int display = wxDisplay::GetFromPoint(composer.GetTopLeft());
    if (display == wxNOT_FOUND) display = wxDisplay::GetFromWindow(owner);
    if (display == wxNOT_FOUND) return NativeAddAction::None;
    const wxRect work = wxDisplay(display).GetClientArea();
    if (work.width <= 0 || work.height <= 0) return NativeAddAction::None;
    wxWeakRef<wxWindow> focus(wxWindow::FindFocus()), owner_guard(owner);
    auto selection = std::make_shared<Selection>();
    wxEventLoop loop;
    selection->loop = &loop;
    auto* popup = new AddPopup(owner, composer, work, snapshot_title, goal, plan, selection);
    wxWeakRef<AddPopup> guard(popup);
    popup->Popup(popup->Canvas());
    if (!selection->done) {
        selection->running = true;
        loop.Run();
    }
    selection->running = false;
    selection->loop = nullptr;
    selection->Finish(NativeAddAction::None);
    if (guard && !guard->IsBeingDeleted()) { guard->Dismiss(); guard->Destroy(); }
    if (owner_guard && !owner_guard->IsBeingDeleted() && focus && !focus->IsBeingDeleted() &&
        focus->IsShownOnScreen() && focus->IsEnabled() &&
        wxGetActiveWindow() == wxGetTopLevelParent(owner_guard.get()))
        focus->SetFocus();
    return selection->action;
}

}  // namespace spark
