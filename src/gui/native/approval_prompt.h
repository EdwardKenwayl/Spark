#pragma once

#include "gui/native/runtime.h"

#include <wx/dialog.h>

namespace spark {

class NativeApprovalPrompt final : public wxDialog {
public:
    NativeApprovalPrompt(wxWindow* owner, const ApprovalRequest& request);
    int ShowModal() override;
    NativeApprovalDecision Decision() const { return decision_; }

private:
    NativeApprovalDecision decision_ = NativeApprovalDecision::Deny;
};

}  // namespace spark
