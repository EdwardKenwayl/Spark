#pragma once

#include "core/config.h"
#include "gui/provider_editor.h"

#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace spark {

class SettingsWindow {
public:
    using SavedCallback = std::function<void(const AppConfig&)>;
    using ChangedCallback = std::function<void()>;

    SettingsWindow() = default;
    ~SettingsWindow();

    void Open(HWND owner, AppConfig config, SavedCallback on_saved, ChangedCallback on_changed = {});
    void Close();
    void Resize();
    bool IsOpen() const { return window_ != nullptr; }

private:
    friend struct AppWindowSettingsTest;
    friend struct SettingsProviderTest;
    friend struct SettingsArchiveTest;

    enum class Page {
        General,
        Providers,
        Archived,
    };

    struct ArchiveEntry {
        std::string path;
        std::string title;
        std::string workspace;
        std::string scope;
        uint64_t archived_at = 0;
        size_t project_index = static_cast<size_t>(-1);
    };

    enum class ArchiveScope {
        AllProjects,
        Project,
        Recent,
    };

    struct ArchiveProject {
        std::string name;
        std::string workspace;
        std::vector<std::string> folders;
    };

    enum class ArchiveRowKind {
        Header,
        Entry,
    };

    struct ArchiveRow {
        ArchiveRowKind kind = ArchiveRowKind::Header;
        size_t entry_index = static_cast<size_t>(-1);
        size_t count = 0;
        std::wstring header;
    };

    // The dark surface needs a picker that does not fall back to the themed combo box chrome,
    // so every choice is an owner-drawn button backed by a popup list.
    struct ChoiceControl {
        HWND button = nullptr;
        std::vector<std::wstring> options;
        std::vector<std::string> values;
        int index = 0;
        operator HWND() const { return button; }
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK ArchiveListProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                            UINT_PTR subclass_id, DWORD_PTR ref_data);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    void CreateControls();
    void Layout();
    void SwitchPage(Page page);
    void CreateProviderControls();
    void RefreshProviders();
    void DrawProviderControl(const DRAWITEMSTRUCT& item);
    RECT ProviderActionRect(int row, int action) const;
    int ProviderRowAt(POINT point) const;
    int ProviderActionAt(POINT point) const;
    void HoverProvider(POINT point);
    void EditProvider(size_t index);
    Result<void> SaveProviders(AppConfig updated);
    Result<void> DuplicateProvider(size_t index);
    Result<void> DeleteProvider(size_t index);
    Result<void> MoveProvider(const std::string& id, size_t destination);
    void ActivateProviderAction(size_t index, int action);
    void CancelProviderDrag();
    void UpdateProviderDrag(POINT point);
    void StartProviderTest(size_t index);
    void PollProviderTest();
    void CancelProviderTest();
    static LRESULT CALLBACK ProviderListProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                             UINT_PTR subclass_id, DWORD_PTR ref_data);
    void RefreshGeneral();
    void RefreshModelStatus();
    void RefreshArchived();
    void RefreshArchiveLabels();
    void SaveGeneral();
    void OpenConfigFile();
    void ChooseWorkspace();
    void CleanCache();
    void DeleteAllChats();
    void RestoreSelectedArchive();
    void DeleteSelectedArchive();
    void RestoreArchiveAt(size_t index);
    void DeleteArchiveAt(size_t index);
    void ShowArchiveScopeMenu();
    void HandleArchiveListClick(POINT point);
    void HandleArchiveListMove(POINT point);
    int ArchiveRowAt(POINT point) const;
    int ArchiveActionAt(int row, int x) const;
    void SetStatus(const std::wstring& text);
    const wchar_t* Text(std::wstring_view english_msgid) const;
    void ApplyLocalization();
    void ApplyTheme();
    void RaiseSurface();
    std::string ArchiveDirectory() const;
    std::string SelectedArchivePath() const;
    int Dpi(int pixels) const;
    HWND CreateChoice(ChoiceControl& control, int id, std::initializer_list<const wchar_t*> options);
    void SetChoiceIndex(ChoiceControl& control, int index);
    std::string ChoiceValue(const ChoiceControl& control) const;
    void SelectChoiceByValue(ChoiceControl& control, const std::string& value, int fallback);
    void ShowChoiceMenu(ChoiceControl& control);
    bool IsChoiceId(int id) const;

    HWND owner_ = nullptr;
    HWND window_ = nullptr;
    HWND back_button_ = nullptr;
    HWND nav_ = nullptr;
    HWND page_title_ = nullptr;
    HWND status_ = nullptr;

    HWND workspace_path_ = nullptr;
    HWND choose_workspace_ = nullptr;
    HWND model_status_ = nullptr;
    HWND config_path_ = nullptr;
    HWND open_config_ = nullptr;
    ChoiceControl language_combo_;
    ChoiceControl theme_combo_;
    ChoiceControl shell_combo_;
    ChoiceControl send_key_combo_;
    HWND data_path_ = nullptr;
    HWND clean_cache_ = nullptr;
    HWND delete_all_button_ = nullptr;

    HWND archive_search_ = nullptr;
    HWND archive_list_ = nullptr;
    HWND archive_tooltip_ = nullptr;
    HWND restore_button_ = nullptr;
    HWND delete_archive_button_ = nullptr;
    HWND archive_status_ = nullptr;
    ChoiceControl archive_scope_combo_;

    static constexpr int kProviderList = 497;
    static constexpr int kAddProvider = 498;
    static constexpr int kProviderEmpty = 499;
    HWND provider_list_ = nullptr;
    HWND add_provider_ = nullptr;
    HWND provider_empty_ = nullptr;
    HWND provider_tooltip_ = nullptr;
    HWND provider_status_ = nullptr;
    int provider_hover_row_ = -1;
    int provider_hover_action_ = -1;
    std::string provider_pressed_id_;
    int provider_pressed_action_ = -1;
    POINT provider_pressed_point_{};
    bool provider_dragging_ = false;
    int provider_drop_ = -1;
    struct ProviderTest;
    std::shared_ptr<ProviderTest> provider_test_;
    ProviderEditor provider_editor_;

    HFONT ui_font_ = nullptr;
    HFONT sidebar_font_ = nullptr;
    HFONT title_font_ = nullptr;
    HBRUSH background_brush_ = nullptr;
    HBRUSH sidebar_brush_ = nullptr;
    HBRUSH panel_brush_ = nullptr;
    HBRUSH input_brush_ = nullptr;
    Page page_ = Page::General;
    AppConfig config_;
    SavedCallback on_saved_;
    ChangedCallback on_changed_;
    std::vector<ArchiveEntry> archive_entries_;
    std::vector<ArchiveProject> archive_projects_;
    std::vector<ArchiveRow> archive_rows_;
    ArchiveScope archive_scope_ = ArchiveScope::AllProjects;
    size_t archive_scope_project_ = static_cast<size_t>(-1);
    int archive_hover_row_ = -1;
    int archive_hover_action_ = 0;
    int archive_pressed_row_ = -1;
    int archive_pressed_action_ = 0;
    std::wstring archive_tooltip_text_;
    int archive_restore_width_ = 0;
    int preferences_extra_height_ = 0;
    RECT general_cards_[3]{};
};

}  // namespace spark
