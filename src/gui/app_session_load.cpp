#include "gui/app.h"
#include "gui/localization.h"
#include "platform/clock.h"

namespace spark {

void AppWindow::ResumeSessionPath(const std::string& path) {
    if (path.empty()) return;
    const std::string key = NormalizePathForComparison(path);
    if (key == NormalizePathForComparison(session_load_path_)) return;
    if (!SessionLoading() && !pending_new_conversation_ && key == NormalizePathForComparison(journal_.Path())) return;
    QueueSessionLoad(path);
}

void AppWindow::QueueSessionLoad(const std::string& path) {
    SaveComposerState();
    CloseSubAgentChat();
    ClearChatView(ChatViewReset::Loading);
    session_load_started_tick_ = MonotonicMillis();
    session_load_path_ = path;
    if (chat_view_ != nullptr) chat_view_->BeginRestore(session_load_started_tick_);
    UpdateSendButton();
    SetWindowTextW(context_label_, L"");
    RefreshSessionTitle(path);
    SetStatus({});
    UpdateEmptyState();
    session_load_generation_ = session_loader_.Request(path);
    SetTimer(window_, kSessionLoadTimer, 16, nullptr);
}

bool AppWindow::SessionLoading() const {
    return session_view_loading_;
}

void AppWindow::PollSessionLoad() {
    if (!SessionLoading()) return;
    if (!session_load_path_.empty()) {
        auto loaded = session_loader_.Poll();
        if (loaded == nullptr || loaded->generation != session_load_generation_) return;
        auto deferred = std::move(session_deferred_events_);
        const size_t deferred_bytes = session_deferred_bytes_;
        const bool deferred_overflow = session_deferred_overflow_;
        session_load_path_.clear();
        if (!loaded->error.empty()) {
            ReloadSessionIntoView();
            RefreshContextLabel();
            SetStatus(i18n::Format(L"Session restore failed: {error}", {{L"error", Utf8ToWide(loaded->error)}}));
        } else {
            journal_ = std::move(loaded->journal);
            show_active_placeholder_ = false;
            const auto& state = journal_.State();
            active_workspace_path_ = state.workspace.empty() ? config_.workspace.path : state.workspace;
            const std::string scope = state.metadata.value("scope", std::string("recent"));
            if (scope == "project" && !FindProjectName(state.workspace, nullptr)) {
                active_workspace_path_ = config_.workspace.path;
            }
            UpdateActiveSessionScope(scope, state.workspace);
            ReloadSessionIntoView();
            RestoreComposerState();
            RefreshContextLabel();
            WriteActiveSessionPath();
        }
        session_deferred_events_ = std::move(deferred);
        session_deferred_bytes_ = deferred_bytes;
        session_deferred_overflow_ = deferred_overflow;
    }
    if (session_replay_step_) {
        if (!session_replay_step_()) return;
        session_replay_step_ = {};
    }
    if (chat_view_ != nullptr && chat_view_->Loading()) return;
    session_view_loading_ = false;
    session_load_started_tick_ = 0;
    KillTimer(window_, kSessionLoadTimer);
    EnableWindow(composer_, TRUE);
    EnableWindow(plus_button_, TRUE);
    const auto running = FindTurn(journal_.Path());
    if (!pending_new_conversation_ && running != nullptr) {
        running->ui_attached = true;
        attached_turn_key_ = running->session_key;
        AttachTurnUi(running);
        SetTimer(window_, kTurnWatchdogTimer, 1000, nullptr);
    }
    if (session_deferred_overflow_) {
        QueueSessionLoad(journal_.Path());
        return;
    }
    auto deferred = std::move(session_deferred_events_);
    session_deferred_bytes_ = 0;
    for (auto& event : deferred) HandleUiEvent(event.release());
    RefreshContextLabel();
    UpdateTurnUi();
    UpdateEmptyState();
    if (history_list_ != nullptr) InvalidateRect(history_list_, nullptr, FALSE);
    if (projects_list_ != nullptr) InvalidateRect(projects_list_, nullptr, FALSE);
}

}  // namespace spark
