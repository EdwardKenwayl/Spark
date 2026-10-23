#include "gui/app.h"
#include "gui/localization.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include "platform/filesystem.h"

namespace spark {
namespace {
constexpr size_t kNoItem = static_cast<size_t>(-1);

std::wstring SourceTitle(const SourceReference& source) {
    if (source.kind == "image" || source.kind == "file") {
        return spark::fs::path(Utf8ToWide(source.detail)).filename().wstring();
    }
    return Utf8ToWide(source.label.empty() ? source.kind : source.label);
}

LRESULT CALLBACK SummaryButtonProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                   UINT_PTR, DWORD_PTR) {
    if (message == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
        InvalidateRect(window, nullptr, FALSE);
    } else if (message == WM_MOUSELEAVE) InvalidateRect(window, nullptr, FALSE);
    return DefSubclassProc(window, message, wparam, lparam);
}
}  // namespace

void AppWindow::CreateSummaryControls() {
    SetWindowTextW(summary_button_, Text(L"Toggle summary"));
    SetWindowSubclass(summary_button_, SummaryButtonProc, 7, 0);
    summary_popup_ = std::make_unique<SummaryView>();
    summary_panel_ = std::make_unique<SummaryView>();
    if (!summary_popup_->Create(window_, instance_, ui_font_, true)) summary_popup_.reset();
    if (!summary_panel_->Create(window_, instance_, ui_font_, false)) summary_panel_.reset();
    for (auto* view : {summary_popup_.get(), summary_panel_.get()}) {
        if (view == nullptr) continue;
        view->SetActions([this](SummaryAction action, const std::string& id) { HandleSummaryAction(action, id); },
                         [this](const std::string& path) { return LoadAttachmentBitmap(path); });
        view->SetAppearance(config_.ui.theme);
    }
    if (summary_panel_ != nullptr && side_chat_view_ != nullptr) {
        SetParent(side_chat_view_->hwnd(), summary_panel_->hwnd());
    }
    summary_tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                      WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                      CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                      window_, nullptr, instance_, nullptr);
    if (summary_tooltip_ != nullptr) {
        TOOLINFOW info{sizeof(info)};
        info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        info.hwnd = window_;
        info.uId = reinterpret_cast<UINT_PTR>(summary_button_);
        info.lpszText = const_cast<wchar_t*>(Text(L"Toggle summary"));
        SendMessageW(summary_tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    }
}

std::vector<SummaryRow> AppWindow::SummaryRows(bool popup) const {
    std::vector<SummaryRow> rows;
    const std::string parent = pending_new_conversation_ ? std::string{} : NormalizePathForComparison(journal_.Path());
    std::vector<const SubAgentInfo*> running;
    std::vector<const SubAgentInfo*> finished;
    for (const auto& entry : subagents_ui_) {
        const auto& info = entry.second.info;
        if (parent.empty() || NormalizePathForComparison(info.parent_session_key) != parent) continue;
        (info.state == SubAgentState::Running ? running : finished).push_back(&info);
    }
    const auto sort = [](const SubAgentInfo* left, const SubAgentInfo* right) {
        return left->started_at != right->started_at ? left->started_at > right->started_at : left->id < right->id;
    };
    std::sort(running.begin(), running.end(), sort);
    std::sort(finished.begin(), finished.end(), sort);
    if (popup) {
        std::vector<const SubAgentInfo*> children = running;
        children.insert(children.end(), finished.begin(), finished.end());
        if (!children.empty()) {
            const bool grouped = children.size() > 4;
            SummaryRow section{SummaryRowKind::Section, Text(L"Subagents")};
            if (grouped) {
                section.action = SummaryAction::ToggleAgents;
                section.expanded = summary_agents_expanded_;
            }
            rows.push_back(std::move(section));
            if (!grouped || summary_agents_expanded_) {
                for (const auto* info : children) {
                    rows.push_back({SummaryRowKind::Agent, Utf8ToWide(info->name), {}, {}, info->id, {},
                                    info->state == SubAgentState::Running ? info->started_at : info->finished_at,
                                    info->state == SubAgentState::Running, SummaryAction::OpenAgent});
                }
            } else {
                std::wstring count;
                const auto add_count = [&](size_t size, std::wstring_view msgid) {
                    if (size == 0) return;
                    if (!count.empty()) count += L" · ";
                    count += i18n::Format(msgid, {{L"count", std::to_wstring(size)}});
                };
                add_count(running.size(), L"{count} running");
                add_count(static_cast<size_t>(std::count_if(finished.begin(), finished.end(),
                    [](const auto* info) { return info->state == SubAgentState::Completed; })), L"{count} completed");
                add_count(static_cast<size_t>(std::count_if(finished.begin(), finished.end(),
                    [](const auto* info) { return info->state == SubAgentState::Failed; })), L"{count} failed");
                add_count(static_cast<size_t>(std::count_if(finished.begin(), finished.end(),
                    [](const auto* info) { return info->state == SubAgentState::Cancelled; })), L"{count} stopped");
                SummaryRow group{SummaryRowKind::AgentGroup, count, {}, {}, {}, {}, 0, false, SummaryAction::ToggleAgents};
                for (size_t index = 0; index < std::min<size_t>(4, children.size()); ++index) {
                    group.agent_ids.push_back(children[index]->id);
                }
                rows.push_back(std::move(group));
            }
            rows.push_back({SummaryRowKind::Divider});
        }
        rows.push_back({SummaryRowKind::Section, Text(L"Sources")});
        const auto sources = pending_new_conversation_ ? std::vector<SourceReference>{} : CollectSummarySources(journal_.State());
        bool web = false;
        size_t images = 0;
        size_t other = 0;
        for (const auto& source : sources) {
            if (source.kind.rfind("web", 0) == 0 || source.kind == "url_citation") web = true;
            else if (source.kind == "image") ++images;
            else ++other;
        }
        if (web) rows.push_back({SummaryRowKind::Link, std::wstring(L"\u25ce  ") + Text(L"Web search"), {}, {}, {}, {}, 0, false, SummaryAction::Sources});
        if (images > 0) rows.push_back({SummaryRowKind::Link, i18n::Format(L"{count} images", {{L"count", std::to_wstring(images)}}), {}, {}, {}, {}, 0, false, SummaryAction::Sources});
        if (other > 0) rows.push_back({SummaryRowKind::Link, i18n::Format(L"{count} tool sources", {{L"count", std::to_wstring(other)}}), {}, {}, {}, {}, 0, false, SummaryAction::Sources});
        rows.push_back({SummaryRowKind::Link, Text(L"View all"), {}, {}, {}, {}, 0, false, SummaryAction::Sources});
        return rows;
    }
    if (summary_tab_ == SummaryTab::Sources) {
        const auto sources = pending_new_conversation_ ? std::vector<SourceReference>{} : CollectSummarySources(journal_.State());
        for (const auto& source : sources) {
            const wchar_t* note = source.kind == "image" || source.kind == "file"
                                      ? Text(L"Attached to conversation")
                                      : source.kind == "mcp" ? Text(L"MCP tool called")
                                      : source.kind == "web" || source.kind == "web_source" || source.kind == "url_citation"
                                            ? Text(L"Web source")
                                      : source.kind == "web_open" ? Text(L"Page opened")
                                      : source.kind == "web_find" ? Text(L"Page content searched")
                                      : source.kind == "web_search" ? Text(L"Web search performed")
                                      : Text(L"Tool used");
            rows.push_back({SummaryRowKind::Source, SourceTitle(source), Utf8ToWide(source.detail),
                            note, source.detail, source.kind, 0, false, SummaryAction::OpenSource});
        }
        if (rows.empty()) rows.push_back({SummaryRowKind::Empty, Text(L"No sources yet")});
        return rows;
    }
    rows.push_back({SummaryRowKind::Section, i18n::Format(L"Active · {count}", {{L"count", std::to_wstring(running.size())}})});
    for (const auto* info : running) {
        rows.push_back({SummaryRowKind::Agent, Utf8ToWide(info->name), {}, {}, info->id, {}, info->started_at, true, SummaryAction::OpenAgent});
    }
    if (running.empty()) rows.push_back({SummaryRowKind::Empty, Text(L"No active subagents")});
    rows.push_back({SummaryRowKind::Section, i18n::Format(L"Finished · {count}", {{L"count", std::to_wstring(finished.size())}})});
    const size_t shown = summary_show_all_ ? finished.size() : std::min<size_t>(10, finished.size());
    for (size_t index = 0; index < shown; ++index) {
        const auto* info = finished[index];
        std::wstring name = Utf8ToWide(info->name);
        if (info->state == SubAgentState::Failed) name = i18n::Format(L"{name} · Failed", {{L"name", name}});
        else if (info->state == SubAgentState::Cancelled) name = i18n::Format(L"{name} · Stopped", {{L"name", name}});
        rows.push_back({SummaryRowKind::Agent, name, {}, {}, info->id, {}, info->finished_at, false, SummaryAction::OpenAgent});
    }
    if (shown < finished.size()) {
        rows.push_back({SummaryRowKind::Link, i18n::Format(L"Show {count} more",
                       {{L"count", std::to_wstring(finished.size() - shown)}}),
                       {}, {}, {}, {}, 0, false, SummaryAction::More});
    }
    if (finished.empty()) rows.push_back({SummaryRowKind::Empty, Text(L"No finished subagents")});
    return rows;
}

void AppWindow::ShowSummaryMenu(POINT anchor) {
    if (summary_popup_ == nullptr) return;
    if (IsWindowVisible(summary_popup_->hwnd())) { summary_popup_->Hide(); return; }
    summary_agents_expanded_ = false;
    RefreshSummaryPanel(true);
    summary_popup_->SetContent(summary_tab_, SummaryRows(true));
    summary_popup_->ShowPopup(anchor);
}

void AppWindow::ShowSubAgentMenu(POINT) { OpenSummaryPanel(SummaryTab::Agents); }
void AppWindow::RefreshSubAgentButton() { ShowWindow(subagent_button_, SW_HIDE); RefreshSummaryPanel(); }

void AppWindow::RefreshSummaryPanel(bool load_history) {
    if (load_history && !pending_new_conversation_ && !journal_.Path().empty()) {
        const auto current = subagents_ != nullptr ? subagents_->List(journal_.Path()) : std::vector<SubAgentInfo>{};
        for (auto& info : SubAgentManager::LoadHistory(DataDirectory(), journal_.Path())) {
            if (info.state == SubAgentState::Running &&
                std::none_of(current.begin(), current.end(), [&](const auto& live) { return live.id == info.id; })) {
                info.state = SubAgentState::Cancelled;
                info.started_tick_ms = 0;
                info.error = "The previous application process stopped before this child completed.";
            }
            if (subagents_ui_.find(info.id) == subagents_ui_.end()) subagents_ui_[info.id].info = std::move(info);
        }
    }
    SetWindowTextW(summary_button_, Text(L"Toggle summary"));
    if (summary_tooltip_ != nullptr) {
        TOOLINFOW info{sizeof(info)};
        info.hwnd = window_;
        info.uId = reinterpret_cast<UINT_PTR>(summary_button_);
        info.lpszText = const_cast<wchar_t*>(Text(L"Toggle summary"));
        SendMessageW(summary_tooltip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&info));
    }
    if (summary_panel_ == nullptr) return;
    summary_panel_->SetAppearance(config_.ui.theme);
    if (!active_subagent_id_.empty()) {
        const auto child = subagents_ui_.find(active_subagent_id_);
        if (child != subagents_ui_.end()) {
            const auto& info = child->second.info;
            std::wstring model = Utf8ToWide(info.model);
            if (!info.provider.empty()) {
                model = i18n::Format(L"{provider} / {model}",
                                     {{L"provider", Utf8ToWide(info.provider)}, {L"model", model}});
            }
            std::wstring effort = Utf8ToWide(info.reasoning_effort);
            if (info.reasoning_effort == "none") effort = Text(L"None");
            else if (info.reasoning_effort == "minimal") effort = Text(L"Minimal");
            else if (info.reasoning_effort == "low") effort = Text(L"Low");
            else if (info.reasoning_effort == "medium") effort = Text(L"Medium");
            else if (info.reasoning_effort == "high") effort = Text(L"High");
            else if (info.reasoning_effort == "xhigh") effort = Text(L"Extra high");
            else if (info.reasoning_effort == "max") effort = Text(L"Maximum");
            else if (info.reasoning_effort == "ultra") effort = Text(L"Ultra");
            if (!effort.empty()) {
                model = i18n::Format(L"{model} · {effort}", {{L"model", model}, {L"effort", effort}});
            }
            summary_panel_->SetContent(SummaryTab::Agents, {}, Utf8ToWide(info.name), model, true, info.id);
        }
    } else summary_panel_->SetContent(summary_tab_, SummaryRows(false));
    if (summary_popup_ != nullptr) {
        summary_popup_->SetAppearance(config_.ui.theme);
        if (IsWindowVisible(summary_popup_->hwnd())) summary_popup_->SetContent(summary_tab_, SummaryRows(true));
    }
}

void AppWindow::OpenSummaryPanel(SummaryTab tab) {
    summary_tab_ = tab;
    active_subagent_id_.clear();
    side_chat_open_ = true;
    RefreshSummaryPanel(true);
    LayoutControls();
}

void AppWindow::HandleSummaryAction(SummaryAction action, const std::string& id) {
    switch (action) {
        case SummaryAction::Agents: OpenSummaryPanel(SummaryTab::Agents); break;
        case SummaryAction::Sources: OpenSummaryPanel(SummaryTab::Sources); break;
        case SummaryAction::OpenAgent: OpenSubAgentChat(id); break;
        case SummaryAction::Back: OpenSummaryPanel(SummaryTab::Agents); break;
        case SummaryAction::Close: CloseSubAgentChat(); break;
        case SummaryAction::More: summary_show_all_ = true; RefreshSummaryPanel(); break;
        case SummaryAction::ToggleAgents:
            summary_agents_expanded_ = !summary_agents_expanded_;
            if (summary_popup_ != nullptr) summary_popup_->SetContent(summary_tab_, SummaryRows(true));
            break;
        case SummaryAction::OpenSource:
            for (const auto& source : CollectSummarySources(journal_.State())) {
                if (source.detail != id) continue;
                if (source.kind == "image" || source.kind == "file") {
                    if (PathExists(id)) OpenAttachment(id);
                } else if ((source.kind.rfind("web", 0) == 0 || source.kind == "url_citation") &&
                           (id.rfind("https://", 0) == 0 || id.rfind("http://", 0) == 0)) {
                    ShellExecuteW(window_, L"open", Utf8ToWide(id).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                break;
            }
            break;
    }
}

void AppWindow::OpenSubAgentChat(const std::string& id) {
    const auto found = subagents_ui_.find(id);
    if (found == subagents_ui_.end() || side_chat_view_ == nullptr || pending_new_conversation_ ||
        NormalizePathForComparison(found->second.info.parent_session_key) != NormalizePathForComparison(journal_.Path())) return;
    active_subagent_id_ = id;
    summary_tab_ = SummaryTab::Agents;
    side_chat_open_ = true;
    RefreshSummaryPanel();
    RefreshSubAgentChat();
    LayoutControls();
}

void AppWindow::CloseSubAgentChat() {
    side_chat_open_ = false;
    active_subagent_id_.clear();
    side_assistant_entry_ = side_turn_entry_ = side_reasoning_entry_ = kNoItem;
    side_tool_entries_.clear();
    if (summary_panel_ != nullptr) summary_panel_->Hide();
    if (summary_popup_ != nullptr) summary_popup_->Hide();
    if (side_chat_view_ != nullptr) ShowWindow(side_chat_view_->hwnd(), SW_HIDE);
    if (window_ != nullptr) LayoutControls();
}

void AppWindow::RefreshSubAgentChat() {
    if (!side_chat_open_ || active_subagent_id_.empty() || side_chat_view_ == nullptr) return;
    const auto found = subagents_ui_.find(active_subagent_id_);
    if (found == subagents_ui_.end()) return;
    const SubAgentUi& child = found->second;
    const SubAgentInfo& info = child.info;
    const bool running = info.state == SubAgentState::Running;
    side_chat_view_->Clear();
    side_tool_entries_.clear();
    side_assistant_entry_ = side_turn_entry_ = side_reasoning_entry_ = kNoItem;
    SessionJournal journal;
    const bool loaded = !info.session_path.empty() && journal.Resume(info.session_path).ok;
    if (loaded) {
        const auto& state = journal.State();
        const auto durations = state.metadata.value("tool_durations_ms", nlohmann::json::object());
        for (const ChatMessage& message : state.messages) {
            if (message.role == MessageRole::User) {
                side_chat_view_->AddUser(message.content, message.timestamp);
                side_turn_entry_ = side_chat_view_->AddTurnStatus(0, running ? info.started_tick_ms : 0);
            } else if (message.role == MessageRole::Assistant) {
                if (!message.content.empty()) {
                    const size_t index = side_chat_view_->AddAssistant(message.content, message.timestamp);
                    side_chat_view_->FinishAssistant(index, message.timestamp);
                }
                for (const auto& call : message.tool_calls) {
                    side_tool_entries_[call.id] = side_chat_view_->AddTool(call.name, call.arguments_json, message.timestamp);
                }
            } else if (message.role == MessageRole::Tool) {
                const auto pending = side_tool_entries_.find(message.tool_call_id);
                const size_t index = pending != side_tool_entries_.end()
                                         ? pending->second : side_chat_view_->AddTool(message.name, "{}", message.timestamp);
                side_chat_view_->FinishTool(index, true, message.content, message.timestamp,
                                           durations.value(message.tool_call_id, uint64_t{0}));
                side_tool_entries_.erase(message.tool_call_id);
            }
        }
        if (!running) {
            const auto usages = state.metadata.value("turn_usages", nlohmann::json::array());
            if (!usages.empty() && usages.back().is_object()) {
                const auto& usage = usages.back();
                side_chat_view_->AddUsageStats(usage.value("input_tokens", uint64_t{0}),
                    usage.value("output_tokens", uint64_t{0}), usage.value("cached_input_tokens", uint64_t{0}),
                    usage.value("model_calls", uint64_t{0}), usage.value("context_tokens", uint64_t{0}),
                    usage.value("context_limit_tokens", uint64_t{0}), info.elapsed_ms, info.finished_at);
            }
        }
    } else {
        side_chat_view_->AddUser(info.task, info.started_at);
        side_turn_entry_ = side_chat_view_->AddTurnStatus(0, running ? info.started_tick_ms : 0);
        if (!running && !info.output.empty()) {
            const size_t index = side_chat_view_->AddAssistant(info.output, info.finished_at);
            side_chat_view_->FinishAssistant(index, info.finished_at);
        }
    }
    if (side_turn_entry_ == kNoItem) {
        side_chat_view_->AddUser(info.task, info.started_at);
        side_turn_entry_ = side_chat_view_->AddTurnStatus(0, running ? info.started_tick_ms : 0);
    }
    if (running) {
        if (!child.live_text.empty()) side_assistant_entry_ = side_chat_view_->AddAssistant(child.live_text);
        if (child.live_text.empty() && !child.reasoning.empty()) {
            side_reasoning_entry_ = side_chat_view_->AddReasoning(child.reasoning);
        }
        side_chat_view_->SetActivityState(child.live_text.empty() && child.reasoning.empty() && side_tool_entries_.empty(),
                                         child.phase, info.started_tick_ms);
        BeginActivityAnimation();
    } else {
        for (const auto& tool : side_tool_entries_) {
            side_chat_view_->FinishTool(tool.second, false, WideToUtf8(Text(L"Stopped before completion.")));
        }
        side_tool_entries_.clear();
        if (side_turn_entry_ != kNoItem) side_chat_view_->FinishTurnStatus(side_turn_entry_, info.elapsed_ms, info.finished_at);
        if (info.state == SubAgentState::Failed && !info.error.empty()) {
            if (IsQuotaExhaustedError(info.error)) side_chat_view_->AddQuotaNotice(info.finished_at);
            else side_chat_view_->AddError(info.error, info.finished_at);
        }
        side_chat_view_->SetActivityState(false, {});
    }
}

bool AppWindow::HandleSubAgentEvent(UiEvent* event) {
    if (event->subagent_id.empty()) return false;
    auto& child = subagents_ui_[event->subagent_id];
    child.info.id = event->subagent_id;
    child.info.parent_session_key = event->session_key;
    const bool visible = side_chat_open_ && active_subagent_id_ == event->subagent_id && side_chat_view_ != nullptr;
    const auto hide_reasoning = [&] {
        if (visible) {
            if (side_reasoning_entry_ != kNoItem) side_chat_view_->HideReasoning(side_reasoning_entry_);
            side_reasoning_entry_ = kNoItem;
        }
        child.reasoning.clear();
    };
    switch (event->kind) {
        case EventKind::SubAgentCreated:
            child.info = std::move(event->child_info);
            BeginActivityAnimation();
            RefreshSummaryPanel();
            break;
        case EventKind::SubAgentPhase:
            child.phase = Utf8ToWide(event->first);
            if (event->phase == AgentPhase::WaitingModel) {
                child.live_text.clear();
                if (visible && side_assistant_entry_ != kNoItem) {
                    side_chat_view_->FinishAssistant(side_assistant_entry_);
                    side_assistant_entry_ = kNoItem;
                }
            }
            if (event->phase == AgentPhase::Streaming || event->phase == AgentPhase::RunningTool) hide_reasoning();
            if (visible) {
                side_chat_view_->SetActivityState(event->phase == AgentPhase::WaitingModel && child.reasoning.empty(),
                                                  child.phase, child.info.started_tick_ms);
            }
            break;
        case EventKind::SubAgentReasoning:
            child.reasoning = TruncateUtf8(child.reasoning + event->first, 16384);
            if (visible) {
                side_chat_view_->SetActivityState(false, {});
                if (side_reasoning_entry_ == kNoItem) side_reasoning_entry_ = side_chat_view_->AddReasoning();
                side_chat_view_->AppendReasoning(side_reasoning_entry_, event->first);
            }
            break;
        case EventKind::SubAgentText:
            hide_reasoning();
            child.live_text = TruncateUtf8(child.live_text + event->first, 48000);
            if (visible) {
                side_chat_view_->SetActivityState(false, {});
                if (side_assistant_entry_ == kNoItem) side_assistant_entry_ = side_chat_view_->AddAssistant();
                side_chat_view_->AppendAssistant(side_assistant_entry_, event->first);
            }
            break;
        case EventKind::SubAgentToolStart:
            hide_reasoning();
            child.live_text.clear();
            if (visible) {
                if (side_assistant_entry_ != kNoItem) side_chat_view_->FinishAssistant(side_assistant_entry_);
                side_assistant_entry_ = kNoItem;
                side_chat_view_->SetActivityState(false, {});
                if (side_tool_entries_.find(event->first) == side_tool_entries_.end()) {
                    side_tool_entries_[event->first] = side_chat_view_->AddTool(event->second, event->third);
                }
            }
            break;
        case EventKind::SubAgentToolFinish:
            if (visible) {
                const auto tool = side_tool_entries_.find(event->first);
                if (tool != side_tool_entries_.end()) {
                    side_chat_view_->FinishTool(tool->second, event->success, event->third);
                    side_tool_entries_.erase(tool);
                }
            }
            break;
        case EventKind::SubAgentError:
            child.info.error = event->first;
            if (visible) {
                if (IsQuotaExhaustedError(event->first)) side_chat_view_->AddQuotaNotice();
                else side_chat_view_->AddError(event->first);
            }
            break;
        case EventKind::SubAgentFinished:
            child.info = std::move(event->child_info);
            child.live_text.clear();
            child.reasoning.clear();
            if (visible) RefreshSubAgentChat();
            RefreshSummaryPanel();
            if (!AnyTurnRunning() && mcp_reload_pending_) {
                ReloadMcpWhenIdle();
            }
            UpdateTurnUi();
            break;
        default: return false;
    }
    return true;
}
}  // namespace spark
