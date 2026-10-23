#pragma once

#include <wx/dialog.h>

class wxTextCtrl;

namespace spark {

class NativeTextPrompt final : public wxDialog {
public:
    NativeTextPrompt(wxWindow* owner, const wxString& title, const wxString& initial);
    int ShowModal() override;
    wxString GetValue() const;

private:
    wxTextCtrl* edit_ = nullptr;
};

}  // namespace spark
