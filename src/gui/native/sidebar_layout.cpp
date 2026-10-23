#include "gui/native/app.h"
#include "gui/localization.h"

#include <algorithm>

namespace spark {

void NativeFrame::CreateSidebar() {
    sidebar_brand_ = new wxStaticText(sidebar_, wxID_ANY, "Spark");
    sidebar_brand_->SetFont(wxFont(wxFontInfo(13).Bold()));
    const auto button = [&](const wchar_t* label, const char* name, const wxArtID& art = wxEmptyString) {
        auto* control = new NativeButton(sidebar_, wxString(i18n::Text(label)), art);
        control->SetName(name);
        labels_.push_back({control, label});
        return control;
    };
    const auto icon = [&](const wchar_t* label, const char* name, const wxArtID& art) {
        auto* control = new NativeButton(sidebar_, {}, art);
        control->SetName(name);
        control->SetToolTip(wxString(i18n::Text(label)));
        tooltips_.push_back({control, label});
        return control;
    };
    new_chat_ = button(L"New chat", "spark.new-chat", wxART_EDIT);
    new_chat_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { NewConversation(); });
    project_header_ = button(L"Projects", "spark.sidebar.section.projects");
    recent_header_ = button(L"Recent", "spark.sidebar.section.recent");
    project_menu_ = icon(L"Projects", "spark.sidebar.projects.menu", wxART_LIST_VIEW);
    project_add_ = icon(L"Add project", "spark.sidebar.projects.add", wxART_PLUS);
    recent_menu_ = icon(L"Recent", "spark.sidebar.recent.menu", wxART_LIST_VIEW);
    recent_new_ = icon(L"New chat", "spark.sidebar.recent.new", wxART_EDIT);
    project_add_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AddProject(); });
    recent_new_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { NewConversation(); });
    recent_menu_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RecentMenu(); });
    project_menu_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SectionMenu(project_menu_, true); });
    project_header_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (projects_.empty()) return;
        project_section_expanded_ = !project_section_expanded_;
        LayoutSidebar();
    });
    recent_header_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (rows_.empty()) return;
        recent_section_expanded_ = !recent_section_expanded_;
        LayoutSidebar();
    });
    project_list_ = new NativeList(sidebar_);
    project_list_->SetName("spark.projects.list");
    history_ = new NativeList(sidebar_);
    history_->SetName("spark.history");
    sidebar_rail_ = new NativeSidebarRail(sidebar_, project_list_, history_);
    for (auto* list : {project_list_, history_}) {
        list->Bind(wxEVT_LISTBOX, [this, list](wxCommandEvent& event) {
            if (!refreshing_) SidebarActivate(list, event.GetSelection(), 0);
        });
        list->Bind(wxEVT_LEFT_DOWN, [this, list](wxMouseEvent& event) {
            const int index = list->HitTest(event.GetPosition());
            const auto& rows = list == project_list_ ? project_rows_ : rows_;
            if (index < 0 || static_cast<size_t>(index) >= rows.size()) return;
            const auto& row = rows[index];
            wxRect rect;
            rect = list->GetItemRect(index);
            const bool project = row.path.empty() && !row.more;
            const int x = event.GetX(), right = rect.x + rect.width;
            int action = 0;
            if (project && x >= right - FromDIP(64))
                action = x >= right - FromDIP(34) ? 1 : 2;
            else if (!row.path.empty() && x >= right - FromDIP(96))
                action = x >= right - FromDIP(32) ? 1 : x >= right - FromDIP(64) ? 2 : 3;
            list->SetFocus();
            SidebarActivate(list, index, action);
        });
        list->Bind(wxEVT_CONTEXT_MENU, &NativeFrame::SidebarMenu, this);
        list->Bind(wxEVT_MOTION, [this, list](wxMouseEvent& event) {
            const int index = list->HitTest(event.GetPosition());
            const auto& rows = list == project_list_ ? project_rows_ : rows_;
            wxString tooltip;
            if (index >= 0 && static_cast<size_t>(index) < rows.size()) {
                const auto& row = rows[index];
                wxRect rect;
                rect = list->GetItemRect(index);
                const int right = rect.x + rect.width, x = event.GetX();
                if (row.path.empty() && !row.more && x >= right - FromDIP(64))
                    tooltip = wxString(i18n::Text(x >= right - FromDIP(34) ? L"New chat" : L"Projects"));
                else if (!row.path.empty() && x >= right - FromDIP(96)) {
                    const auto item = std::find_if(history_items_.begin(), history_items_.end(),
                        [&](const auto& history) { return history.path == row.path; });
                    tooltip = wxString(i18n::Text(x >= right - FromDIP(32) ? L"Archive chat" :
                        x < right - FromDIP(64) ? L"Delete chat" :
                        item != history_items_.end() && item->pinned ? L"Unpin" : L"Pin"));
                } else tooltip = list->GetString(index);
            }
            if (tooltip != list->GetToolTipText()) list->SetToolTip(tooltip);
            event.Skip();
        });
    }
    settings_button_ = button(L"Settings", "spark.sidebar.settings", wxART_EXECUTABLE_FILE);
    settings_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OpenSettings(); });
    sidebar_->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { LayoutSidebar(); event.Skip(); });
}

void NativeFrame::LayoutSidebar() {
    const int width = sidebar_->GetClientSize().x, height = sidebar_->GetClientSize().y;
    if (width <= 0 || height <= 0) return;
    const auto place = [&](wxWindow* child, int x, int y, int w, int h) {
        const wxRect bounds(x, y, std::max(0, w), std::max(0, h));
        if (child->GetRect() != bounds) child->SetSize(bounds);
    };
    place(sidebar_brand_, FromDIP(16), FromDIP(12), width - FromDIP(32), FromDIP(28));
    place(new_chat_, FromDIP(10), FromDIP(46), width - FromDIP(20), FromDIP(34));
    place(project_header_, FromDIP(16), FromDIP(90), width - FromDIP(94), FromDIP(22));
    place(project_menu_, width - FromDIP(70), FromDIP(87), FromDIP(24), FromDIP(24));
    place(project_add_, width - FromDIP(42), FromDIP(87), FromDIP(24), FromDIP(24));
    const int settings_top = height - FromDIP(42), projects_top = FromDIP(116);
    const int bottom = std::max(projects_top + FromDIP(88), settings_top - FromDIP(8));
    int project_height = 0;
    if (project_section_expanded_ && !project_rows_.empty()) {
        const int available = std::max(FromDIP(72),
            bottom - projects_top - FromDIP(54) - (recent_section_expanded_ ? FromDIP(70) : 0));
        const int wanted = std::max(FromDIP(34), static_cast<int>(project_rows_.size()) * FromDIP(34));
        project_height = std::min(wanted, std::max(FromDIP(68), available * 52 / 100));
    }
    const int recent_y = projects_top + project_height + FromDIP(12);
    const int history_y = recent_y + FromDIP(26);
    const int history_height = recent_section_expanded_ ? std::max(FromDIP(48), bottom - history_y) : 0;
    place(project_list_, FromDIP(6), projects_top, width - FromDIP(24), project_height);
    project_list_->Show(project_height > 0);
    place(recent_header_, FromDIP(16), recent_y, width - FromDIP(94), FromDIP(24));
    place(recent_menu_, width - FromDIP(70), recent_y - FromDIP(3), FromDIP(24), FromDIP(24));
    place(recent_new_, width - FromDIP(42), recent_y - FromDIP(3), FromDIP(24), FromDIP(24));
    place(history_, FromDIP(6), history_y, width - FromDIP(24), history_height);
    history_->Show(history_height > 0);
    int rail_top = projects_top, rail_bottom = history_y + history_height;
    if (!project_section_expanded_ || project_height <= 0) rail_top = history_y;
    if (!recent_section_expanded_ || history_height <= 0) rail_bottom = projects_top + project_height;
    if (rail_bottom <= rail_top) rail_bottom = std::min(bottom, rail_top + FromDIP(48));
    place(sidebar_rail_, width - FromDIP(18), rail_top, FromDIP(10),
          std::max(FromDIP(24), rail_bottom - rail_top));
    sidebar_rail_->Sync();
    place(settings_button_, FromDIP(10), settings_top, width - FromDIP(20), FromDIP(32));
    project_header_->SetDisclosure(projects_.empty() ? NativeDisclosure::None :
        project_section_expanded_ ? NativeDisclosure::Expanded : NativeDisclosure::Collapsed);
    recent_header_->SetDisclosure(rows_.empty() ? NativeDisclosure::None :
        recent_section_expanded_ ? NativeDisclosure::Expanded : NativeDisclosure::Collapsed);
}

}  // namespace spark
