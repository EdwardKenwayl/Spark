#pragma once

#include "gui/native/chat_surface.h"
#include "gui/native/runtime.h"
#include "gui/native/settings.h"
#include "gui/native/controls.h"
#include "gui/native/attachment_strip.h"
#include "gui/native/summary_panel.h"
#include "gui/native/sidebar_rail.h"
#include "gui/native/thumbnail_cache.h"
#include "gui/session_loader.h"
#include "gui/native/history_loader.h"

#include <wx/choice.h>
#include <wx/button.h>
#include <wx/frame.h>
#include <wx/listbox.h>
#include <wx/splitter.h>
#include <wx/stattext.h>
#include <wx/taskbar.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/sizer.h>
#include <set>

namespace spark {

class NativeFrame : public wxFrame {
public:
    explicit NativeFrame(AppConfig config);
    ~NativeFrame() override;
    void OpenSettings();
    void RestoreWindow();

private:
    struct View {
        SessionJournal journal;
        Transcript transcript;
        size_t assistant = kNoChatItem, reasoning = kNoChatItem, status = kNoChatItem;
        std::map<std::string, size_t> tools;
        uint64_t context = 0, limit = 0, replay = 0;
    };
    struct Project {
        std::string name, workspace;
        std::vector<std::string> folders;
        bool expanded = false, show_all = false;
        bool pinned = false;
        uint64_t last_opened = 0;
    };
    struct SidebarRow { std::string path; int project = -1; bool project_chat = false, more = false; };
    class Tray;
    void CreateChat();
    void CreateSidebar();
    void LayoutSidebar();
    void LayoutShell();
    void LayoutHeader();
    void LayoutComposer();
    void ModelMenu();
    void ReasoningMenu();
    void SidebarActivate(NativeList* list, int index, int action);
    void ChatAction(const SidebarRow& row, int command);
    void ApplyConfig(const AppConfig& config);
    void RefreshSidebar();
    void RenderSidebar();
    void RefreshModels();
    void UpdateControls();
    void OpenConversation(const std::string& path);
    void NewConversation(int project = -1);
    void AddProject();
    void ProjectMenu(int project, const wxPoint& anchor);
    void SidebarMenu(wxContextMenuEvent& event);
    void RecentMenu();
    void SectionMenu(wxWindow* anchor, bool projects);
    void ImportConversation(int project = -1);
    void SaveProjects();
    void LoadProjects();
    bool Pause();
    void Send();
    void AddAttachments();
    void AttachPath(const std::string& path);
    bool PasteAttachments();
    void RefreshAttachments();
    void PreviewAttachment(const std::string& path);
    void Summary();
    void RefreshSummary(bool load_history = false);
    void SelectChild(const std::string& id);
    void ResetSideConversation();
    void OpenChild(const SubAgentInfo& child);
    void SaveDraft();
    void RestoreDraft();
    std::string DraftPath() const;
    void EditMessage(size_t row);
    void Tick(wxTimerEvent&);
    void Consume(const NativeEvent& event);
    void PollExternal();
    void OnClose(wxCloseEvent&);
    void Error(const std::string& message);
    View* Active();
    const ModelConfig* SelectedModel() const;

    AppConfig config_;
    NativeRuntime runtime_;
    SessionLoader loader_;
    SessionLoader side_loader_;
    NativeHistoryLoader history_loader_;
    NativeThumbnailCache summary_thumbnails_;
    wxTimer timer_;
    std::unique_ptr<Tray> tray_;
    wxPanel *shell_ = nullptr, *sidebar_ = nullptr, *chat_page_ = nullptr;
    NativeAttachmentStrip* attachment_panel_ = nullptr;
    NativeSummaryPanel* side_panel_ = nullptr;
    wxPanel *composer_host_ = nullptr, *header_panel_ = nullptr;
    NativeComposer* input_panel_ = nullptr;
    wxBoxSizer *root_ = nullptr, *chat_sizer_ = nullptr;
    NativeSettings* settings_ = nullptr;
    NativeChatSurface *chat_ = nullptr, *side_chat_ = nullptr;
    wxStaticText *title_ = nullptr, *context_ = nullptr;
    wxStaticText* composer_hint_ = nullptr;
    NativeList* history_ = nullptr;
    NativeList* project_list_ = nullptr;
    NativeSidebarRail* sidebar_rail_ = nullptr;
    wxStaticText* sidebar_brand_ = nullptr;
    NativeButton *new_chat_ = nullptr, *settings_button_ = nullptr;
    NativeButton *project_header_ = nullptr, *recent_header_ = nullptr;
    NativeButton *project_menu_ = nullptr, *project_add_ = nullptr, *recent_menu_ = nullptr, *recent_new_ = nullptr;
    wxTextCtrl* composer_ = nullptr;
    NativeChoice* permissions_ = nullptr;
    NativeButton *model_ = nullptr, *attach_ = nullptr, *send_ = nullptr, *summary_ = nullptr;
    std::vector<std::pair<wxWindow*, std::wstring>> labels_, tooltips_;
    std::vector<SidebarRow> rows_;
    std::vector<SidebarRow> project_rows_;
    std::vector<TranscriptHistory> history_items_;
    std::vector<Project> projects_;
    std::vector<SubAgentInfo> summary_children_;
    std::vector<SourceReference> summary_sources_;
    std::map<std::string, std::unique_ptr<View>> views_;
    std::unique_ptr<View> pending_view_;
    Transcript empty_;
    std::unique_ptr<Transcript> side_transcript_ = std::make_unique<Transcript>(), pending_side_;
    std::string active_, project_workspace_, project_name_, goal_, side_path_;
    std::vector<std::string> attachments_;
    std::map<std::string, nlohmann::json> external_;
    std::set<std::string> external_approvals_;
    std::map<std::string, std::pair<uintmax_t, spark::fs::file_time_type>> stamps_;
    std::map<std::string, std::pair<spark::fs::file_time_type, TranscriptHistory>> summaries_;
    uint64_t load_generation_ = 0, loading_since_ = 0, last_poll_ = 0, last_draft_ = 0, side_poll_ = 0;
    uint64_t side_generation_ = 0, side_replay_ = 0;
    std::string loading_path_;
    std::string summary_parent_;
    uint64_t snapshot_target_id_ = 0;
    size_t edit_row_ = kNoChatItem;
    bool plan_ = false, quitting_ = false, refreshing_ = false, draft_dirty_ = false;
    bool side_loading_ = false;
    bool close_pending_ = false;
    bool project_section_expanded_ = true, recent_section_expanded_ = true;
    bool recent_show_all_ = false;
    std::string organize_mode_ = "project", chat_sort_ = "recent";
};

}  // namespace spark
