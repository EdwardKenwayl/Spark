#include "gui/native/text_prompt.h"
#include "gui/native/controls.h"
#include "gui/localization.h"

#include <wx/settings.h>
#include <wx/textctrl.h>
#include <wx/utils.h>
#include <wx/weakref.h>

namespace spark {

NativeTextPrompt::NativeTextPrompt(wxWindow* owner, const wxString& title, const wxString& initial)
    : wxDialog(owner, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE) {
    SetName("spark.text-prompt");
    auto font = GetFont();
    font.SetPointSize(10);
    SetFont(font);
    SetAffirmativeId(wxID_OK);
    SetEscapeId(wxID_CANCEL);
    SetSize(FromDIP(wxSize(520, 190)));

    edit_ = new wxTextCtrl(this, wxID_ANY, initial, FromDIP(wxPoint(18, 22)),
                           FromDIP(wxSize(484, 30)), wxTE_PROCESS_ENTER | wxBORDER_SIMPLE);
    edit_->SetName("spark.text-prompt.value");
    const auto button = [&](const wchar_t* label, const char* name, int x, int id) {
        auto* control = new NativeButton(this, wxString(i18n::Text(label)));
        control->SetId(id);
        control->SetName(name);
        control->SetLabelAlignment(wxALIGN_CENTER);
        control->SetMinSize(FromDIP(wxSize(80, 32)));
        control->SetSize(wxRect(FromDIP(wxPoint(x, 76)), FromDIP(wxSize(80, 32))));
    };
    button(L"OK", "spark.text-prompt.ok", 330, wxID_OK);
    button(L"Cancel", "spark.text-prompt.cancel", 420, wxID_CANCEL);
    const bool dark = owner && owner->GetForegroundColour().IsOk()
        ? owner->GetForegroundColour().Red() > 180 : wxSystemSettings::GetAppearance().IsDark();
    ApplyNativeColors(this, dark);

    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); }, wxID_OK);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); }, wxID_CANCEL);
    edit_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { EndModal(wxID_OK); });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { EndModal(wxID_CANCEL); });
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_ESCAPE) EndModal(wxID_CANCEL);
        else event.Skip();
    });
}

int NativeTextPrompt::ShowModal() {
    wxWeakRef<wxWindow> focus(wxWindow::FindFocus()), owner(GetParent());
    if (owner) CentreOnParent();
    else CentreOnScreen();
    edit_->SetFocus();
    const int result = wxDialog::ShowModal();
    if (owner && !owner->IsBeingDeleted() && focus && !focus->IsBeingDeleted() &&
        focus->IsShownOnScreen() && focus->IsEnabled() &&
        wxGetActiveWindow() == wxGetTopLevelParent(owner.get()))
        focus->SetFocus();
    return result;
}

wxString NativeTextPrompt::GetValue() const { return edit_->GetValue(); }

}  // namespace spark
