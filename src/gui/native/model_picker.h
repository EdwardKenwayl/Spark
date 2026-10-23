#pragma once

#include <wx/gdicmn.h>
#include <wx/string.h>
#include <vector>

class wxWindow;

namespace spark {

enum class NativeModelPickerKind { Header, Model, Reasoning };
struct NativeModelPickerRow {
    int command;
    wxString label;
    NativeModelPickerKind kind;
};

// GUI-thread only. The bottom-left anchor is in screen coordinates.
int ShowNativeModelPicker(wxWindow* owner, const wxPoint& anchor,
                          std::vector<NativeModelPickerRow> rows, int checked);

}  // namespace spark
