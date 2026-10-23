#include "gui/native/app.h"
#include "gui/native/approval_prompt.h"
#include "gui/localization.h"
#include "core/external_agents.h"
#include "platform/clock.h"

#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/weakref.h>
#include <algorithm>
#include <csignal>
#include <cerrno>

namespace spark {
namespace {
wxString U(const std::string& text) { return wxString::FromUTF8(text.data(), text.size()); }
std::string S(const wxString& text) { return text.ToUTF8().data(); }
TranscriptTime Now() { return {UnixTimeSeconds(), MonotonicMillis()}; }
size_t Add(Transcript& model, ChatItemKind kind, const std::string& text = {}) {
    ChatItem item; item.kind = kind; item.text = text;
    return model.Apply({TranscriptEventKind::Add, kNoChatItem, std::move(item)}, Now());
}
void HideReasoning(Transcript& model, size_t& row) {
    if (row != kNoChatItem) model.Apply({TranscriptEventKind::Hide, row, {}}, Now());
    row = kNoChatItem;
}
bool LiveProcess(uint64_t id) {
    return id > 0 && id <= 2147483647 && (::kill(static_cast<pid_t>(id), 0) == 0 || errno == EPERM);
}
}  // namespace

bool NativeFrame::Pause() {
    if (runtime_.Running(active_)) { runtime_.Pause(active_); return true; }
    const auto remote = external_.find(active_);
    if (remote != external_.end()) {
        const nlohmann::json command = {{"action", "cancel"}, {"id", remote->second.value("id", std::string{})},
                                        {"requested_at", UnixTimeSeconds()}};
        WriteFileUtf8Atomic(JoinPath(JoinPath(config_.storage.data_dir, "external_agents"), "control.json"), command.dump());
        return true;
    }
    return false;
}

void NativeFrame::Send() {
    if (!loading_path_.empty()) return;
    if (Pause()) return;
    const auto validation = ValidateConfig(config_);
    if (!validation.empty()) { Error(validation.front()); return; }
    const std::string text = Trim(S(composer_->GetValue()));
    View* view = Active();
    const bool resume = view && text.empty() && attachments_.empty() &&
                        view->journal.State().metadata.contains("paused_turn");
    if (text.empty() && attachments_.empty() && !resume) return;
    const std::string pending_draft = DraftPath();
    if (!view) {
        auto created = std::make_unique<View>();
        std::string workspace = project_workspace_, error;
        if (workspace.empty()) workspace = CreateNoProjectConversationWorkspace(config_.workspace.path, &error);
        if (workspace.empty()) { Error(error); return; }
        const auto opened = created->journal.Open(config_.storage.data_dir, workspace);
        if (!opened.ok) { Error(opened.error); return; }
        created->journal.MutableState().metadata["scope"] = project_workspace_.empty() ? "recent" : "project";
        created->journal.Append("session.scope", {{"scope", project_workspace_.empty() ? "recent" : "project"}});
        created->journal.MutableState().title = TruncateUtf8(text.empty() ? "Attachment" : text, 80);
        created->journal.Append("session.title", {{"title", created->journal.State().title}});
        active_ = created->journal.Path();
        view = created.get();
        views_[active_] = std::move(created);
        title_->SetLabel(U(view->journal.State().title));
        chat_->SetTranscript(&view->transcript, true);
    }
    if (edit_row_ != kNoChatItem) {
        size_t turn = 0;
        for (size_t index = 0; index < edit_row_; ++index)
            if (view->transcript.Items()[index].kind == ChatItemKind::User) ++turn;
        auto& state = view->journal.MutableState();
        size_t count = 0, boundary = state.messages.size();
        for (size_t index = 0; index < state.messages.size(); ++index)
            if (state.messages[index].role == MessageRole::User && count++ == turn) { boundary = index; break; }
        state.messages.resize(boundary);
        state.context_messages.clear();
        for (const char* key : {"turn_durations_ms", "turn_usages"}) {
            auto& array = state.metadata[key];
            if (array.is_array() && array.size() > turn) array.erase(array.begin() + static_cast<ptrdiff_t>(turn), array.end());
        }
        auto& errors = state.metadata["turn_errors"];
        if (errors.is_array()) errors.erase(std::remove_if(errors.begin(), errors.end(), [turn](const nlohmann::json& value) {
            return value.is_object() && value.value("turn_index", uint64_t{0}) >= turn;
        }), errors.end());
        state.metadata.erase("paused_turn");
        view->journal.SaveSnapshot();
        view->transcript.EraseFrom(edit_row_);
        edit_row_ = kNoChatItem;
    }
    AppConfig run_config = config_;
    ApplyPermissionMode(&run_config, permissions_->GetSelection() == 2 ? "full_access" :
                                     permissions_->GetSelection() == 1 ? "approve_for_me" : "ask");
    run_config.workspace.path = view->journal.State().workspace;
    AgentTurnOptions options;
    options.mode = plan_ ? CollaborationMode::Plan : CollaborationMode::Execute;
    options.goal = goal_;
    std::string message = text.empty() && !resume ? "Please inspect the attached content." : text;
    std::vector<std::string> images;
    for (const auto& path : attachments_) {
        std::error_code error;
        if (spark::fs::is_directory(NativePath(path), error)) {
            message += "\nAttached folder: " + path;
            continue;
        }
        const auto extension = LowerAscii(PathToUtf8(NativePath(path).extension()));
        if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
            extension == ".bmp" || extension == ".gif" || extension == ".webp") images.push_back(path);
        else message += "\nAttached file: " + path;
    }
    const auto started = runtime_.Start(run_config, view->journal, message, resume, images, options);
    if (!started.ok) { Error(started.error); return; }
    if (!resume) {
        Add(view->transcript, ChatItemKind::User, message);
        view->status = Add(view->transcript, ChatItemKind::TurnStatus);
    } else {
        ChatItem status;
        status.kind = ChatItemKind::TurnStatus;
        status.start_tick_ms = runtime_.Started(active_);
        status.elapsed_ms = view->journal.State().metadata.value("paused_turn", nlohmann::json::object())
            .value("elapsed_ms", uint64_t{0});
        for (size_t index = 0; index < view->transcript.Count(); ++index)
            if (view->transcript.Items()[index].kind == ChatItemKind::TurnStatus) view->status = index;
        if (view->status == kNoChatItem)
            view->status = view->transcript.Apply({TranscriptEventKind::Add, kNoChatItem, status}, Now());
        else view->transcript.Apply({TranscriptEventKind::ResumeTurn, view->status, status}, Now());
    }
    view->assistant = view->reasoning = kNoChatItem;
    view->tools.clear();
    composer_->ChangeValue("");
    attachments_.clear();
    RefreshAttachments();
    SaveDraft();
    if (pending_draft != DraftPath()) {
        std::error_code ignored;
        spark::fs::remove(NativePath(pending_draft), ignored);
    }
    chat_->Changed();
    chat_->ScrollToLatest();
    UpdateControls();
    RefreshSidebar();
}

void NativeFrame::Consume(const NativeEvent& event) {
    if (event.kind == NativeEvent::Kind::Child) {
        if (event.path == active_) {
            RefreshSummary();
            const auto found = std::find_if(summary_children_.begin(), summary_children_.end(),
                [&](const auto& child) { return child.id == event.child.id; });
            if (found != summary_children_.end()) *found = event.child;
            else if (summary_children_.size() < NativeSummaryPanel::kMaxAgents) summary_children_.push_back(event.child);
            side_panel_->SetAgents(summary_children_);
        }
        return;
    }
    const auto found = views_.find(event.path);
    if (found == views_.end()) return;
    auto& view = *found->second;
    const auto finish_assistant = [&] {
        if (view.assistant != kNoChatItem)
            view.transcript.Apply({TranscriptEventKind::Finish, view.assistant, {}}, Now());
        view.assistant = kNoChatItem;
    };
    if (event.kind == NativeEvent::Kind::Text) {
        HideReasoning(view.transcript, view.reasoning);
        if (view.assistant == kNoChatItem) view.assistant = Add(view.transcript, ChatItemKind::Assistant);
        ChatItem delta; delta.text = event.first;
        view.transcript.Apply({TranscriptEventKind::AppendText, view.assistant, delta}, Now());
    } else if (event.kind == NativeEvent::Kind::Reasoning) {
        if (view.reasoning == kNoChatItem) view.reasoning = Add(view.transcript, ChatItemKind::Reasoning);
        ChatItem delta; delta.text = event.first;
        view.transcript.Apply({TranscriptEventKind::AppendText, view.reasoning, delta}, Now());
    } else if (event.kind == NativeEvent::Kind::ToolStart) {
        finish_assistant();
        HideReasoning(view.transcript, view.reasoning);
        ChatItem tool;
        tool.kind = ChatItemKind::Tool;
        tool.call_id = event.first; tool.tool_name = event.second; tool.arguments = event.third;
        view.tools[event.first] = view.transcript.Apply({TranscriptEventKind::Add, kNoChatItem, tool}, Now());
    } else if (event.kind == NativeEvent::Kind::ToolFinish) {
        const auto item = view.tools.find(event.first);
        if (item != view.tools.end()) {
            ChatItem result; result.success = event.success; result.result = event.third;
            view.transcript.Apply({TranscriptEventKind::Finish, item->second, result}, Now());
            view.tools.erase(item);
        }
    } else if (event.kind == NativeEvent::Kind::Compact) {
        ChatItem notice;
        notice.kind = ChatItemKind::Compaction;
        notice.success = event.success;
        view.transcript.Apply({TranscriptEventKind::Add, kNoChatItem, notice}, Now());
    } else if (event.kind == NativeEvent::Kind::Context) {
        view.context = event.first_number; view.limit = event.second_number;
    } else if (event.kind == NativeEvent::Kind::Phase) {
        if (event.phase == AgentPhase::WaitingModel && !view.transcript.NeedsAnimation())
            view.status = Add(view.transcript, ChatItemKind::TurnStatus);
    } else if (event.kind == NativeEvent::Kind::Done) {
        if (event.path == active_) RefreshSummary();
        const auto completed = runtime_.FinishedJournal(event.path);
        if (completed.ok) {
            view.journal = completed.value;
            finish_assistant();
            HideReasoning(view.transcript, view.reasoning);
            const auto& metadata = view.journal.State().metadata;
            const bool paused = metadata.contains("paused_turn");
            const auto& durations = metadata.value("turn_durations_ms", nlohmann::json::array());
            ChatItem final;
            final.elapsed_ms = durations.empty() ? 0 : durations.back().get<uint64_t>();
            for (const auto& tool : view.tools) {
                final.success = false;
                final.result = paused ? "Interrupted" : "Tool did not complete";
                view.transcript.Apply({TranscriptEventKind::Finish, tool.second, final}, Now());
            }
            view.tools.clear();
            if (view.status != kNoChatItem)
                view.transcript.Apply({paused ? TranscriptEventKind::PauseTurn : TranscriptEventKind::FinishTurn,
                                       view.status, final}, Now());
            if (!event.success && !paused && !event.first.empty())
                Add(view.transcript, IsQuotaExhaustedError(event.first) ? ChatItemKind::Quota : ChatItemKind::Error, event.first);
            const auto& usages = metadata.value("turn_usages", nlohmann::json::array());
            if (!usages.empty()) {
                const auto& usage = usages.back();
                ChatItem row;
                row.kind = ChatItemKind::UsageStats;
                row.input_tokens = usage.value("input_tokens", uint64_t{0});
                row.output_tokens = usage.value("output_tokens", uint64_t{0});
                row.cached_input_tokens = usage.value("cached_input_tokens", uint64_t{0});
                row.context_tokens = usage.value("context_tokens", uint64_t{0});
                row.context_limit_tokens = usage.value("context_limit_tokens", uint64_t{0});
                row.elapsed_ms = usage.value("elapsed_ms", uint64_t{0});
                row.model_calls = usage.value("model_calls", uint64_t{0});
                view.context = row.context_tokens;
                view.limit = row.context_limit_tokens;
                view.transcript.Apply({TranscriptEventKind::Add, kNoChatItem, row}, Now());
            }
        }
        RefreshSidebar();
    }
    if (event.path == active_) { chat_->Changed(); UpdateControls(); }
}

void NativeFrame::EditMessage(size_t row) {
    auto* view = Active();
    if (!view || runtime_.Running(active_) || external_.count(active_)) return;
    const auto* item = view->transcript.Item(row);
    if (!item || item->kind != ChatItemKind::User) return;
    composer_->ChangeValue(U(item->text));
    edit_row_ = row;
    composer_->SetFocus();
    UpdateControls();
}

void NativeFrame::Tick(wxTimerEvent&) {
    if (close_pending_) {
        if (summary_thumbnails_.Drain()) { close_pending_ = false; Close(true); }
        return;
    }
    if (draft_dirty_ && MonotonicMillis() - last_draft_ > 500) { SaveDraft(); last_draft_ = MonotonicMillis(); }
    if (summary_thumbnails_.Poll()) side_panel_->RefreshThumbnails();
    for (const auto& event : runtime_.Poll()) Consume(event);
    for (const auto& approval : runtime_.Approvals()) {
        wxWeakRef<NativeFrame> owner(this);
        NativeApprovalPrompt prompt(this, approval.request);
        prompt.ShowModal();
        if (!owner || owner->IsBeingDeleted()) return;
        runtime_.Approve(approval.id, prompt.Decision());
    }
    if (auto loaded = loader_.Poll()) {
        if (loaded->generation == load_generation_) {
            if (!loaded->error.empty()) {
                loading_path_.clear(); chat_->SetLoading(0); UpdateControls(); Error(loaded->error);
            } else {
                pending_view_ = std::make_unique<View>();
                pending_view_->journal = std::move(loaded->journal);
                const bool running = external_.count(loading_path_) != 0;
                uint64_t start = 0;
                const auto& metadata = pending_view_->journal.State().metadata;
                if (running && metadata.contains("live_turn") && metadata["live_turn"].is_object())
                    start = metadata["live_turn"].value("started_tick", uint64_t{0});
                pending_view_->replay = pending_view_->transcript.BeginReplay(pending_view_->journal.State(), Now(), {running, start});
                if (metadata.contains("turn_usages") && metadata["turn_usages"].is_array() && !metadata["turn_usages"].empty()) {
                    const auto& usage = metadata["turn_usages"].back();
                    pending_view_->context = usage.value("context_tokens", uint64_t{0});
                    pending_view_->limit = usage.value("context_limit_tokens", uint64_t{0});
                }
            }
        }
    }
    if (pending_view_ && pending_view_->transcript.ReplayStep(pending_view_->replay)) {
        const std::string path = loading_path_;
        const bool switch_view = !Active() || chat_->Loading(); // Replacing a live snapshot keeps composer draft intact.
        views_[path] = std::move(pending_view_);
        loading_path_.clear(); chat_->SetLoading(0);
        if (active_ == path) {
            chat_->SetTranscript(&views_[path]->transcript, switch_view);
            title_->SetLabel(U(views_[path]->journal.State().title));
            if (switch_view) RestoreDraft();
            UpdateControls();
            if (side_panel_->IsShown()) RefreshSummary(true);
        }
        while (views_.size() > 8) {
            auto remove = std::find_if(views_.begin(), views_.end(), [this](const auto& item) {
                return item.first != active_ && !runtime_.Running(item.first) && !external_.count(item.first);
            });
            if (remove == views_.end()) break;
            views_.erase(remove);
        }
    }
    for (auto& item : views_) if (item.second->transcript.Loading()) {
        if (item.second->transcript.ReplayStep(item.second->replay) && item.first == active_) chat_->Changed();
    }
    if (auto summaries = history_loader_.Poll()) { history_items_ = std::move(*summaries); RenderSidebar(); }
    if (MonotonicMillis() - last_poll_ >= 1000) {
        PollExternal(); RefreshSidebar(); last_poll_ = MonotonicMillis();
        RenderSidebar();
            if (side_panel_->IsShown()) RefreshSummary();
            if (!side_path_.empty() && !side_loading_ && !pending_side_ && MonotonicMillis() - side_poll_ >= 1000) {
            side_generation_ = side_loader_.Request(side_path_);
            side_loading_ = true;
            side_poll_ = MonotonicMillis();
        }
    }
    if (auto loaded = side_loader_.Poll()) {
        if (loaded->generation == side_generation_ && !side_path_.empty() && loaded->error.empty()) {
            pending_side_ = std::make_unique<Transcript>();
            side_replay_ = pending_side_->BeginReplay(loaded->journal.State(), Now(),
                {loaded->journal.State().metadata.contains("live_turn"), 0});
        } else if (loaded->generation == side_generation_ && !side_path_.empty()) {
            side_chat_->SetTranscript(nullptr);
            side_transcript_->Clear();
            Add(*side_transcript_, ChatItemKind::Error, loaded->error);
            side_chat_->SetLoading(0);
            side_chat_->SetTranscript(side_transcript_.get(), true);
        }
        if (loaded->generation == side_generation_) side_loading_ = false;
    }
    if (pending_side_ && pending_side_->ReplayStep(side_replay_)) {
        side_chat_->SetTranscript(nullptr);
        side_transcript_ = std::move(pending_side_);
        side_chat_->SetLoading(0);
        side_chat_->SetTranscript(side_transcript_.get());
        side_chat_->Changed();
    }
    chat_->Tick();
    history_->Tick();
    project_list_->Tick();
    sidebar_rail_->Sync();
    if (side_panel_->IsShown()) side_chat_->Tick();
}

void NativeFrame::PollExternal() {
    std::map<std::string, nlohmann::json> current;
    std::error_code error;
    const auto directory = NativePath(JoinPath(config_.storage.data_dir, "external_agents"));
    spark::fs::directory_iterator iterator(directory, error), end;
    size_t inspected = 0;
    for (; !error && iterator != end && inspected++ < 64; iterator.increment(error)) {
        const auto name = PathToUtf8(iterator->path().filename());
        if (name.rfind("process-", 0) != 0 || iterator->path().extension() != ".json") continue;
        const auto contents = ReadFileUtf8(PathToUtf8(iterator->path()));
        if (!contents || contents->size() > 4 * 1024 * 1024) continue;
        const auto document = nlohmann::json::parse(*contents, nullptr, false);
        if (!IsExternalAgentRegistryFresh(document, UnixTimeSeconds()) ||
            !LiveProcess(document.value("process_id", uint64_t{0})) ||
            !document.contains("runs") || !document["runs"].is_array()) continue;
        for (const auto& run : document["runs"]) {
            if (!run.is_object()) continue;
            const std::string path = run.value("session_path", std::string{});
            const std::string state = run.value("state", std::string{});
            if (path.empty()) continue;
            if (state == "running" || state == "queued" || state == "awaiting_approval" || state == "cancelling")
                current[path] = run;
        }
    }
    external_ = std::move(current);
    for (const auto& entry : external_) {
        const auto& run = entry.second;
        if (!run.value("awaiting_approval", false)) continue;
        const std::string key = run.value("id", std::string{}) + ":" + std::to_string(run.value("approval_id", uint64_t{0}));
        if (!external_approvals_.insert(key).second) continue;
        const bool approved = wxMessageBox(U(run.value("approval_detail", std::string{})),
            U(run.value("approval_title", std::string("Spark"))), wxYES_NO | wxICON_QUESTION, this) == wxYES;
        const nlohmann::json command = {{"action", approved ? "approve" : "deny"}, {"id", run.value("id", std::string{})},
                                       {"requested_at", UnixTimeSeconds()}};
        WriteFileUtf8Atomic(JoinPath(JoinPath(config_.storage.data_dir, "external_agents"), "control.json"), command.dump());
    }
    if (external_approvals_.size() > 1024) external_approvals_.erase(external_approvals_.begin());
    if (!active_.empty() && !runtime_.Running(active_) && loading_path_.empty()) {
        const auto file = NativePath(active_);
        const auto time = spark::fs::last_write_time(file, error);
        if (!error) {
            const auto size = spark::fs::file_size(file, error);
            const auto stamp = std::make_pair(size, time);
            if (!error && stamps_[active_] != stamp) {
                stamps_[active_] = stamp;
                loading_path_ = active_;
                load_generation_ = loader_.Request(active_);
            }
        }
    }
    UpdateControls();
}

}  // namespace spark
