#pragma once

#include "core/agent_types.h"
#include <wx/gdicmn.h>
#include <functional>
#include <string>
#include <vector>

class wxWindow;

namespace spark {

struct SubAgentInfo;
enum class NativeSummaryPopupAction { Sources, Agent };
using NativeSummaryPopupCallback =
    std::function<void(NativeSummaryPopupAction, const std::string&)>;

// UI thread only; screen_anchor is in physical screen pixels. Copies bounded
// metadata before returning, never retains agents/sources or their references.
// Same-owner repeated call toggles off. Dismissal has no selection callback.
// An outside press reposted to the invoker is part of that same toggle gesture.
// Selection dismisses first; callback is suppressed if owner has been destroyed.
// Copy the borrowed ID in the callback before retaining it for asynchronous work.
void ShowNativeSummaryPopup(wxWindow* owner, const wxPoint& screen_anchor,
                            const std::vector<SubAgentInfo>& agents,
                            const std::vector<SourceReference>& sources,
                            NativeSummaryPopupCallback on_select);

}  // namespace spark
