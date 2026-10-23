#pragma once

#include <wx/window.h>

#include <functional>
#include <string>
#include <vector>

namespace spark {

struct NativeReasoningPopupState {
    bool reasoning = true;
    bool built_in_defaults = false;
    std::vector<std::string> efforts;
    std::vector<std::string> selected;
    std::string default_effort;
};

using NativeReasoningPopupCallback =
    std::function<void(const NativeReasoningPopupState&)>;

void ShowNativeReasoningPopup(wxWindow* owner, const wxPoint& screen_anchor,
                              NativeReasoningPopupState state,
                              NativeReasoningPopupCallback on_change);

}  // namespace spark
