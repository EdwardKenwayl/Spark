#include "gui/native/app.h"
#include "gui/localization.h"
#include "gui/native/popup_menu.h"
#include "gui/native/text_prompt.h"
#include "core/session_exchange.h"
#include "platform/clock.h"

#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/weakref.h>
#include <algorithm>
#include <sstream>

namespace spark {
namespace {
wxString U(const std::string& text) { return wxString::FromUTF8(text.data(), text.size()); }
wxString T(const wchar_t* text) { return wxString(i18n::Text(text)); }
std::string S(const wxString& text) { return text.ToUTF8().data(); }
}  // namespace

void NativeFrame::LoadProjects() {
    std::map<std::string, std::pair<bool, bool>> expansion;
    for (const auto& project : projects_)
        expansion[NormalizePathForComparison(project.workspace)] = {project.expanded, project.show_all};
    projects_.clear();
    const auto text = ReadFileUtf8(JoinPath(config_.storage.data_dir, "projects.json"));
    if (!text) return;
    const auto document = nlohmann::json::parse(*text, nullptr, false);
    if (!document.is_object() || !document.contains("projects") || !document["projects"].is_array()) return;
    organize_mode_ = document.value("organize_mode", std::string("project"));
    chat_sort_ = document.value("chat_sort", std::string("recent"));
    for (const auto& entry : document["projects"]) {
        if (!entry.is_object()) continue;
        Project project;
        project.workspace = entry.value("workspace_path", std::string{});
        project.name = entry.value("name", std::string{});
        project.pinned = entry.value("pinned", false);
        project.last_opened = entry.value("last_opened_at", uint64_t{0});
        if (project.workspace.empty()) continue;
        if (project.name.empty()) project.name = PathToUtf8(NativePath(project.workspace).filename());
        if (entry.contains("folders") && entry["folders"].is_array()) for (const auto& path : entry["folders"])
            if (path.is_string()) project.folders.push_back(path.get<std::string>());
        if (project.folders.empty()) project.folders.push_back(project.workspace);
        const auto old = expansion.find(NormalizePathForComparison(project.workspace));
        if (old != expansion.end()) {
            project.expanded = old->second.first;
            project.show_all = old->second.second;
        }
        projects_.push_back(std::move(project));
    }
}
void NativeFrame::SaveProjects() {
    nlohmann::json document = {{"schema_version", 1}, {"projects", nlohmann::json::array()}};
    const auto old = ReadFileUtf8(JoinPath(config_.storage.data_dir, "projects.json"));
    if (old) {
        auto parsed = nlohmann::json::parse(*old, nullptr, false);
        if (parsed.is_object()) document = std::move(parsed);
    }
    const auto old_projects = document.value("projects", nlohmann::json::array());
    document["projects"] = nlohmann::json::array();
    for (const auto& project : projects_) {
        nlohmann::json value = nlohmann::json::object();
        if (old_projects.is_array()) for (const auto& old_project : old_projects) {
            if (old_project.is_object() &&
                NormalizePathForComparison(old_project.value("workspace_path", std::string{})) ==
                    NormalizePathForComparison(project.workspace)) {
                value = old_project; break;
            }
        }
        value["name"] = project.name; value["workspace_path"] = project.workspace; value["folders"] = project.folders;
        value["pinned"] = project.pinned; value["last_opened_at"] = project.last_opened;
        document["projects"].push_back(std::move(value));
    }
    document["organize_mode"] = organize_mode_;
    document["chat_sort"] = chat_sort_;
    std::error_code error;
    spark::fs::create_directories(NativePath(config_.storage.data_dir), error);
    std::string message;
    if (!WriteFileUtf8Atomic(JoinPath(config_.storage.data_dir, "projects.json"), document.dump(2), &message)) Error(message);
}
void NativeFrame::RefreshSidebar() { history_loader_.Request(config_.storage.data_dir); }
void NativeFrame::RenderSidebar() {
    std::stable_sort(projects_.begin(), projects_.end(), [&](const auto& left, const auto& right) {
        if (left.pinned != right.pinned) return left.pinned > right.pinned;
        return chat_sort_ != "manual" && left.last_opened > right.last_opened;
    });
    std::vector<SidebarRow> project_rows, recent_rows;
    wxArrayString project_labels, recent_labels;
    std::vector<std::vector<const TranscriptHistory*>> chats(projects_.size());
    std::vector<const TranscriptHistory*> recent;
    for (const auto& item : history_items_) {
        if (!item.has_messages) continue;
        size_t match = projects_.size();
        if (item.scope != "recent" && !item.workspace.empty())
            for (size_t index = 0; index < projects_.size() && match == projects_.size(); ++index)
                for (const auto& folder : projects_[index].folders)
                    if (NormalizePathForComparison(item.workspace) == NormalizePathForComparison(folder)) {
                        match = index; break;
                    }
        if (match < projects_.size()) chats[match].push_back(&item);
        else if (item.scope != "project") recent.push_back(&item);
    }
    const auto sort = [&](auto& items) {
        if (chat_sort_ == "manual") return;
        std::stable_sort(items.begin(), items.end(), [&](const auto* left, const auto* right) {
            if (left->pinned != right->pinned) return left->pinned > right->pinned;
            return chat_sort_ == "recent" && left->updated_at > right->updated_at;
        });
    };
    for (size_t index = 0; index < projects_.size(); ++index) {
        project_labels.Add(U(projects_[index].name));
        project_rows.push_back({{}, static_cast<int>(index)});
        if (!projects_[index].expanded || organize_mode_ != "project") continue;
        auto& items = chats[index];
        sort(items);
        const size_t count = projects_[index].show_all ? items.size() : std::min<size_t>(5, items.size());
        for (size_t item = 0; item < count; ++item) {
            project_labels.Add(U(items[item]->title));
            project_rows.push_back({items[item]->path, static_cast<int>(index), true});
        }
        if (count < items.size()) {
            project_labels.Add(T(L"Show more"));
            project_rows.push_back({{}, static_cast<int>(index), true, true});
        }
    }
    sort(recent);
    const size_t count = recent_show_all_ ? recent.size() : std::min<size_t>(5, recent.size());
    for (size_t index = 0; index < count; ++index) {
        recent_labels.Add(U(recent[index]->title));
        recent_rows.push_back({recent[index]->path});
    }
    if (count < recent.size()) {
        recent_labels.Add(T(L"Show more")); recent_rows.push_back({{}, -1, false, true});
    }
    refreshing_ = true;
    const auto update = [&](NativeList* list, std::vector<SidebarRow>& old_rows,
                            std::vector<SidebarRow> rows, const wxArrayString& labels) {
        bool same = rows.size() == old_rows.size();
        for (size_t index = 0; same && index < rows.size(); ++index)
            same = rows[index].path == old_rows[index].path && rows[index].project == old_rows[index].project &&
                   rows[index].more == old_rows[index].more;
        if (same) {
            for (size_t index = 0; index < rows.size(); ++index)
                if (list->GetString(index) != labels[index]) list->SetString(index, labels[index]);
        } else list->Set(labels);
        old_rows = std::move(rows);
        int selection = wxNOT_FOUND;
        for (size_t index = 0; index < old_rows.size(); ++index) {
            const auto& row = old_rows[index];
            NativeListKind kind = row.project_chat ? NativeListKind::ProjectChat : NativeListKind::Chat;
            if (row.more) kind = NativeListKind::More;
            else if (row.path.empty() && row.project >= 0)
                kind = chats[row.project].empty() ? NativeListKind::ProjectEmpty :
                    projects_[row.project].expanded ? NativeListKind::ProjectExpanded : NativeListKind::ProjectCollapsed;
            list->Style(index, kind, !row.path.empty() && (runtime_.Running(row.path) || external_.count(row.path)));
            if (!active_.empty() && row.path == active_) selection = static_cast<int>(index);
        }
        if (list->GetSelection() != selection) list->SetSelection(selection);
    };
    update(project_list_, project_rows_, std::move(project_rows), project_labels);
    update(history_, rows_, std::move(recent_rows), recent_labels);
    refreshing_ = false;
    LayoutSidebar();
}

void NativeFrame::SidebarActivate(NativeList* list, int index, int action) {
    const auto& rows = list == project_list_ ? project_rows_ : rows_;
    if (index < 0 || static_cast<size_t>(index) >= rows.size()) return;
    const auto row = rows[index];
    if (row.more) {
        if (row.project >= 0) projects_[row.project].show_all = true;
        else recent_show_all_ = true;
        RenderSidebar(); return;
    }
    if (!row.path.empty()) {
        if (action) ChatAction(row, action == 1 ? 3 : action == 2 ? 5 : 4);
        else { OpenConversation(row.path); RenderSidebar(); }
        return;
    }
    if (row.project < 0 || static_cast<size_t>(row.project) >= projects_.size()) return;
    if (action == 1) { NewConversation(row.project); return; }
    if (action == 2) {
        const wxRect rect = list->GetItemRect(index);
        ProjectMenu(row.project, list->ClientToScreen(
            {rect.x + rect.width - FromDIP(12), rect.y + rect.height}));
        return;
    }
    const auto& project = projects_[row.project];
    const bool has_chats = std::any_of(history_items_.begin(), history_items_.end(), [&](const auto& item) {
        if (!item.has_messages || item.scope == "recent") return false;
        return std::any_of(project.folders.begin(), project.folders.end(), [&](const auto& folder) {
            return NormalizePathForComparison(folder) == NormalizePathForComparison(item.workspace);
        });
    });
    if (!has_chats || organize_mode_ != "project") return;
    projects_[row.project].expanded = !projects_[row.project].expanded;
    projects_[row.project].show_all = false;
    RenderSidebar();
}
void NativeFrame::NewConversation(int project) {
    SaveDraft();
    loader_.Cancel();
    pending_view_.reset();
    loading_path_.clear();
    active_.clear();
    history_->SetSelection(wxNOT_FOUND);
    project_list_->SetSelection(wxNOT_FOUND);
    edit_row_ = kNoChatItem;
    project_workspace_.clear(); project_name_.clear();
    if (project >= 0 && static_cast<size_t>(project) < projects_.size()) {
        project_workspace_ = projects_[project].workspace;
        project_name_ = projects_[project].name;
        projects_[project].last_opened = UnixTimeSeconds();
        SaveProjects();
    }
    empty_.Clear();
    chat_->SetTranscript(&empty_, true);
    chat_->SetLoading(0);
    const auto title = project_name_.empty() ? T(L"What would you like to do today?") :
        wxString(i18n::Format(L"What would you like to do in {project}?", {{L"project", Utf8ToWide(project_name_)}}));
    chat_->SetEmptyTitle(title);
    title_->SetLabel(T(L"New chat"));
    if (const auto* model = SelectedModel()) config_.active.reasoning_effort = EffectiveDefaultReasoningEffort(*model);
    RefreshModels();
    RestoreDraft();
    UpdateControls();
    side_panel_->Hide();
    RefreshSummary();
    LayoutShell();
    composer_->SetFocus();
    RenderSidebar();
}
void NativeFrame::OpenConversation(const std::string& path) {
    if (path == active_ && loading_path_.empty()) return;
    SaveDraft();
    pending_view_.reset();
    edit_row_ = kNoChatItem;
    const auto cached = views_.find(path);
    active_ = path;
    RefreshSummary();
    project_workspace_.clear(); project_name_.clear();
    for (const auto& item : history_items_) if (item.path == path) {
        title_->SetLabel(U(item.title));
        if (item.scope == "project") {
            project_workspace_ = item.workspace;
            for (const auto& project : projects_)
                if (NormalizePathForComparison(project.workspace) == NormalizePathForComparison(item.workspace))
                    project_name_ = project.name;
        }
        break;
    }
    if (cached != views_.end()) {
        loader_.Cancel();
        loading_path_.clear();
        chat_->SetLoading(0);
        chat_->SetTranscript(&cached->second->transcript, true);
        RestoreDraft(); UpdateControls(); return;
    }
    loading_path_ = path;
    loading_since_ = MonotonicMillis();
    load_generation_ = loader_.Request(path);
    chat_->SetTranscript(&empty_);
    chat_->SetLoading(loading_since_);
    UpdateControls();
}
void NativeFrame::ImportConversation(int project) {
    wxFileDialog file(this, T(L"Import Codex chat branch"), U(DefaultCodexSessionsRoot()), "",
                       "JSONL (*.jsonl)|*.jsonl", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (file.ShowModal() != wxID_OK) return;
    const std::string workspace = project >= 0 ? projects_[project].workspace : config_.workspace.path;
    const auto result = ImportCodexSessionToSpark(S(file.GetPath()), config_.storage.data_dir, workspace,
                                                  project >= 0 ? "project" : "recent");
    if (!result.ok) { Error(result.error); return; }
    RefreshSidebar();
    OpenConversation(result.value.path);
}
void NativeFrame::RecentMenu() {
    SectionMenu(recent_menu_, false);
}
void NativeFrame::SectionMenu(wxWindow* anchor, bool projects) {
    wxMenu menu;
    auto* organize = new wxMenu;
    organize->AppendCheckItem(11, T(L"By project"));
    organize->AppendCheckItem(12, T(L"As a list"));
    organize->Check(organize_mode_ == "list" ? 12 : 11, true);
    menu.AppendSubMenu(organize, T(L"Organize sidebar"));
    auto* sort = new wxMenu;
    sort->AppendCheckItem(21, T(L"Priority"));
    sort->AppendCheckItem(22, T(L"Recently updated"));
    sort->AppendCheckItem(23, T(L"Manual order"));
    sort->Check(chat_sort_ == "priority" ? 21 : chat_sort_ == "manual" ? 23 : 22, true);
    menu.AppendSubMenu(sort, T(L"Chat sort order"));
    if (!projects) { menu.AppendSeparator(); menu.Append(31, T(L"Import Codex chat branch")); }
    wxWeakRef<NativeFrame> owner(this);
    const int selected = ShowNativeMenu(this, menu, anchor->ClientToScreen({0, anchor->GetSize().y}));
    if (!owner || owner->IsBeingDeleted()) return;
    if (selected == 31) { ImportConversation(); return; }
    if (selected == 11 || selected == 12) organize_mode_ = selected == 11 ? "project" : "list";
    else if (selected >= 21 && selected <= 23)
        chat_sort_ = selected == 21 ? "priority" : selected == 22 ? "recent" : "manual";
    else return;
    SaveProjects(); RenderSidebar();
}
void NativeFrame::SidebarMenu(wxContextMenuEvent& event) {
    const wxPoint screen = event.GetPosition() == wxDefaultPosition ? wxGetMousePosition() : event.GetPosition();
    auto* list = event.GetEventObject() == project_list_ ? project_list_ : history_;
    const auto& rows = list == project_list_ ? project_rows_ : rows_;
    const int index = event.GetPosition() == wxDefaultPosition ? list->GetSelection()
        : list->HitTest(list->ScreenToClient(screen));
    if (index == wxNOT_FOUND || static_cast<size_t>(index) >= rows.size() || rows[index].more) return;
    const auto row = rows[index];
    wxMenu menu;
    if (row.path.empty() && row.project >= 0) {
        ProjectMenu(row.project, screen); return;
    }
    if (row.path.empty()) return;
    menu.Append(1, T(L"Rename"));
    menu.Append(2, T(L"Export as Codex chat branch"));
    if (runtime_.Running(row.path) || external_.count(row.path)) menu.Enable(1, false);
    wxWeakRef<NativeFrame> owner(this);
    const int selected = ShowNativeMenu(this, menu, screen);
    if (!owner || owner->IsBeingDeleted()) return;
    ChatAction(row, selected);
}
void NativeFrame::ChatAction(const SidebarRow& selected_row, int selected) {
    const auto row = selected_row;
    if ((selected == 1 || selected == 3 || selected == 4) &&
        (runtime_.Running(row.path) || external_.count(row.path))) return;
    if (selected == 2) {
        wxFileDialog file(this, T(L"Export as Codex chat branch"), "",
                           U(PathToUtf8(NativePath(SuggestedCodexRolloutPath(DefaultCodexSessionsRoot())).filename())),
                           "JSONL (*.jsonl)|*.jsonl", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (file.ShowModal() == wxID_OK) {
            const auto result = ExportSparkSessionToCodex(row.path, S(file.GetPath()), config_.active.provider);
            if (!result.ok) Error(result.error);
        }
    } else if (selected == 1) {
        SessionJournal journal;
        const auto resumed = journal.Resume(row.path);
        if (!resumed.ok) { Error(resumed.error); return; }
        wxString initial = U(journal.State().title);
        if (initial.empty() || initial == "New chat" || initial == L"\u65b0\u5bf9\u8bdd")
            initial = T(L"New chat");
        wxWeakRef<NativeFrame> owner(this);
        NativeTextPrompt name(this, T(L"Rename chat"), initial);
        const int result = name.ShowModal();
        if (!owner || owner->IsBeingDeleted()) return;
        const std::string renamed = TruncateUtf8(Trim(S(name.GetValue())), 120);
        if (result == wxID_OK && !renamed.empty()) {
            journal.MutableState().title = renamed;
            journal.Append("session.title", {{"title", journal.State().title}});
            journal.SaveSnapshot();
            if (active_ == row.path) {
                if (auto* view = Active()) view->journal = std::move(journal);
                if (auto* view = Active()) title_->SetLabel(U(view->journal.State().title));
                LayoutHeader();
            } else views_.erase(row.path);
        }
    } else if (selected == 5) {
        const auto entry = std::find_if(history_items_.begin(), history_items_.end(),
            [&](const auto& item) { return item.path == row.path; });
        SessionJournal journal;
        const auto resumed = journal.Resume(row.path);
        if (!resumed.ok) { Error(resumed.error); return; }
        const auto result = journal.Append("session.pinned",
            {{"pinned", entry == history_items_.end() || !entry->pinned}});
        if (!result.ok) { Error(result.error); return; }
    } else if (selected == 3 || selected == 4) {
        if (selected == 4) {
            wxWeakRef<NativeFrame> owner(this);
            const int choice = wxMessageBox(T(L"Permanently delete this chat? This cannot be undone."),
                T(L"Delete chat"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
            if (!owner || owner->IsBeingDeleted() || choice != wxYES) return;
            if (runtime_.Running(row.path) || external_.count(row.path)) return;
        }
        if (active_ == row.path) NewConversation();
        std::error_code error;
        if (selected == 3) {
            const auto directory = NativePath(JoinPath(config_.storage.data_dir, "archive"));
            spark::fs::create_directories(directory, error);
            if (!error) spark::fs::rename(NativePath(row.path), directory / NativePath(row.path).filename(), error);
        } else spark::fs::remove(NativePath(row.path), error);
        if (error) Error(error.message());
        else views_.erase(row.path);
    }
    RefreshSidebar();
}
std::string NativeFrame::DraftPath() const {
    std::string id;
    const auto view = views_.find(active_);
    if (view != views_.end()) id = view->second->journal.State().id;
    if (id.empty()) {
        uint64_t hash = 14695981039346656037ull;
        const std::string seed = (project_workspace_.empty() ? "recent\n" : "project\n") +
            NormalizePathForComparison(project_workspace_.empty() ? config_.workspace.path : project_workspace_) + "\n" + project_name_;
        for (unsigned char byte : seed) { hash ^= byte; hash *= 1099511628211ull; }
        std::ostringstream value;
        value << "pending-" << (project_workspace_.empty() ? "recent" : "project") << "-" << std::hex << hash;
        id = value.str();
    }
    return JoinPath(JoinPath(config_.storage.data_dir, "drafts"), id + ".json");
}
void NativeFrame::SaveDraft() {
    if (!composer_ || !loading_path_.empty()) return;
    const auto path = DraftPath();
    std::error_code error;
    spark::fs::create_directories(NativePath(ParentPath(path)), error);
    const nlohmann::json state = {{"draft", S(composer_->GetValue())}, {"attachments", attachments_},
                                 {"goal", goal_}, {"plan_mode", plan_}};
    WriteFileUtf8Atomic(path, state.dump(2));
    draft_dirty_ = false;
}
void NativeFrame::RestoreDraft() {
    attachments_.clear(); goal_.clear(); plan_ = false;
    const auto* view = Active();
    const auto mode = view ? view->journal.State().metadata.value("permission_mode", config_.permissions.default_mode) :
                             config_.permissions.default_mode;
    permissions_->SetSelection(mode == "full_access" ? 2 : mode == "approve_for_me" ? 1 : 0);
    std::string text;
    const auto contents = ReadFileUtf8(DraftPath());
    const auto state = contents ? nlohmann::json::parse(*contents, nullptr, false) : nlohmann::json{};
    if (state.is_object()) {
        text = state.value("draft", std::string{});
        goal_ = state.value("goal", std::string{});
        plan_ = state.value("plan_mode", false);
        if (state.contains("attachments") && state["attachments"].is_array()) for (const auto& value : state["attachments"])
            if (value.is_string() && attachments_.size() < 64) attachments_.push_back(value.get<std::string>());
    }
    refreshing_ = true;
    composer_->ChangeValue(U(text));
    refreshing_ = false;
    draft_dirty_ = false;
    RefreshAttachments();
}
}  // namespace spark
