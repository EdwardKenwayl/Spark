#include "gui/app.h"
#include "gui/attachment_picker.h"

#include <algorithm>

namespace spark {

void AppWindow::ShowAddMenu(POINT) {
    if (!add_menu_) {
        add_menu_ = std::make_unique<AddMenu>();
        if (!add_menu_->Create(window_, instance_, ui_font_)) {
            add_menu_.reset();
            return;
        }
    }
    const auto layout = MeasureLayout();
    RECT composer{layout.panel_left, layout.panel_top, layout.panel_right, layout.panel_bottom};
    MapWindowPoints(window_, nullptr, reinterpret_cast<POINT*>(&composer), 2);
    add_menu_->Show(composer, config_.ui.theme, snapshot_shortcut_.TargetTitle(),
                   Utf8ToWide(goal_), plan_mode_);
}

void AppWindow::HandleAddAction(AddAction action) {
    if (exiting_) return;
    SetFocus(composer_);
    if (action == AddAction::Files) ChooseAttachments();
    else if (action == AddAction::Snapshot) AttachSmartSnapshot();
    else if (action == AddAction::Goal) EditGoal();
    else if (action == AddAction::Plan) {
        plan_mode_ = !plan_mode_;
        SaveComposerState();
    }
}

void AppWindow::ChooseAttachments() {
    const auto selected = PickAttachments(window_);
    if (!selected.ok) {
        SetStatus(Utf8ToWide(selected.error));
        return;
    }
    for (const auto& path : selected.value) {
        if (attachments_.size() >= 64) break;
        if (std::find(attachments_.begin(), attachments_.end(), path) == attachments_.end()) attachments_.push_back(path);
    }
    SaveComposerState();
    LayoutControls();
    UpdateSendButton();
    InvalidateRect(window_, nullptr, FALSE);
    SetFocus(composer_);
}

void AppWindow::AttachSmartSnapshot() {
    if (exiting_ || settings_mode_) return;
    if (snapshot_capture_.Pending()) return;
    if (attachments_.size() >= 64) {
        SetStatus(Text(L"Remove an attachment before adding another."));
        return;
    }
    const HWND target = snapshot_shortcut_.Target();
    if (!target) {
        SetStatus(Text(L"Switch to the app to attach, then press both Alt keys."));
        return;
    }
    if (add_menu_) add_menu_->Hide();
    const auto started = snapshot_capture_.Start(target, JoinPath(JoinPath(config_.storage.data_dir, "drafts"), "snapshots"));
    if (!started.ok) {
        SetStatus(Utf8ToWide(started.error));
        return;
    }
    snapshot_draft_path_ = ComposerStatePath();
    SetTimer(window_, kSnapshotPollTimer, 50, nullptr);
}

void AppWindow::PollSmartSnapshot() {
    const auto result = snapshot_capture_.Poll();
    if (!result) return;
    KillTimer(window_, kSnapshotPollTimer);
    if (!result->ok) {
        SetStatus(Utf8ToWide(result->error));
        return;
    }
    // A delayed result must never leak into another conversation's draft.
    if (exiting_ || ComposerStatePath() != snapshot_draft_path_ || attachments_.size() >= 64) {
        DeleteFileW(Utf8ToWide(result->value.image_path).c_str());
        return;
    }
    attachments_.push_back(result->value.image_path);
    snapshot_contexts_[result->value.image_path] = result->value.context;
    SaveComposerState();
    LayoutControls();
    UpdateSendButton();
    InvalidateRect(window_, nullptr, FALSE);
}

std::string AppWindow::SnapshotContext() const {
    std::string context;
    for (const auto& path : attachments_) {
        const auto snapshot = snapshot_contexts_.find(path);
        if (snapshot == snapshot_contexts_.end()) continue;
        if (context.empty()) context = "[智能快照]\n";
        const std::string fragment = TruncateUtf8(snapshot->second, 2048) + "\n\n";
        if (context.size() + fragment.size() > 8192) break;
        context += fragment;
    }
    return context;
}

}  // namespace spark
