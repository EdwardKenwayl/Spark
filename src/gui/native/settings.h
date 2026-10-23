#pragma once

#include "core/config.h"
#include "gui/native/controls.h"
#include "gui/native/archive_list.h"
#include "gui/native/provider_list.h"

#include <wx/panel.h>
#include <wx/timer.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

class wxButton;
class wxChoice;
class wxSimplebook;
class wxScrolledWindow;
class wxSearchCtrl;
class wxStaticText;
class wxTextCtrl;

namespace spark {

class NativeProviderEditor;

class NativeSettings : public wxPanel {
public:
    // Archive actions also notify saved so the owner can refresh its session list.
    NativeSettings(wxWindow* parent, const AppConfig& config,
                   std::function<void(const AppConfig&)> saved,
                   std::function<void()> back);
    ~NativeSettings() override;
    void Reload(const AppConfig& config);
    bool Layout() override;

private:
    struct Probe;
    struct ArchiveJob;
    struct ArchiveEntry {
        std::string path, title, workspace, scope;
        uint64_t modified = 0;
        size_t project = static_cast<size_t>(-1);
    };
    struct ArchiveProject { std::string key, name; std::vector<std::string> folders; };
    void CreateGeneral(wxWindow* page);
    void CreateProviders(wxWindow* page);
    void CreateArchives(wxWindow* page);
    void LayoutSettings();
    void ApplyTheme();
    void Localize();
    void SaveGeneral();
    void CleanCache();
    bool Commit(AppConfig updated, bool providers);
    void RefreshProviders();
    void EditProvider(bool add, size_t index = static_cast<size_t>(-1));
    void ShowProviderList();
    void ProviderAction(size_t index, ProviderListAction action);
    void ReorderProvider(size_t source, size_t destination);
    void StartProbe(size_t index);
    void StartArchiveScan();
    void RefreshArchiveScopes();
    void FilterArchives();
    void ArchiveAction(size_t index, bool restore);
    void DeleteAllArchives();
    void Poll(wxTimerEvent&);
    void Status(const std::string& text);

    AppConfig config_;
    std::function<void(const AppConfig&)> saved_;
    std::function<void()> back_;
    bool syncing_ = false, archives_dirty_ = true, laying_out_ = false;
    wxTimer timer_;
    std::shared_ptr<Probe> probe_;
    std::shared_ptr<ArchiveJob> archive_job_;
    std::thread probe_worker_, archive_worker_;
    std::vector<ArchiveEntry> archives_;
    std::vector<ArchiveProject> archive_projects_;
    std::vector<std::string> archive_scope_keys_;
    std::string archive_scope_key_ = "all";
    std::string archive_scan_status_;
    wxSimplebook* pages_ = nullptr;
    wxSimplebook* provider_pages_ = nullptr;
    wxSimplebook* archive_pages_ = nullptr;
    wxPanel* provider_list_page_ = nullptr;
    NativeProviderEditor* provider_editor_ = nullptr;
    int provider_editor_page_ = wxNOT_FOUND;
    wxPanel* navigation_ = nullptr;
    wxScrolledWindow* general_page_ = nullptr;
    wxPanel* general_cards_[3]{};
    wxStaticText* general_labels_[3]{};
    wxStaticText *model_label_ = nullptr, *config_label_ = nullptr;
    NativeButton *back_button_ = nullptr, *browse_ = nullptr;
    NativeButton* nav_[3]{};
    wxStaticText* section_title_ = nullptr;
    wxStaticText *status_ = nullptr, *config_status_ = nullptr, *archive_status_ = nullptr, *data_path_ = nullptr;
    wxTextCtrl* output_ = nullptr;
    wxStaticText* config_path_ = nullptr;
    NativeChoice *language_ = nullptr, *theme_ = nullptr, *shell_ = nullptr,
                 *send_key_ = nullptr, *archive_scope_ = nullptr;
    std::vector<std::string> languages_;
    NativeProviderList* provider_list_ = nullptr;
    wxStaticText* provider_empty_ = nullptr;
    NativeArchiveList* archive_list_ = nullptr;
    wxSearchCtrl* archive_search_ = nullptr;
    NativeButton* add_ = nullptr;
    NativeButton* open_config_ = nullptr;
    NativeButton* clear_cache_ = nullptr;
    NativeButton* delete_all_archives_ = nullptr;
};

}  // namespace spark
