#include "gui/native/summary_popup.h"
#include "gui/native/controls.h"
#include "gui/native/summary_panel.h"

#include <wx/dcbuffer.h>
#include <wx/display.h>
#include <wx/eventfilter.h>
#include <wx/nonownedwnd.h>
#include <wx/popupwin.h>
#include <wx/sizer.h>
#include <wx/utils.h>
#include <wx/weakref.h>
#include <algorithm>
#include <array>
#include <utility>

namespace spark {
namespace {
class SummaryPopup final : public wxPopupTransientWindow, private wxEventFilter {
public:
    SummaryPopup(wxWindow* owner, wxPoint anchor, const std::vector<SubAgentInfo>& agents,
                 const std::vector<SourceReference>& sources, NativeSummaryPopupCallback callback)
        : wxPopupTransientWindow(owner, wxBORDER_NONE | wxFRAME_SHAPED), owner_(owner),
          anchor_(anchor), callback_(std::move(callback)) {
        SetName("spark.summary.popup");
        dark_ = owner->GetForegroundColour().Red() > 180;
        SetBackgroundColour(NativeColors(dark_).background);
        NativeSummaryPanel::Callbacks actions;
        actions.close = [this] { Cancel(); };
        actions.active_child = [this](const std::string& id) {
            if (!id.empty()) Finish(NativeSummaryPopupAction::Agent, id);
        };
        actions.show_sources = [this] { Finish(NativeSummaryPopupAction::Sources, {}); };
        actions.popup_size_changed = [this](const wxSize& size) { Resize(size); };
        panel_ = new NativeSummaryPanel(this, std::move(actions));
        panel_->SetFont(owner->GetFont());
        panel_->SetPopupMode(true);
        panel_->SetDark(dark_);
        panel_->SetAgents(agents);
        panel_->SetSources(sources);
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(panel_, 1, wxEXPAND); SetSizer(sizer);
        // Border pieces overlay, not inset, preserving Windows row coordinates.
        for (auto*& edge : frame_) {
            edge = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
            edge->SetBackgroundStyle(wxBG_STYLE_PAINT);
            edge->Bind(wxEVT_PAINT, [this, edge](wxPaintEvent&) {
                wxAutoBufferedPaintDC dc(edge); const auto p = NativeColors(dark_);
                dc.SetBackground(wxBrush(p.background)); dc.Clear();
                dc.SetPen(wxPen(p.border)); dc.SetBrush(wxBrush(p.background));
                const auto at = edge->GetPosition();
                dc.DrawRoundedRectangle(-at.x, -at.y, GetClientSize().x, GetClientSize().y, FromDIP(8));
            });
        }
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { PlaceFrame(); event.Skip(); });
        Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
            if (event.GetKeyCode() == WXK_ESCAPE) Cancel();
            else event.Skip();
        });
        Resize(panel_->PopupSize());
        owner->Bind(wxEVT_DESTROY, &SummaryPopup::OwnerDestroyed, this);
        wxEvtHandler::AddFilter(this);
    }
    ~SummaryPopup() override {
        wxEvtHandler::RemoveFilter(this);
        if (owner_) owner_->Unbind(wxEVT_DESTROY, &SummaryPopup::OwnerDestroyed, this);
    }
    bool Closing() const { return closing_; }
    bool WaitingForRelease() const { return waiting_release_; }
    void Open() { Popup(panel_); }
    void UpdateContent(const std::vector<SubAgentInfo>& agents, const std::vector<SourceReference>& sources) {
        // The next periodic refresh retries after release without invalidating the pressed row.
        if (closing_ || panel_->InteractionActive()) return;
        panel_->SetAgents(agents);
        panel_->SetSources(sources);
        Resize(panel_->PopupSize());
    }
    void Cancel() {
        if (destroy_requested_) return;
        closing_ = true; waiting_release_ = false; callback_ = {};
        Dismiss(); destroy_requested_ = true; Destroy();
    }
protected:
    bool ProcessLeftDown(wxMouseEvent& event) override {
        auto* source = wxDynamicCast(event.GetEventObject(), wxWindow);
        dismiss_from_press_ = source &&
            !GetScreenRect().Contains(source->ClientToScreen(event.GetPosition()));
        return false;
    }
    void OnDismiss() override {
        if (closing_) return;
        closing_ = true; callback_ = {};
        // wx can repost the outside press to the summary button. Keep the
        // closed instance through that release so Show consumes the toggle.
        waiting_release_ = dismiss_from_press_ || wxGetMouseState().LeftIsDown();
        if (!waiting_release_) Cancel();
    }
private:
    int FilterEvent(wxEvent& event) override {
        const auto type = event.GetEventType();
        if (waiting_release_ && (type == wxEVT_LEFT_UP || type == wxEVT_KEY_DOWN ||
            (type == wxEVT_LEFT_DOWN && event.GetTimestamp() != last_press_))) {
            if (type != wxEVT_LEFT_UP) waiting_release_ = false;
            const wxWeakRef<SummaryPopup> weak(this);
            CallAfter([weak] { if (weak) weak->Cancel(); });
        }
        if (type == wxEVT_LEFT_DOWN) last_press_ = event.GetTimestamp();
        return Event_Skip;
    }
    void OwnerDestroyed(wxWindowDestroyEvent& event) {
        if (event.GetEventObject() == owner_.get()) Cancel();
        event.Skip();
    }
    void Finish(NativeSummaryPopupAction action, const std::string& id) {
        if (closing_) return;
        closing_ = true;
        const auto owner = owner_;
        auto callback = std::move(callback_);
        const std::string selected = id;
        Dismiss(); destroy_requested_ = true; Destroy();
        if (owner && !owner->IsBeingDeleted() && callback) callback(action, selected);
    }
    void Resize(wxSize desired) {
        if (closing_ || !owner_ || owner_->IsBeingDeleted() || wxDisplay::GetCount() == 0) return;
        int monitor = wxDisplay::GetFromPoint(anchor_);
        if (monitor == wxNOT_FOUND) monitor = wxDisplay::GetFromWindow(owner_.get());
        if (monitor == wxNOT_FOUND) monitor = 0;
        const auto work = wxDisplay(static_cast<unsigned>(monitor)).GetClientArea();
        if (work.width <= 0 || work.height <= 0) return;
        desired.x = std::clamp(desired.x, 1, work.width);
        desired.y = std::clamp(desired.y, 1, std::max(1, work.height - FromDIP(24)));
        const int x = std::clamp(anchor_.x - desired.x, work.x, work.x + work.width - desired.x);
        const int y = std::clamp(anchor_.y + FromDIP(6), work.y, work.y + work.height - desired.y);
        if (GetRect() == wxRect(x, y, desired.x, desired.y)) return;
        SetSize(x, y, desired.x, desired.y);
        ApplyRoundedPopupShape(this, 8);
        Layout(); PlaceFrame();
    }
    void PlaceFrame() {
        if (!frame_[0]) return;
        const auto size = GetClientSize();
        const int radius = std::max(1, std::min({FromDIP(8), size.x / 2, size.y / 2}));
        const int width = std::max(1, size.x), height = std::max(1, size.y);
        const wxRect edges[] = {
            {radius, 0, std::max(0, width - 2 * radius), 1},
            {radius, height - 1, std::max(0, width - 2 * radius), 1},
            {0, radius, 1, std::max(0, height - 2 * radius)},
            {width - 1, radius, 1, std::max(0, height - 2 * radius)},
            {0, 0, radius, radius}, {width - radius, 0, radius, radius},
            {0, height - radius, radius, radius}, {width - radius, height - radius, radius, radius}};
        for (size_t i = 0; i < frame_.size(); ++i) {
            frame_[i]->SetSize(edges[i]); frame_[i]->Raise(); frame_[i]->Refresh(false);
        }
    }
    wxWeakRef<wxWindow> owner_;
    wxPoint anchor_;
    NativeSummaryPopupCallback callback_;
    NativeSummaryPanel* panel_ = nullptr;
    std::array<wxPanel*, 8> frame_{};
    long last_press_ = 0;
    bool dark_ = true, closing_ = false, waiting_release_ = false;
    bool dismiss_from_press_ = false, destroy_requested_ = false;
};
wxWeakRef<SummaryPopup> current;
}  // namespace

bool NativeSummaryPopupShown(wxWindow* owner) {
    return owner && !owner->IsBeingDeleted() && current && current->GetParent() == owner &&
           !current->Closing() && current->IsShown();
}

void RefreshNativeSummaryPopup(wxWindow* owner, const std::vector<SubAgentInfo>& agents,
                               const std::vector<SourceReference>& sources) {
    if (NativeSummaryPopupShown(owner)) current->UpdateContent(agents, sources);
}

void DismissNativeSummaryPopup(wxWindow* owner) {
    if (current && current->GetParent() == owner) current->Cancel();
}

void ShowNativeSummaryPopup(wxWindow* owner, const wxPoint& screen_anchor,
                            const std::vector<SubAgentInfo>& agents,
                            const std::vector<SourceReference>& sources,
                            NativeSummaryPopupCallback on_select) {
    if (!owner || owner->IsBeingDeleted() || wxDisplay::GetCount() == 0) return;
    if (current) {
        const bool toggle_off = current->GetParent() == owner &&
                               (!current->Closing() || current->WaitingForRelease());
        current->Cancel();
        if (toggle_off) return;
    }
    auto* popup = new SummaryPopup(owner, screen_anchor, agents, sources, std::move(on_select));
    current = popup; popup->Open();
}

}  // namespace spark
