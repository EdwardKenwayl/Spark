#pragma once

#include "gui/completion_notice.h"

#include <wx/frame.h>
#include <wx/weakref.h>
#include <functional>
#include <vector>

namespace spark {

// UI-thread owner of independent, non-activating completion cards.
class NativeCompletionNotifications final : public wxEvtHandler {
public:
    NativeCompletionNotifications(wxFrame* owner, std::function<void(const std::string&)> activate);
    ~NativeCompletionNotifications() override;
    NativeCompletionNotifications(const NativeCompletionNotifications&) = delete;
    NativeCompletionNotifications& operator=(const NativeCompletionNotifications&) = delete;
    static bool ShouldNotify(bool shown, bool iconized, bool active);
    static bool IsUserSession(const std::string& path, const std::string& data_directory);
    bool Show(const CompletionNotice& notice);
    void SetDark(bool dark);
    void DismissAll();

private:
    class Card;
    void Dismiss(Card* card, bool activate);
    void Forget(Card* card);
    void Reposition();
    void OwnerDestroyed(wxWindowDestroyEvent& event);
    wxWeakRef<wxFrame> owner_;
    std::function<void(const std::string&)> activate_;
    std::vector<Card*> cards_;
    bool dark_ = false;
};

}  // namespace spark
