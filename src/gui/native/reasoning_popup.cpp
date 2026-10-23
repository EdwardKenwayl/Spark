#include "gui/native/reasoning_popup.h"

#include "gui/native/controls.h"
#include "gui/localization.h"

#include <wx/dcbuffer.h>
#include <wx/display.h>
#include <wx/nonownedwnd.h>
#include <wx/popupwin.h>
#include <wx/sizer.h>
#include <wx/utils.h>
#include <wx/weakref.h>

#include <algorithm>
#include <utility>

namespace spark {
namespace {

class ReasoningPopup final : public wxPopupTransientWindow {
public:
    ReasoningPopup(wxWindow* owner, wxPoint anchor, NativeReasoningPopupState state,
                   NativeReasoningPopupCallback on_change)
        : wxPopupTransientWindow(owner, wxBORDER_NONE | wxFRAME_SHAPED),
          owner_(owner), anchor_(anchor), state_(std::move(state)),
          on_change_(std::move(on_change)),
          colors_(NativeColors(owner->GetForegroundColour().Red() > 180)) {
        SetName("spark.reasoning.popup");
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        canvas_ = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS);
        canvas_->SetName("spark.reasoning.popup.canvas");
        canvas_->SetBackgroundStyle(wxBG_STYLE_PAINT);
        canvas_->SetBackgroundColour(colors_.panel);
        canvas_->SetForegroundColour(colors_.text);
        canvas_->SetFont(owner->GetFont());
        auto* layout = new wxBoxSizer(wxVERTICAL);
        layout->Add(canvas_, 1, wxEXPAND);
        SetSizer(layout);
        canvas_->Bind(wxEVT_PAINT, &ReasoningPopup::Paint, this);
        canvas_->Bind(wxEVT_MOTION, &ReasoningPopup::Motion, this);
        canvas_->Bind(wxEVT_LEFT_DOWN, &ReasoningPopup::Press, this);
        canvas_->Bind(wxEVT_KEY_DOWN, &ReasoningPopup::Key, this);
        Bind(wxEVT_CHAR_HOOK, &ReasoningPopup::Key, this);
        if (owner) owner->Bind(wxEVT_DESTROY, &ReasoningPopup::OwnerDestroyed, this);
        Resize();
    }

    ~ReasoningPopup() override {
        if (owner_) owner_->Unbind(wxEVT_DESTROY, &ReasoningPopup::OwnerDestroyed, this);
    }

    void Open() { Popup(canvas_); }

    void Cancel() {
        if (closing_) return;
        closing_ = true;
        on_change_ = {};
        Dismiss();
        Destroy();
    }

protected:
    void OnDismiss() override {
        if (closing_) return;
        closing_ = true;
        on_change_ = {};
        Destroy();
    }

private:
    int Dip(int value) const { return canvas_->FromDIP(value); }

    wxPoint Point(const wxMouseEvent& event) const {
        auto* source = wxDynamicCast(event.GetEventObject(), wxWindow);
        return canvas_->ScreenToClient(source ? source->ClientToScreen(event.GetPosition())
                                              : wxGetMousePosition());
    }

    void Resize() {
        const int width = Dip(296);
        const int padding = Dip(12);
        const int header_height = Dip(42);
        const int effort_height = Dip(32);
        const int chip_height = Dip(32);
        const int reset_height = Dip(28);
        const int chip_gap = Dip(6);
        const int chip_limit = width - 2 * padding;

        effort_rects_.clear();
        for (size_t index = 0; index < state_.efforts.size(); ++index) {
            const int top = header_height + static_cast<int>(index) * effort_height;
            effort_rects_.push_back({padding, top, width - 2 * padding, effort_height});
        }

        int chip_x = padding;
        int chip_y = header_height + static_cast<int>(std::max(size_t{1}, state_.efforts.size())) * effort_height +
                     Dip(44);
        wxClientDC dc(canvas_);
        dc.SetFont(canvas_->GetFont());
        chip_rects_.clear();
        for (size_t index = 0; index <= state_.efforts.size(); ++index) {
            const wxString label = index == 0 ? T(L"Auto") : U(state_.efforts[index - 1]);
            const int chip_width = std::min(
                chip_limit, std::max(Dip(42), dc.GetTextExtent(label).x + Dip(20)));
            if (chip_x != padding && chip_x + chip_width > width - padding) {
                chip_x = padding;
                chip_y += chip_height + chip_gap;
            }
            chip_rects_.push_back({chip_x, chip_y, chip_width, chip_height});
            chip_x += chip_width + chip_gap;
        }

        const int reset_top = chip_y + chip_height + Dip(8);
        reset_rect_ = {padding, reset_top, width - 2 * padding, reset_height};
        close_rect_ = {width - Dip(36), Dip(6), Dip(30), Dip(30)};
        const int height = reset_top + Dip(36);

        int monitor = wxDisplay::GetFromPoint(anchor_);
        if (monitor == wxNOT_FOUND) monitor = wxDisplay::GetFromWindow(owner_.get());
        if (monitor == wxNOT_FOUND) monitor = 0;
        const wxRect work = wxDisplay(static_cast<unsigned>(monitor)).GetClientArea();
        const int x = std::clamp(anchor_.x, work.x, work.GetRight() + 1 - width);
        const int below = anchor_.y + Dip(6);
        const int y = std::clamp(below + height <= work.GetBottom() + 1
            ? below : anchor_.y - height - Dip(6),
            work.y, std::max(work.y, work.GetBottom() + 1 - height));
        SetSize(x, y, width, height);
        ApplyRoundedPopupShape(this, 8);
        Layout();
    }

    static wxString U(const std::string& value) {
        return wxString::FromUTF8(value.data(), value.size());
    }

    static wxString T(const wchar_t* value) {
        return wxString(i18n::Text(value));
    }

    void Commit() {
        if (on_change_) on_change_(state_);
        canvas_->Refresh(false);
    }

    void ToggleEffort(size_t index) {
        if (index >= state_.efforts.size()) return;
        const auto& effort = state_.efforts[index];
        auto selected = std::find(state_.selected.begin(), state_.selected.end(), effort);
        if (selected == state_.selected.end()) {
            state_.selected.push_back(effort);
        } else {
            state_.selected.erase(selected);
        }
        state_.built_in_defaults = false;
        state_.reasoning = !state_.selected.empty();
        if (std::find(state_.selected.begin(), state_.selected.end(), state_.default_effort) ==
            state_.selected.end())
            state_.default_effort.clear();
        Commit();
    }

    void ChooseDefault(size_t index) {
        if (index == 0) {
            state_.default_effort.clear();
        } else if (index <= state_.efforts.size()) {
            const auto& effort = state_.efforts[index - 1];
            if (std::find(state_.selected.begin(), state_.selected.end(), effort) ==
                state_.selected.end())
                return;
            state_.default_effort = effort;
        }
        state_.built_in_defaults = false;
        Commit();
    }

    void UseBuiltInDefaults() {
        state_.reasoning = true;
        state_.built_in_defaults = true;
        state_.selected = state_.efforts;
        state_.default_effort.clear();
        Commit();
    }

    void Motion(wxMouseEvent& event) {
        const wxPoint point = Point(event);
        int next_effort = -1;
        int next_chip = -1;
        for (size_t index = 0; index < effort_rects_.size(); ++index)
            if (effort_rects_[index].Contains(point)) next_effort = static_cast<int>(index);
        for (size_t index = 0; index < chip_rects_.size(); ++index)
            if (chip_rects_[index].Contains(point)) next_chip = static_cast<int>(index);
        const bool next_reset = reset_rect_.Contains(point);
        if (next_effort != hovered_effort_ || next_chip != hovered_chip_ ||
            next_reset != hovered_reset_) {
            hovered_effort_ = next_effort;
            hovered_chip_ = next_chip;
            hovered_reset_ = next_reset;
            canvas_->Refresh(false);
        }
    }

    void Press(wxMouseEvent& event) {
        const wxPoint point = Point(event);
        if (close_rect_.Contains(point)) {
            Cancel();
            return;
        }
        for (size_t index = 0; index < effort_rects_.size(); ++index)
            if (effort_rects_[index].Contains(point)) {
                ToggleEffort(index);
                return;
            }
        for (size_t index = 0; index < chip_rects_.size(); ++index)
            if (chip_rects_[index].Contains(point)) {
                ChooseDefault(index);
                return;
            }
        if (reset_rect_.Contains(point)) UseBuiltInDefaults();
    }

    void Key(wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_ESCAPE) Cancel();
        else event.Skip();
    }

    void Paint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(canvas_);
        dc.SetBackground(wxBrush(colors_.panel));
        dc.Clear();
        const wxRect bounds = canvas_->GetClientRect();
        dc.SetPen(wxPen(colors_.border));
        dc.SetBrush(wxBrush(colors_.panel));
        dc.DrawRoundedRectangle(bounds, Dip(8));
        dc.SetFont(canvas_->GetFont());

        dc.SetTextForeground(colors_.text);
        dc.DrawText(T(L"Reasoning levels"), Dip(14), Dip(11));
        dc.SetPen(wxPen(colors_.muted, Dip(1)));
        dc.DrawLine(close_rect_.x + Dip(9), close_rect_.y + Dip(9),
                    close_rect_.x + Dip(19), close_rect_.y + Dip(19));
        dc.DrawLine(close_rect_.x + Dip(19), close_rect_.y + Dip(9),
                    close_rect_.x + Dip(9), close_rect_.y + Dip(19));

        for (size_t index = 0; index < effort_rects_.size(); ++index) {
            const wxRect row = effort_rects_[index];
            const bool hovered = static_cast<int>(index) == hovered_effort_;
            if (hovered) {
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(colors_.selection));
                dc.DrawRoundedRectangle(row, Dip(4));
            }
            const wxRect box(row.x + Dip(5), row.y + Dip(7), Dip(16), Dip(16));
            const bool selected = state_.reasoning &&
                std::find(state_.selected.begin(), state_.selected.end(), state_.efforts[index]) !=
                    state_.selected.end();
            dc.SetPen(wxPen(selected ? colors_.text : colors_.muted));
            dc.SetBrush(selected ? wxBrush(colors_.text) : wxBrush(colors_.panel));
            dc.DrawRoundedRectangle(box, Dip(3));
            if (selected) {
                dc.SetPen(wxPen(colors_.panel, Dip(2)));
                dc.DrawLine(box.x + Dip(3), box.y + Dip(8), box.x + Dip(7), box.y + Dip(12));
                dc.DrawLine(box.x + Dip(7), box.y + Dip(12), box.x + Dip(13), box.y + Dip(4));
            }
            dc.SetTextForeground(colors_.text);
            dc.DrawText(U(state_.efforts[index]), row.x + Dip(30), row.y + Dip(6));
        }

        if (state_.efforts.empty()) {
            dc.SetTextForeground(colors_.muted);
            dc.DrawText(T(L"No preset levels"), Dip(14), Dip(48));
        }
        const int heading_y = Dip(42) +
            static_cast<int>(std::max(size_t{1}, state_.efforts.size())) * Dip(32) + Dip(8);
        dc.SetTextForeground(colors_.muted);
        dc.DrawText(T(L"Default level"), Dip(14), heading_y);
        for (size_t index = 0; index < chip_rects_.size(); ++index) {
            const wxRect chip = chip_rects_[index];
            const bool enabled = index == 0 || std::find(
                state_.selected.begin(), state_.selected.end(), state_.efforts[index - 1]) !=
                state_.selected.end();
            const bool selected = index == 0 ? state_.default_effort.empty() :
                state_.default_effort == state_.efforts[index - 1];
            if (static_cast<int>(index) == hovered_chip_) {
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(colors_.selection));
            } else {
                dc.SetPen(wxPen(colors_.border));
                dc.SetBrush(wxBrush(selected ? colors_.selection : colors_.panel));
            }
            dc.DrawRoundedRectangle(chip, Dip(4));
            dc.SetTextForeground(enabled ? colors_.text : colors_.muted);
            const wxString label = index == 0 ? T(L"Auto") : U(state_.efforts[index - 1]);
            dc.DrawText(label, chip.x + Dip(10), chip.y + Dip(5));
        }

        if (hovered_reset_) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(colors_.selection));
        } else {
            dc.SetPen(wxPen(colors_.border));
            dc.SetBrush(wxBrush(colors_.panel));
        }
        dc.DrawRoundedRectangle(reset_rect_, Dip(4));
        dc.SetTextForeground(colors_.text);
        dc.DrawText(T(L"Use built-in defaults"), reset_rect_.x + Dip(10),
                    reset_rect_.y + (reset_rect_.height - dc.GetCharHeight()) / 2);
    }

    void OwnerDestroyed(wxWindowDestroyEvent& event) {
        if (event.GetEventObject() == owner_.get()) Cancel();
        event.Skip();
    }

    wxWeakRef<wxWindow> owner_;
    wxPoint anchor_;
    NativeReasoningPopupState state_;
    NativeReasoningPopupCallback on_change_;
    NativePalette colors_;
    wxPanel* canvas_ = nullptr;
    std::vector<wxRect> effort_rects_;
    std::vector<wxRect> chip_rects_;
    wxRect close_rect_, reset_rect_;
    int hovered_effort_ = -1, hovered_chip_ = -1;
    bool hovered_reset_ = false, closing_ = false;
};

}  // namespace

void ShowNativeReasoningPopup(wxWindow* owner, const wxPoint& screen_anchor,
                              NativeReasoningPopupState state,
                              NativeReasoningPopupCallback on_change) {
    if (!owner || owner->IsBeingDeleted() || wxDisplay::GetCount() == 0) return;
    static wxWeakRef<ReasoningPopup> current;
    if (current) {
        const bool toggle_off = current->GetParent() == owner && !current->IsBeingDeleted();
        current->Cancel();
        if (toggle_off) return;
    }
    auto* popup = new ReasoningPopup(owner, screen_anchor, std::move(state),
                                     std::move(on_change));
    current = popup;
    popup->Open();
}

}  // namespace spark
