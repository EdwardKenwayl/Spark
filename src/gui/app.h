#pragma once

#include "core/agent.h"
#include "core/config.h"
#include "gui/chat_view.h"
#include "gui/settings_window.h"
#include "gui/session_loader.h"
#include "gui/summary_view.h"
#include "gui/add_menu.h"
#include "platform/smart_snapshot.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace spark {

inline constexpr wchar_t kAppWindowClass[] = L"SparkMainWindow";
inline constexpr wchar_t kAppMutexName[] = L"Local\\Spark.SingleInstance";
inline constexpr UINT kSingleInstanceMessage = WM_APP + 6;
inline constexpr UINT_PTR kSessionLoadTimer = 6;
inline constexpr UINT_PTR kTurnWatchdogTimer = 2;

// Set once in AppWindow::Run when the previous process ended without a clean
// shutdown. Recovery mode deliberately avoids replaying the last transcript.
inline bool g_startup_recovery_mode = false;

class AppWindow {
public:
    AppWindow() = default;
    ~AppWindow();

    int Run(HINSTANCE instance, int show_command);

private:
    friend struct AppWindowExitTest;
    friend struct AppWindowComposerTest;
    friend struct AppWindowSettingsTest;
    friend struct AppWindowSessionLoadTest;

    enum class EventKind {
        Status,
        Phase,
        UserMessageAppended,
        TextDelta,
        ReasoningDelta,
        ToolStart,
        ToolFinish,
        TurnFinished,
        Context,
        Compaction,
        Error,
        SubAgentCreated,
        SubAgentPhase,
        SubAgentText,
        SubAgentFinished,
        SubAgentError,
        SubAgentReasoning,
        SubAgentToolStart,
        SubAgentToolFinish,
    };

    struct UiEvent {
        UiEvent() = default;
        UiEvent(EventKind kind, std::string first = {}, std::string second = {}, bool success = false)
            : kind(kind), first(std::move(first)), second(std::move(second)), success(success) {}

        EventKind kind = EventKind::Status;
        std::string session_key;
        std::string subagent_id;
        std::string first;
        std::string second;
        std::string third;
        AgentPhase phase = AgentPhase::WaitingModel;
        bool success = false;
        SubAgentInfo child_info;
    };

    // One running turn per conversation. Each runtime owns its own journal and
    // cancellation token so two conversations never share mutable session state.
    struct TurnRuntime {
        std::string path;
        std::string session_key;
        std::string workspace;
        AppConfig config;
        SessionJournal journal;
        CancellationToken cancellation;
        std::thread worker;
        std::atomic<bool> complete{false};
        std::atomic<bool> success{false};
        std::atomic<bool> cancelled{false};
        std::atomic<bool> interrupt_requested{false};
        std::atomic<uint64_t> last_progress_tick{0};
        std::atomic<uint64_t> usage_input_tokens{0};
        std::atomic<uint64_t> usage_output_tokens{0};
        std::atomic<uint64_t> usage_cached_input_tokens{0};
        std::atomic<uint64_t> usage_model_calls{0};
        std::atomic<uint64_t> usage_last_input_tokens{0};
        std::atomic<uint64_t> usage_last_output_tokens{0};
        std::atomic<uint64_t> usage_context_tokens{0};
        std::atomic<uint64_t> usage_context_limit_tokens{0};
        std::atomic<bool> usage_context_published{false};
        uint64_t started_tick = 0;
        bool resume_turn = false;
        bool timed_turn = false;
        bool ui_attached = false;
        std::atomic<bool> approve_turn{false};
        bool plan_mode = false;
        std::string goal;
        std::string permission_mode = "ask";
        AgentPhase phase = AgentPhase::WaitingModel;
        std::string phase_detail;
        std::wstring phase_text;
        std::wstring reasoning_preview;
        std::string pending_archive_path;
        std::string pending_delete_path;
    };

    struct ApprovalContext {
        ApprovalRequest request;
        std::shared_ptr<TurnRuntime> runtime;
        bool approved = false;
    };

    struct ProjectEntry {
        std::string name;
        std::string workspace;
        uint64_t last_opened_at = 0;
        bool pinned = false;
        std::vector<std::string> folders;
    };

    struct ModelMenuTarget {
        std::string provider;
        std::string model;
    };

    // The projects list is a flat list of rows so a project can expand into its own chats,
    // exactly like the Codex sidebar.
    enum class SidebarRowKind {
        Project,
        Chat,
        More,
    };

    struct SidebarRow {
        SidebarRowKind kind = SidebarRowKind::Project;
        size_t project = 0;
        std::string session_path;
        std::wstring label;
    };

    struct LayoutMetrics {
        int width = 0;
        int height = 0;
        int sidebar = 0;
        int main_x = 0;
        int main_width = 0;
        int topbar_height = 0;
        int composer_width = 0;
        int panel_top = 0;
        int panel_bottom = 0;
        int panel_left = 0;
        int panel_right = 0;
        int main_right = 0;
        int side_left = 0;
        int side_width = 0;
        int attachment_size = 0;
        int attachment_height = 0;
    };

    struct AttachmentHitArea {
        RECT preview{};
        RECT remove{};
        std::string path;
    };

    struct SubAgentUi {
        SubAgentInfo info;
        std::wstring phase;
        bool opened = false;
        std::string live_text;
        std::string reasoning;
    };

    struct CombinedSidebarMetrics {
        bool needed = false;
        int count = 0;
        int visible = 1;
        int top = 0;
        int max_top = 0;
        int project_count = 0;
        int project_visible = 0;
        int project_top = 0;
        int project_max_top = 0;
        int history_count = 0;
        int history_visible = 0;
        int history_top = 0;
        int history_max_top = 0;
        int track_top = 0;
        int track_bottom = 0;
        int thumb_top = 0;
        int thumb_bottom = 0;
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK ComposerSubclassProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                                 UINT_PTR subclass_id, DWORD_PTR reference_data);
    static LRESULT CALLBACK HistorySubclassProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                                UINT_PTR subclass_id, DWORD_PTR reference_data);
    static LRESULT CALLBACK SidebarListSubclassProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                                    UINT_PTR subclass_id, DWORD_PTR reference_data);
    static LRESULT CALLBACK SidebarScrollbarProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                                 UINT_PTR subclass_id, DWORD_PTR reference_data);
    static LRESULT CALLBACK ModelComboSubclassProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                                   UINT_PTR subclass_id, DWORD_PTR reference_data);
    static LRESULT CALLBACK StatusLabelSubclassProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                                    UINT_PTR subclass_id, DWORD_PTR reference_data);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    void CreateControls();
    LayoutMetrics MeasureLayout() const;
    void LayoutControls();
    void LoadOrCreateConfiguration();
    void ApplyConfiguration(AppConfig config);
    void ReloadMcpWhenIdle();
    McpManager* McpForTurn(const AppConfig& config);
    void ApplyTheme();
    void ApplyLocalization();
    const wchar_t* Text(std::wstring_view message) const;
    void RefreshModelSelector();
    void RefreshModelButton();
    void ResizeModelDropdown();
    void RefreshProjects();
    void SaveProjects();
    void RefreshProjectRows();
    // Keeps the sidebar listing available on a cold start, before any session
    // has been opened or resumed.
    void EnsureJournalStorageRoot();
    void ToggleProjectExpanded(size_t project_index);
    void ShowSectionMenu(POINT anchor, bool for_projects);
    void ShowProjectMenu(POINT anchor, size_t project_index);
    void HandleProjectRowAction(size_t row_index, int action);
    void ShowHistoryMenu(POINT anchor, int history_index);
    void ShowChatContextMenu(POINT anchor, const std::string& session_path);
    void RenameSession(const std::string& session_path);
    void ExportSessionToCodex(const std::string& session_path);
    void ImportCodexSessionInteractive(const std::string& workspace, const std::string& scope);
    void ArchiveSessionPath(const std::string& session_path);
    bool ActivateSessionAfterRemoval(const std::string& removed_session_path,
                                     const std::string& preferred_workspace);
    void NewConversationInProject(size_t project_index);
    void ActivateSidebarRow(int row_index, bool toggle_only);
    CombinedSidebarMetrics MeasureSidebarScrollbar();
    void ScrollSidebarBy(int lines);
    void ScrollSidebarTo(int combined_top);
    void ScrollSidebarToPoint(int y);
    void UpdateSidebarScrollbar();
    void CreateProjectInteractive();
    void EditProjectInteractive(size_t project_index);
    void SortProjects();
    void RefreshHistory();
    void HandleSettingsDataChanged();
    void SetSettingsMode(bool enabled);
    void ResumeSessionPath(const std::string& path);
    void RefreshSessionTitle(const std::string& path);
    void QueueSessionLoad(const std::string& path);
    void PollSessionLoad();
    bool SessionLoading() const;
    // Queues a conversation switch for after the current message finishes. Rebuilding the
    // transcript while a list box is still processing its own click message is what made a
    // single click unreliable.
    void PostSessionActivation(const std::string& path);
    void ApplyModelSelection();
    void SwitchProject(int index);
    void StartTurn();
    void CancelTurn();
    void InterruptTurn();
    void ResumePausedTurn();
    void ToggleSendAction();
    enum class ChatViewReset { Ready, Loading };
    void ClearChatView(ChatViewReset mode = ChatViewReset::Ready);
    void BeginNewSession();
    void PreparePendingConversation(const std::string& scope, const std::string& project_name);
    bool MaterializePendingConversation();
    void ResumeSelectedSession();
    void ChooseWorkspace();
    void ShowAddMenu(POINT anchor);
    void HandleAddAction(AddAction action);
    void ChooseAttachments();
    void AttachSmartSnapshot();
    void PollSmartSnapshot();
    std::string SnapshotContext() const;
    void EditGoal();
    bool AttachClipboardImage();
    void DrawAttachmentPreview(HDC device, const LayoutMetrics& metrics);
    bool IsImageAttachment(const std::string& path) const;
    HBITMAP LoadAttachmentBitmap(const std::string& path);
    void OpenAttachment(const std::string& path);
    void ShowPermissionMenu(POINT anchor);
    void ShowModelMenu(POINT anchor);
    void ShowReasoningMenu(POINT anchor, const std::string& provider_id, const std::string& model_id);
    void ShowSummaryMenu(POINT anchor);
    void ShowSubAgentMenu(POINT anchor);
    void RefreshSubAgentButton();
    void OpenSubAgentChat(const std::string& id);
    void CloseSubAgentChat();
    void RefreshSubAgentChat();
    void CreateSummaryControls();
    void RefreshSummaryPanel(bool load_history = false);
    void OpenSummaryPanel(SummaryTab tab);
    void HandleSummaryAction(SummaryAction action, const std::string& id);
    bool HandleSubAgentEvent(UiEvent* event);
    std::vector<SummaryRow> SummaryRows(bool popup) const;
    void ApplyModelChoice(const std::string& provider_id, const std::string& model_id);
    void HandleHistoryAction(int index, int action);
    void HandleSessionAction(const std::string& path, int action);
    void UpdateSendButton();
    void HandleUiEvent(UiEvent* event);
    void FinishTurn(const std::shared_ptr<TurnRuntime>& runtime, bool success, const std::string& reason);
    void PersistInterruptedTurn(const std::shared_ptr<TurnRuntime>& runtime, uint64_t elapsed_ms);
    bool HasPausedTurn() const;
    uint64_t PausedTurnElapsedMs() const;
    void ClearPausedTurn(SessionJournal& journal);
    void DiscardPausedTurnForNewMessage(SessionJournal& journal);
    bool AskApproval(const std::shared_ptr<TurnRuntime>& runtime, const ApprovalRequest& request);
    std::shared_ptr<TurnRuntime> FindTurn(const std::string& path) const;
    bool AnyTurnRunning() const;
    bool IsSessionRunning(const std::string& path) const;
    bool IsActiveSessionRunning() const;
    void DetachAttachedTurn();
    void UpdateTurnUi();
    void TickTurnWatchdog();
    void CancelAndJoinAllTurns();
    void ToggleToolEntry(size_t index);
    void ToggleToolGroupEntry(size_t index);
    void CopyChatItem(size_t index);
    void EditChatItem(size_t index);
    void SetStatus(const std::wstring& text);
    void SetPhase(const std::shared_ptr<TurnRuntime>& runtime, AgentPhase phase, const std::string& detail);
    void FinishActiveReasoning(bool hide);
    void AttachTurnUi(const std::shared_ptr<TurnRuntime>& runtime);
    void AppendReasoningPreview(const std::shared_ptr<TurnRuntime>& runtime, const std::string& delta);
    void BeginActivityAnimation();
    bool RefreshExternalRuns();
    void PollExternalRuns();
    bool RouteMouseWheelToChat(WPARAM wparam, LPARAM lparam);
    void StopActivityAnimation();
    void TickActivity();
    // Repaints the single sidebar row that owns a running turn.
    void InvalidateSidebarRow(const std::string& session_key);
    // Streaming answers and running tools already show their own progress inside the
    // transcript, so the shared status line stays hidden during those phases.
    bool ActivityRowVisible() const;
    std::string ComposerText() const;
    void ClearComposer();
    void RefreshContextLabel();
    void ReloadSessionIntoView();
    void RestoreComposerState();
    void SaveComposerState();
    void ClearComposerState();
    std::string ComposerStatePath() const;
    std::string PendingComposerStatePath() const;
    std::string ReadActiveSessionPath() const;
    void WriteActiveSessionPath() const;
    std::string DataDirectory() const;
    bool FindProjectName(const std::string& workspace, std::string* name) const;
    std::string PrepareNoProjectConversationWorkspace(std::string* error) const;
    void UpdateActiveSessionScope(const std::string& scope, const std::string& workspace);
    void UpdateEmptyState();
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu();
    void RestoreFromTray();
    void OpenSettingsPage();
    void RequestExit();
    int Dpi(int pixels) const;
    static HBRUSH DarkBrush(COLORREF color);

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND sidebar_title_ = nullptr;
    HWND new_button_ = nullptr;
    HWND project_label_ = nullptr;
    HWND project_menu_button_ = nullptr;
    HWND project_add_button_ = nullptr;
    HWND projects_list_ = nullptr;
    HWND sidebar_scrollbar_ = nullptr;
    HWND recent_label_ = nullptr;
    HWND recent_menu_button_ = nullptr;
    HWND recent_new_button_ = nullptr;
    HWND history_list_ = nullptr;
    HWND settings_button_ = nullptr;
    HWND main_title_label_ = nullptr;
    HWND context_label_ = nullptr;
    HWND summary_button_ = nullptr;
    HWND summary_tooltip_ = nullptr;
    HWND subagent_button_ = nullptr;
    HWND model_label_ = nullptr;
    HWND model_combo_ = nullptr;
    HWND model_button_ = nullptr;
    HWND permission_button_ = nullptr;
    HWND plus_button_ = nullptr;
    HWND composer_ = nullptr;
    HWND send_button_ = nullptr;
    HWND workspace_label_ = nullptr;
    HWND status_label_ = nullptr;
    HWND side_title_label_ = nullptr;
    HWND side_close_button_ = nullptr;
    HWND empty_mark_ = nullptr;
    HWND empty_title_ = nullptr;
    HFONT ui_font_ = nullptr;
    HFONT chat_font_ = nullptr;
    HFONT mono_font_ = nullptr;
    HFONT brand_font_ = nullptr;
    HFONT empty_mark_font_ = nullptr;
    HFONT empty_title_font_ = nullptr;
    HBRUSH background_brush_ = nullptr;
    HBRUSH panel_brush_ = nullptr;
    HBRUSH input_brush_ = nullptr;
    HICON tray_icon_ = nullptr;
    AppConfig config_;
    SessionJournal journal_;
    SessionLoader session_loader_;
    std::string session_load_path_;
    uint64_t session_load_generation_ = 0;
    bool session_view_loading_ = false;
    uint64_t session_load_started_tick_ = 0;
    std::function<bool()> session_replay_step_;
    std::vector<std::unique_ptr<UiEvent>> session_deferred_events_;
    size_t session_deferred_bytes_ = 0;
    bool session_deferred_overflow_ = false;
    SettingsWindow settings_;
    std::unique_ptr<McpManager> mcp_;
    std::unique_ptr<McpManager> mcp_network_override_;
    bool mcp_network_discovered_ = false;
    std::unique_ptr<SubAgentManager> subagents_;
    std::unique_ptr<ChatView> chat_view_;
    std::unique_ptr<ChatView> side_chat_view_;
    std::unique_ptr<SummaryView> summary_popup_;
    std::unique_ptr<SummaryView> summary_panel_;
    std::unique_ptr<AddMenu> add_menu_;
    SnapshotShortcut snapshot_shortcut_;
    SnapshotCapture snapshot_capture_;
    std::string snapshot_draft_path_;
    std::map<std::string, std::shared_ptr<TurnRuntime>> active_turns_;
    std::mutex mcp_mutex_;
    bool mcp_reload_pending_ = false;
    std::string attached_turn_key_;
    bool close_pending_ = false;
    bool populating_ = false;
    bool settings_mode_ = false;
    bool configuration_initialized_ = false;
    bool settings_theme_dirty_ = false;
    bool settings_language_dirty_ = false;
    std::atomic<bool> mcp_discovered_{false};
    bool assistant_stream_started_ = false;
    bool plan_mode_ = false;
    bool composer_dirty_ = false;
    int hovered_history_item_ = -1;
    std::string goal_;
    std::string permission_mode_ = "ask";
    std::vector<std::string> attachments_;
    std::map<std::string, std::string> snapshot_contexts_;
    std::map<std::string, HBITMAP> attachment_bitmaps_;
    std::vector<AttachmentHitArea> attachment_hit_areas_;
    std::vector<std::string> history_paths_;
    size_t history_more_index_ = static_cast<size_t>(-1);
    std::string pending_history_action_path_;
    int pending_history_action_ = 0;
    bool pending_history_more_ = false;
    std::string pending_history_click_path_;
    // Last context size shown for each conversation, keyed by normalized session path. The value
    // comes from the provider (or from a compaction) so the header and the usage card agree.
    std::map<std::string, uint64_t> session_context_tokens_;
    std::vector<ProjectEntry> projects_;
    std::vector<SidebarRow> project_rows_;
    std::vector<bool> project_expanded_;
    std::map<std::string, bool> project_show_all_;
    int hovered_project_item_ = -1;
    std::string pending_project_action_path_;
    int pending_project_action_ = 0;
    bool pending_project_more_ = false;
    std::string pending_project_click_path_;
    bool project_section_expanded_ = true;
    bool recent_section_expanded_ = true;
    bool recent_show_all_ = false;
    bool show_active_placeholder_ = false;
    bool pending_new_conversation_ = false;
    std::string active_session_scope_ = "recent";
    std::string active_project_name_;
    bool sidebar_scrollbar_dragging_ = false;
    bool tray_icon_added_ = false;
    bool exiting_ = false;
    int activity_frame_ = 0;
    bool activity_timer_active_ = false;
    // MCP-created sessions run in a separate process. This mirror is refreshed
    // from data/external_agents so their sidebar spinners and transcript refresh
    // use the same UI path as turns started by this window.
    std::set<std::string> external_running_sessions_;
    std::map<std::string, nlohmann::json> external_run_details_;
    std::map<std::string, nlohmann::json> external_run_states_;
    std::map<std::string, std::pair<ULONGLONG, FILETIME>> external_session_stamps_;
    AgentPhase current_phase_ = AgentPhase::WaitingModel;
    // The waiting phase has to show how long the provider has been thinking, plus
    // the tail of the reasoning stream, otherwise a slow prefill looks like a hang.
    uint64_t turn_started_tick_ = 0;
    std::wstring phase_text_;
    std::wstring reasoning_preview_;
    std::string organize_mode_ = "project";
    std::string chat_sort_ = "priority";
    std::map<UINT, std::wstring> menu_item_texts_;
    std::map<UINT, bool> menu_submenu_items_;
    std::map<UINT, ModelMenuTarget> model_menu_targets_;
    std::map<UINT, std::string> reasoning_menu_targets_;
    std::map<UINT, size_t> project_menu_targets_;
    std::map<UINT, int> history_menu_targets_;
    std::map<std::string, size_t> active_tool_entries_;
    std::map<std::string, SubAgentUi> subagents_ui_;
    std::map<UINT, std::string> subagent_menu_targets_;
    std::map<UINT, std::wstring> source_menu_texts_;
    std::string active_subagent_id_;
    size_t side_assistant_entry_ = static_cast<size_t>(-1);
    bool side_chat_open_ = false;
    SummaryTab summary_tab_ = SummaryTab::Agents;
    bool summary_show_all_ = false;
    bool summary_agents_expanded_ = false;
    size_t side_turn_entry_ = static_cast<size_t>(-1);
    size_t side_reasoning_entry_ = static_cast<size_t>(-1);
    std::map<std::string, size_t> side_tool_entries_;
    size_t active_turn_entry_ = static_cast<size_t>(-1);
    size_t active_assistant_entry_ = static_cast<size_t>(-1);
    size_t active_reasoning_entry_ = static_cast<size_t>(-1);
    size_t latest_reasoning_entry_ = static_cast<size_t>(-1);
    size_t pending_edit_index_ = static_cast<size_t>(-1);
    bool resume_turn_requested_ = false;
    std::string pending_archive_path_;
    std::string pending_delete_path_;
    std::string config_path_;
    std::string active_workspace_path_;
};

}  // namespace spark
