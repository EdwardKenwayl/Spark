#include "gui/native/app.h"
#include "gui/native/summary_popup.h"
#include "platform/clock.h"

#include <wx/weakref.h>
#include <algorithm>

namespace spark {
namespace {
std::vector<SourceReference> Sources(const SessionState& state) {
    std::vector<SourceReference> sources;
    std::set<std::string> seen;
    const auto add = [&](SourceReference source) {
        if (sources.size() >= NativeSummaryPanel::kMaxSources || source.kind == "subagent" ||
            source.detail.size() > 4096) return;
        source.label = TruncateUtf8(source.label, 2048);
        const bool path = source.kind == "image" || source.kind == "file";
        const bool web = source.kind == "url_citation" || source.kind == "web_source" ||
            source.kind == "web_open" || source.kind == "web_find" || source.kind == "web";
        const std::string key = (web ? "web" : source.kind) + "\n" +
            (path ? NormalizePathForComparison(source.detail) : source.detail);
        if (seen.insert(key).second) sources.push_back(std::move(source));
    };
    for (const auto& message : state.messages) {
        for (const auto& path : message.image_paths)
            add({"image", PathToUtf8(NativePath(path).filename()), path});
        for (const auto& source : message.sources) add(source);
        if (sources.size() >= NativeSummaryPanel::kMaxSources) break;
    }
    return sources;
}
}  // namespace

void NativeFrame::ResetSideConversation() {
    side_loader_.Cancel();
    ++side_generation_;
    side_path_.clear();
    pending_side_.reset();
    side_loading_ = false;
    side_chat_->SetTranscript(nullptr);
    side_chat_->SetLoading(0);
    side_transcript_->Clear();
}

void NativeFrame::RefreshSummary(bool load_history) {
    const bool changed_parent = summary_parent_ != active_;
    if (changed_parent) {
        summary_parent_ = active_;
        ResetSideConversation();
        side_panel_->ShowAgents();
        summary_children_.clear();
        summary_thumbnails_.Clear();
    }
    const auto live = runtime_.Children(active_);
    if (load_history || changed_parent) {
        summary_children_ = active_.empty() ? std::vector<SubAgentInfo>{} :
            SubAgentManager::LoadHistory(config_.storage.data_dir, NormalizePathForComparison(active_));
    }
    for (const auto& child : live) {
        const auto found = std::find_if(summary_children_.begin(), summary_children_.end(),
            [&](const auto& old) { return old.id == child.id; });
        if (found == summary_children_.end()) summary_children_.push_back(child);
        else *found = child;
    }
    std::stable_sort(summary_children_.begin(), summary_children_.end(), [](const auto& left, const auto& right) {
        if ((left.state == SubAgentState::Running) != (right.state == SubAgentState::Running))
            return left.state == SubAgentState::Running;
        return left.started_at != right.started_at ? left.started_at > right.started_at : left.id < right.id;
    });
    if (summary_children_.size() > NativeSummaryPanel::kMaxAgents)
        summary_children_.resize(NativeSummaryPanel::kMaxAgents);
    summary_sources_ = Active() ? Sources(Active()->journal.State()) : std::vector<SourceReference>{};
    for (const auto& source : summary_sources_) if (source.kind == "image")
        summary_thumbnails_.Request(source.detail, std::min(FromDIP(28), NativeSummaryPanel::kMaxThumbnailPixels));
    side_panel_->SetAgents(summary_children_);
    side_panel_->SetSources(summary_sources_);
}

void NativeFrame::Summary() {
    RefreshSummary(true);
    const auto children = summary_children_;
    wxWeakRef<NativeFrame> owner(this);
    const auto parent = active_;
    ShowNativeSummaryPopup(this, summary_->ClientToScreen(wxPoint(summary_->GetSize().x, summary_->GetSize().y)),
        children, summary_sources_, [owner, parent](NativeSummaryPopupAction action, const std::string& id) {
            if (!owner || owner->IsBeingDeleted() || owner->active_ != parent) return;
            if (action == NativeSummaryPopupAction::Agent) {
                const auto found = std::find_if(owner->summary_children_.begin(), owner->summary_children_.end(),
                    [&](const auto& child) { return child.id == id; });
                if (found != owner->summary_children_.end()) owner->OpenChild(*found);
            } else {
                owner->side_panel_->ShowSources();
                owner->side_panel_->Show();
                owner->LayoutShell();
            }
        });
}

void NativeFrame::OpenChild(const SubAgentInfo& child) {
    if (active_.empty() || child.session_path.empty()) return;
    side_panel_->AddAgentTab(child);
    side_panel_->SelectAgentTab(child.id);
    side_panel_->Show();
    LayoutShell();
}

void NativeFrame::SelectChild(const std::string& id) {
    ResetSideConversation();
    const auto found = std::find_if(summary_children_.begin(), summary_children_.end(),
        [&](const auto& child) { return child.id == id; });
    if (found == summary_children_.end() || found->session_path.empty()) return;
    side_path_ = found->session_path;
    side_generation_ = side_loader_.Request(side_path_);
    side_loading_ = true;
    side_chat_->SetLoading(MonotonicMillis());
}

}  // namespace spark
