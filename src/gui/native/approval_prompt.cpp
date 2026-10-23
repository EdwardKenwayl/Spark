#include "gui/native/approval_prompt.h"
#include "gui/native/controls.h"
#include "gui/localization.h"

#include <wx/button.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/utils.h>
#include <wx/weakref.h>
#include <algorithm>

namespace spark {

NativeApprovalPrompt::NativeApprovalPrompt(wxWindow* owner, const ApprovalRequest& request)
    : wxDialog(owner, wxID_ANY, wxString(i18n::Text(L"Approval required")),
               wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE) {
    SetName("spark.approval");
    SetFont(wxFont(wxFontInfo(10)));
    SetEscapeId(wxID_NO);
    auto details = Utf8ToWide(TruncateUtf8(Trim(request.detail), 16000));
    if (details.size() > 4000) details.resize(4000);
    const auto message = wxString(Utf8ToWide(request.title)) + "\n\n" +
        wxString(details) + wxString(i18n::Text(
            L"\n\nThis is application-level approval, not an operating-system sandbox.\n\nChoose Yes to allow once, No to deny, or Cancel to allow for this turn."));
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* body = new wxBoxSizer(wxHORIZONTAL);
    body->Add(new wxStaticBitmap(this, wxID_ANY,
        wxArtProvider::GetBitmap(wxART_WARNING, wxART_MESSAGE_BOX, FromDIP(wxSize(32, 32)))),
        0, wxRIGHT, FromDIP(14));
    auto* scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    scroll->SetScrollRate(0, FromDIP(12));
    auto* text = new wxStaticText(scroll, wxID_ANY, message);
    text->SetName("spark.approval.message");
    text->Wrap(FromDIP(500));
    auto* content = new wxBoxSizer(wxVERTICAL);
    content->Add(text, 0, wxEXPAND);
    scroll->SetSizer(content);
    scroll->SetMinSize(wxSize(FromDIP(520), std::clamp(text->GetBestSize().y, FromDIP(80), FromDIP(320))));
    body->Add(scroll, 1, wxEXPAND);
    root->Add(body, 1, wxEXPAND | wxALL, FromDIP(20));
    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    actions->AddStretchSpacer();
    const auto button = [&](int id, const wxString& label, NativeApprovalDecision decision) {
        auto* action = new wxButton(this, id, label);
        action->SetMinSize(FromDIP(wxSize(80, 32)));
        actions->Add(action, 0, wxLEFT, FromDIP(8));
        Bind(wxEVT_BUTTON, [this, id, decision](wxCommandEvent&) {
            decision_ = decision;
            EndModal(id);
        }, id);
        return action;
    };
    button(wxID_YES, {}, NativeApprovalDecision::Once)->SetDefault();
    button(wxID_NO, {}, NativeApprovalDecision::Deny);
    button(wxID_CANCEL, wxString(i18n::Text(L"Cancel")), NativeApprovalDecision::Turn);
    root->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));
    SetSizerAndFit(root);
    const bool dark = owner && owner->GetForegroundColour().IsOk()
        ? owner->GetForegroundColour().Red() > 180 : wxSystemSettings::GetAppearance().IsDark();
    ApplyNativeColors(this, dark);
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
        decision_ = NativeApprovalDecision::Deny;
        EndModal(wxID_CANCEL);
    });
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_ESCAPE) {
            decision_ = NativeApprovalDecision::Deny;
            EndModal(wxID_CANCEL);
        } else event.Skip();
    });
}

int NativeApprovalPrompt::ShowModal() {
    decision_ = NativeApprovalDecision::Deny;
    wxWeakRef<wxWindow> focus(wxWindow::FindFocus()), owner(GetParent());
    if (owner) CentreOnParent();
    else CentreOnScreen();
    const int result = wxDialog::ShowModal();
    if (owner && !owner->IsBeingDeleted() && focus && !focus->IsBeingDeleted() &&
        focus->IsShownOnScreen() && focus->IsEnabled() &&
        wxGetActiveWindow() == wxGetTopLevelParent(owner.get()))
        focus->SetFocus();
    return result;
}

}  // namespace spark
