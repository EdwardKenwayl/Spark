#pragma once

#include <wx/gdicmn.h>
#include <wx/string.h>

class wxWindow;

namespace spark {

enum class NativeAddAction { None = -1, Files, Snapshot, Goal, Plan };

// GUI-thread only. Composer bounds are in screen coordinates.
NativeAddAction ShowNativeAddMenu(wxWindow* owner, const wxRect& composer,
                                  const wxString& snapshot_title, const wxString& goal, bool plan);

}  // namespace spark
