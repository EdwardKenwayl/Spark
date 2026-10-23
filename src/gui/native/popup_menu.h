#pragma once

#include <wx/gdicmn.h>
#include <wx/defs.h>

class wxMenu;
class wxWindow;

namespace spark {

enum class NativeMenuAlignment { Top, Bottom };

// GUI-thread only. The anchor is in screen coordinates (default: mouse position).
// Returns a leaf ID or wxID_NONE; does not dispatch events or mutate the menu.
int ShowNativeMenu(wxWindow* owner, wxMenu& menu,
                   const wxPoint& screen_anchor = wxDefaultPosition,
                   NativeMenuAlignment alignment = NativeMenuAlignment::Top);

}  // namespace spark
