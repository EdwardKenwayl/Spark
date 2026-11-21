#include "gui/native/completion_notification.h"
#include "gui/native/controls.h"

#include <wx/dcbuffer.h>
#include <wx/display.h>
#include <wx/dynlib.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <algorithm>
#include <utility>

namespace spark {

class NativeCompletionNotifications::Card final : public wxFrame {
public:
    Card(NativeCompletionNotifications* manager, wxFrame* owner, const CompletionNotice& notice, bool dark)
        : wxFrame(nullptr, wxID_ANY, wxString(notice.title), wxDefaultPosition, wxDefaultSize,
                  wxBORDER_NONE | wxFRAME_NO_TASKBAR | wxFRAME_TOOL_WINDOW | wxSTAY_ON_TOP),
          manager_(manager), path_(notice.path), body_(notice.body), timer_(this), dark_(dark) {
        SetName("spark.completion.notification");
        SetHelpText(body_);
        SetFont(owner->GetFont());
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetClientSize(FromDIP(wxSize(356, 136)));
        Bind(wxEVT_PAINT, &Card::Paint, this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { if (manager_) manager_->Dismiss(this, false); });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { if (manager_) manager_->Dismiss(this, false); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
            pressed_ = CloseBounds().Contains(event.GetPosition()) ? Press::Close : Press::Body;
            CaptureMouse();
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
            const auto pressed = pressed_;
            pressed_ = Press::None;
            if (HasCapture()) ReleaseMouse();
            if (manager_ && pressed != Press::None && GetClientRect().Contains(event.GetPosition()) &&
                (pressed == Press::Close) == CloseBounds().Contains(event.GetPosition()))
                manager_->Dismiss(this, pressed == Press::Body);
        });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { pressed_ = Press::None; });
    }
    ~Card() override { timer_.Stop(); if (manager_) manager_->Forget(this); }
    bool AcceptsFocus() const override { return false; }
    bool AcceptsFocusFromKeyboard() const override { return false; }
    const std::string& Path() const { return path_; }
    void Open() {
#ifdef __WXGTK__
        // wx's focus-on-map hint protects showing; also refuse focus on clicks.
        const auto accept_focus = reinterpret_cast<void (*)(void*, int)>(
            wxDynamicLibrary::RawGetSymbol(wxDynamicLibrary::GetProgramHandle(), "gtk_window_set_accept_focus"));
        if (accept_focus) accept_focus(GetHandle(), 0);
#endif
        ShowWithoutActivating();
#ifdef __WXOSX_COCOA__
        // wx enables hidesOnDeactivate when showing without activation. A
        // background completion must remain visible while Spark is inactive.
        const auto program = wxDynamicLibrary::GetProgramHandle();
        const auto selector = reinterpret_cast<void* (*)(const char*)>(
            wxDynamicLibrary::RawGetSymbol(program, "sel_registerName"));
        const auto message = wxDynamicLibrary::RawGetSymbol(program, "objc_msgSend");
        if (selector && message) {
            auto* window = static_cast<void*>(GetWXWindow());
            reinterpret_cast<void (*)(void*, void*, bool)>(message)(
                window, selector("setHidesOnDeactivate:"), false);
            reinterpret_cast<void (*)(void*, void*)>(message)(window, selector("orderFrontRegardless"));
        }
#endif
        timer_.StartOnce(10000);
    }
    void SetDark(bool dark) { dark_ = dark; Refresh(false); }
    void Detach() {
        manager_ = nullptr;
        timer_.Stop();
        if (HasCapture()) ReleaseMouse();
        Hide();
    }

private:
    enum class Press { None, Body, Close };
    wxRect CloseBounds() const {
        return {GetClientSize().x - FromDIP(36), FromDIP(8), FromDIP(28), FromDIP(28)};
    }
    void Paint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        const auto p = NativeColors(dark_);
        dc.SetBackground(wxBrush(p.panel)); dc.Clear();
        dc.SetPen(wxPen(p.border)); dc.SetBrush(wxBrush(p.panel));
        dc.DrawRoundedRectangle(GetClientRect().Deflate(1), FromDIP(8));
        const int left = FromDIP(16), width = std::max(1, GetClientSize().x - 2 * left);
        dc.SetTextForeground(p.text);
        dc.SetFont(GetFont().Bold());
        dc.DrawText(wxControl::Ellipsize(GetTitle(), dc, wxELLIPSIZE_END,
                    std::max(1, width - FromDIP(28)), wxELLIPSIZE_FLAGS_NONE), left, FromDIP(14));
        const auto bounds = CloseBounds();
        const wxPoint close(bounds.x + bounds.width / 2, bounds.y + bounds.height / 2);
        const int radius = FromDIP(4);
        dc.SetPen(wxPen(p.muted, std::max(1, FromDIP(1))));
        dc.DrawLine(close.x - radius, close.y - radius, close.x + radius, close.y + radius);
        dc.DrawLine(close.x + radius, close.y - radius, close.x - radius, close.y + radius);
        dc.SetFont(GetFont()); dc.SetTextForeground(p.muted);
        wxString remaining = body_;
        const int line_height = std::max(dc.GetCharHeight(), FromDIP(20));
        for (int line = 0; line < 3 && !remaining.empty(); ++line) {
            wxArrayInt extents;
            dc.GetPartialTextExtents(remaining, extents);
            size_t count = 0;
            while (count < extents.size() && extents[count] <= width) ++count;
            count = std::max(size_t{1}, count);
            if (line < 2 && count < remaining.length()) {
                const auto space = remaining.Left(count).find_last_of(" ");
                if (space != wxString::npos && space > 0) count = space;
            }
            const auto text = line == 2 ? wxControl::Ellipsize(remaining, dc, wxELLIPSIZE_END,
                width, wxELLIPSIZE_FLAGS_NONE) : remaining.Left(count);
            dc.DrawText(text, left, FromDIP(46) + line * line_height);
            remaining = remaining.Mid(count).Trim(false);
        }
    }
    NativeCompletionNotifications* manager_;
    std::string path_;
    wxString body_;
    wxTimer timer_;
    bool dark_;
    Press pressed_ = Press::None;
};

NativeCompletionNotifications::NativeCompletionNotifications(
    wxFrame* owner, std::function<void(const std::string&)> activate)
    : owner_(owner), activate_(std::move(activate)) {
    if (owner) owner->Bind(wxEVT_DESTROY, &NativeCompletionNotifications::OwnerDestroyed, this);
}
NativeCompletionNotifications::~NativeCompletionNotifications() {
    if (owner_) owner_->Unbind(wxEVT_DESTROY, &NativeCompletionNotifications::OwnerDestroyed, this);
    activate_ = {};
    DismissAll();
}
bool NativeCompletionNotifications::ShouldNotify(bool shown, bool iconized, bool active) {
    return !shown || iconized || !active;
}
bool NativeCompletionNotifications::IsUserSession(const std::string& path, const std::string& data_directory) {
    // SubAgentManager stores parent_session_key/session_path in child history,
    // not in the journal metadata. Its journals use data/subagents/<id>/sessions;
    // only journals created in the main storage root are ordinary user sessions.
    return !path.empty() && !data_directory.empty() &&
        NormalizePathForComparison(ParentPath(path)) ==
        NormalizePathForComparison(JoinPath(data_directory, "sessions"));
}
bool NativeCompletionNotifications::Show(const CompletionNotice& notice) {
    if (!owner_ || owner_->IsBeingDeleted() || notice.path.empty() || notice.body.empty() ||
        !ShouldNotify(owner_->IsShown(), owner_->IsIconized(), owner_->IsActive()) ||
        wxDisplay::GetCount() == 0) return false;
    if (cards_.size() == 3) Dismiss(cards_.front(), false);
    auto* card = new Card(this, owner_.get(), notice, dark_);
    cards_.push_back(card);
    Reposition();
    card->Open();
    wxBell();
    return true;
}
void NativeCompletionNotifications::SetDark(bool dark) {
    dark_ = dark;
    for (auto* card : cards_) card->SetDark(dark);
}
void NativeCompletionNotifications::Reposition() {
    if (!owner_ || wxDisplay::GetCount() == 0) return;
    int monitor = wxDisplay::GetFromWindow(owner_.get());
    if (monitor == wxNOT_FOUND) monitor = 0;
    const auto work = wxDisplay(static_cast<unsigned>(monitor)).GetClientArea();
    const int gap = owner_->FromDIP(12);
    int bottom = work.GetBottom() + 1 - gap;
    for (auto entry = cards_.rbegin(); entry != cards_.rend(); ++entry) {
        auto* card = *entry;
        const auto size = card->GetSize();
        card->Move(std::max(work.x, work.GetRight() + 1 - gap - size.x),
                   std::max(work.y, bottom - size.y));
        bottom -= size.y + gap;
    }
}
void NativeCompletionNotifications::Forget(Card* card) {
    cards_.erase(std::remove(cards_.begin(), cards_.end(), card), cards_.end());
    Reposition();
}
void NativeCompletionNotifications::Dismiss(Card* card, bool activate) {
    const std::string path = card->Path();
    auto callback = activate ? activate_ : std::function<void(const std::string&)>{};
    Forget(card);
    card->Detach();
    card->Destroy();
    if (owner_ && !owner_->IsBeingDeleted() && callback) callback(path);
}
void NativeCompletionNotifications::DismissAll() {
    while (!cards_.empty()) Dismiss(cards_.back(), false);
}
void NativeCompletionNotifications::OwnerDestroyed(wxWindowDestroyEvent& event) {
    if (event.GetEventObject() == owner_.get()) {
        activate_ = {};
        DismissAll();
        owner_ = nullptr;
    }
    event.Skip();
}

}  // namespace spark
