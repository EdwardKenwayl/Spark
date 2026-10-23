#include "gui/native/app.h"

#include <wx/dcclient.h>
#include <algorithm>

namespace spark {

void NativeFrame::LayoutShell() {
    if (!shell_ || !shell_->GetSizer()) return;
    const int width = shell_->GetClientSize().x;
    const int sidebar = std::min(FromDIP(300), std::max(FromDIP(190),
        std::min(FromDIP(236), width * 24 / 100)));
    const int side = side_panel_->IsShown() ? std::min(
        std::min(FromDIP(600), std::max(FromDIP(340), width * 40 / 100)),
        std::max(0, width - sidebar - FromDIP(1) - FromDIP(320))) : 0;
    if (sidebar_->GetMinSize().x != sidebar) sidebar_->SetMinSize(wxSize(sidebar, -1));
    if (side_panel_->GetMinSize().x != side) side_panel_->SetMinSize(wxSize(side, -1));
    shell_->Layout();
    LayoutSidebar();
    LayoutHeader();
    LayoutComposer();
}

void NativeFrame::LayoutHeader() {
    if (!header_panel_ || !summary_ || !context_) return;
    const int width = header_panel_->GetClientSize().x, left = FromDIP(20);
    int cursor = std::max(left, width - FromDIP(12));
    const auto place = [](wxWindow* window, const wxRect& bounds) {
        if (window->GetRect() != bounds) window->SetSize(bounds);
    };
    place(summary_, wxRect(std::max(left, cursor - FromDIP(32)), FromDIP(7), FromDIP(32), FromDIP(32)));
    cursor = std::max(left, cursor - FromDIP(44));
    wxClientDC dc(context_);
    dc.SetFont(context_->GetFont());
    const int wanted = std::max(FromDIP(195), dc.GetTextExtent(context_->GetLabel()).x + FromDIP(16));
    const int available = std::min(wanted, std::max(0, cursor - left));
    place(context_, wxRect(cursor - available, FromDIP(13), available, FromDIP(20)));
    cursor = std::max(left, cursor - available - FromDIP(12));
    place(title_, wxRect(left, FromDIP(11), std::max(0, cursor - left), FromDIP(24)));
}

}  // namespace spark
