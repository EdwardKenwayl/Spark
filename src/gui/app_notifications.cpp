#include "gui/app.h"
#include "gui/localization.h"

namespace spark {

void AppWindow::NotifyTurnCompletion(const std::string& path, const SessionState& state) {
    if (exiting_ || close_pending_ || !CompletionNotifications::IsBackground(window_)) return;
    if (NormalizePathForComparison(ParentPath(path)) !=
        NormalizePathForComparison(JoinPath(config_.storage.data_dir, "sessions"))) return;
    auto notice = MakeCompletionNotice(path, state);
    if (!notice) return;
    if (notice->title.empty()) notice->title = i18n::Text(L"Task completed");
    if (completion_notifications_.Show(window_, std::move(*notice), theme::For(config_.ui.theme),
        [this](const std::string& target) { OpenCompletedConversation(target); })) {
        MessageBeep(MB_OK);
    }
}

void AppWindow::OpenCompletedConversation(const std::string& path) {
    if (exiting_ || close_pending_ || path.empty()) return;
    // The normal loader saves the previous draft, selects project scope, and
    // cancels stale pending reads. Notifications never bypass that route.
    RestoreFromTray();
    SetSettingsMode(false);
    QueueSessionLoad(path);
}

}  // namespace spark
