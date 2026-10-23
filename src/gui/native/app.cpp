#include "gui/native/app.h"
#include "core/context_compactor.h"
#include "gui/localization.h"
#include "gui/native/startup_dpi.h"
#include "gui/native/summary_popup.h"
#include "platform/clock.h"
#include "platform/desktop_capture.h"

#include <wx/artprov.h>
#include <wx/bmpbuttn.h>
#include <wx/dcbuffer.h>
#include <wx/filefn.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stdpaths.h>
#include <wx/settings.h>
#include <wx/weakref.h>
#include <wx/wrapsizer.h>
#include <algorithm>

namespace spark {
namespace {
wxString U(const std::string& text) { return wxString::FromUTF8(text.data(), text.size()); }
wxString T(const wchar_t* text) { return wxString(i18n::Text(text)); }
std::string S(const wxString& text) { return text.ToUTF8().data(); }
NativeButton* Icon(wxWindow* parent, const wxArtID& art, const wchar_t* label) {
    auto* button = new NativeButton(parent, {}, art);
    button->SetToolTip(T(label));
    return button;
}
}  // namespace

class NativeFrame::Tray : public wxTaskBarIcon {
public:
    explicit Tray(NativeFrame* owner) : owner_(owner) {
        wxIcon icon = owner->GetIcon();
        if (!icon.IsOk()) icon = wxArtProvider::GetIcon(wxART_EXECUTABLE_FILE, wxART_FRAME_ICON, wxSize(32, 32));
        SetIcon(icon, "Spark");
        Bind(wxEVT_TASKBAR_LEFT_UP, [this](wxTaskBarIconEvent&) { owner_->RestoreWindow(); });
        Bind(wxEVT_MENU, [this](wxCommandEvent& event) {
            if (event.GetId() == 1) owner_->OpenSettings();
            else if (event.GetId() == 2) { owner_->quitting_ = true; owner_->Close(); }
        });
    }
    wxMenu* CreatePopupMenu() override {
        auto* menu = new wxMenu;
        menu->Append(1, T(L"Settings"));
        menu->AppendSeparator();
        menu->Append(2, T(L"Quit Spark"));
        return menu;
    }
private:
    NativeFrame* owner_;
};

NativeFrame::NativeFrame(AppConfig config)
    : wxFrame(nullptr, wxID_ANY, "Spark"),
      config_(std::move(config)), timer_(this) {
    ApplyNativeStartupDpi(*this);
    for (const auto* relative : {"../share/spark/spark_agent.ico", "../Resources/spark_agent.ico"}) {
        const auto path = JoinPath(GetExecutableDirectory(), relative);
        wxIcon icon;
        if (PathExists(path) && icon.LoadFile(U(path), wxBITMAP_TYPE_ICO)) { SetIcon(icon); break; }
    }
    i18n::SetLocale(config_.ui.language);
    root_ = new wxBoxSizer(wxVERTICAL);
    SetSizer(root_);
    CreateChat();
    LoadProjects();
    NewConversation();
    ApplyConfig(config_);
    RefreshSidebar();
    tray_ = std::make_unique<Tray>(this);
    Bind(wxEVT_TIMER, [this, last_summary_refresh = uint64_t{0}](wxTimerEvent& event) mutable {
        const wxWeakRef<NativeFrame> owner(this);
        Tick(event);
        if (!owner || owner->IsBeingDeleted()) return;
        const auto now = MonotonicMillis();
        if (!close_pending_ && NativeSummaryPopupShown(this) && now - last_summary_refresh >= 1000) {
            last_summary_refresh = now;
            RefreshSummary();
        }
    });
    Bind(wxEVT_CLOSE_WINDOW, &NativeFrame::OnClose, this);
    Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& event) {
        const auto windows = SnapshotWindows();
        snapshot_target_id_ = windows.empty() ? 0 : windows.front().id;
        event.Skip();
    });
    Bind(wxEVT_ICONIZE, [this](wxIconizeEvent& event) {
        if (event.IsIconized() && tray_ && wxTaskBarIcon::IsAvailable()) Hide();
        else event.Skip();
    });
    timer_.Start(50);
}
NativeFrame::~NativeFrame() { timer_.Stop(); loader_.Stop(); side_loader_.Stop(); history_loader_.Stop(); runtime_.Stop(); }

void NativeFrame::CreateChat() {
    shell_ = new wxPanel(this);
    shell_->SetName("spark.chat");
    root_->Add(shell_, 1, wxEXPAND);
    auto* columns = new wxBoxSizer(wxHORIZONTAL);
    shell_->SetSizer(columns);
    sidebar_ = new wxPanel(shell_);
    sidebar_->SetName("spark.sidebar");
    sidebar_->SetMinSize(FromDIP(wxSize(236, -1)));
    CreateSidebar();
    columns->Add(sidebar_, 0, wxEXPAND);
    auto* divider = new wxPanel(shell_);
    divider->SetName("spark.sidebar.divider");
    divider->SetMinSize(FromDIP(wxSize(1, -1)));
    columns->Add(divider, 0, wxEXPAND);
    chat_page_ = new wxPanel(shell_);
    chat_sizer_ = new wxBoxSizer(wxVERTICAL);
    chat_page_->SetSizer(chat_sizer_);
    header_panel_ = new wxPanel(chat_page_);
    header_panel_->SetMinSize(FromDIP(wxSize(-1, 46)));
    header_panel_->SetBackgroundStyle(wxBG_STYLE_PAINT);
    header_panel_->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(header_panel_);
        const auto palette = NativeColors(header_panel_->GetForegroundColour().Red() > 180);
        dc.SetBackground(wxBrush(palette.background)); dc.Clear();
        dc.SetPen(wxPen(palette.border));
        const auto size = header_panel_->GetClientSize();
        dc.DrawLine(0, size.y - 1, size.x, size.y - 1);
    });
    header_panel_->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { LayoutHeader(); event.Skip(); });
    title_ = new wxStaticText(header_panel_, wxID_ANY, T(L"New chat"), wxDefaultPosition,
                              wxDefaultSize, wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    context_ = new wxStaticText(header_panel_, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                wxALIGN_RIGHT | wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    context_->SetName("spark.context");
    summary_ = Icon(header_panel_, wxART_INFORMATION, L"Toggle summary");
    summary_->SetName("spark.summary");
    tooltips_.push_back({summary_, L"Toggle summary"});
    summary_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Summary(); });
    chat_sizer_->Add(header_panel_, 0, wxEXPAND);
    chat_ = new NativeChatSurface(chat_page_);
    chat_->SetName("spark.transcript");
    chat_->edit = [this](size_t row) { EditMessage(row); };
    auto* transcript_row = new wxBoxSizer(wxHORIZONTAL);
    transcript_row->AddSpacer(FromDIP(16));
    transcript_row->Add(chat_, 1, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(8));
    transcript_row->AddSpacer(FromDIP(16));
    chat_sizer_->Add(transcript_row, 1, wxEXPAND);
    composer_host_ = new wxPanel(chat_page_);
    auto* centered = new wxBoxSizer(wxHORIZONTAL);
    centered->AddStretchSpacer();
    input_panel_ = new NativeComposer(composer_host_);
    input_panel_->SetName("spark.input");
    centered->Add(input_panel_, 0, wxEXPAND);
    centered->AddStretchSpacer();
    composer_host_->SetSizer(centered);
    chat_sizer_->Add(composer_host_, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
    attachment_panel_ = new NativeAttachmentStrip(input_panel_);
    attachment_panel_->preview = [this](const std::string& path) { PreviewAttachment(path); };
    attachment_panel_->remove = [this](const std::string& path) {
        attachments_.erase(std::remove(attachments_.begin(), attachments_.end(), path), attachments_.end());
        draft_dirty_ = true; RefreshAttachments(); UpdateControls();
    };
    composer_ = new wxTextCtrl(input_panel_, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(-1, 80)),
                                wxTE_MULTILINE | wxTE_NO_VSCROLL | wxBORDER_NONE);
    composer_->SetName("spark.composer");
    composer_hint_ = new wxStaticText(input_panel_, wxID_ANY, T(L"Message Spark"), wxDefaultPosition,
        wxDefaultSize, wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    composer_hint_->SetName("spark.composer.placeholder");
    composer_hint_->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { composer_->SetFocus(); });
    composer_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { if (!refreshing_) draft_dirty_ = true; UpdateControls(); });
    composer_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_ESCAPE && Pause()) return;
        if (event.GetKeyCode() == 'V' && event.CmdDown() && PasteAttachments()) return;
        if (event.GetKeyCode() == WXK_RETURN && !event.ShiftDown() &&
            ((config_.ui.send_key == "ctrl+enter") == event.CmdDown())) { Send(); return; }
        event.Skip();
    });
    attach_ = Icon(input_panel_, wxART_PLUS, L"Attach");
    attach_->SetName("spark.attach");
    tooltips_.push_back({attach_, L"Attach"});
    attach_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AddAttachments(); });
    permissions_ = new NativeChoice(input_panel_);
    permissions_->SetMinSize(FromDIP(wxSize(132, 32)));
    permissions_->SetName("spark.permissions");
    for (const auto* label : {L"Ask for approval", L"Approve for me", L"Full access"}) permissions_->Append(T(label));
    permissions_->SetSelection(config_.permissions.default_mode == "full_access" ? 2 :
                                config_.permissions.default_mode == "approve_for_me" ? 1 : 0);
    permissions_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        ApplyPermissionMode(&config_, permissions_->GetSelection() == 2 ? "full_access" :
                                       permissions_->GetSelection() == 1 ? "approve_for_me" : "ask");
        const auto saved = SaveConfigAtomically(config_);
        if (!saved.ok) Error(saved.error);
        if (auto* view = Active(); view && !runtime_.Running(active_) && !external_.count(active_)) {
            view->journal.Append("session.permission", {{"mode", config_.permissions.default_mode}});
            view->journal.SaveSnapshot();
        }
    });
    model_ = new NativeButton(input_panel_);
    model_->SetName("spark.model");
    model_->SetMinSize(FromDIP(wxSize(100, 32)));
    model_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ModelMenu(); });
    send_ = Icon(input_panel_, wxART_GO_FORWARD, L"Send");
    send_->SetName("spark.send");
    send_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Send(); });
    chat_page_->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        LayoutComposer();
        event.Skip();
    });
    columns->Add(chat_page_, 1, wxEXPAND);
    NativeSummaryPanel::Callbacks callbacks;
    callbacks.close = [this] { ResetSideConversation(); LayoutShell(); };
    callbacks.active_child = [this](const std::string& id) { SelectChild(id); };
    callbacks.thumbnail = [this](const SourceReference& source, int edge) {
        return summary_thumbnails_.Get(source.detail, edge);
    };
    callbacks.open_source = [this](const SourceReference& source) {
        if (source.kind == "image" || source.kind == "input_image") PreviewAttachment(source.detail);
        else if (source.kind.rfind("web", 0) == 0 || source.kind == "url_citation") {
            const auto& url = source.detail;
            if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0)
                wxLaunchDefaultBrowser(U(url));
        } else if (source.kind == "file" && PathExists(source.detail)) wxLaunchDefaultApplication(U(source.detail));
    };
    side_panel_ = new NativeSummaryPanel(shell_, std::move(callbacks));
    side_chat_ = side_panel_->ChildChatSurface();
    columns->Add(side_panel_, 0, wxEXPAND);
    side_panel_->Hide();
    shell_->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { LayoutShell(); event.Skip(); });
}

NativeFrame::View* NativeFrame::Active() {
    const auto found = views_.find(active_);
    return found == views_.end() ? nullptr : found->second.get();
}
const ModelConfig* NativeFrame::SelectedModel() const {
    const auto* provider = FindProvider(config_, config_.active.provider);
    return provider ? FindModel(*provider, config_.active.model) : nullptr;
}
void NativeFrame::ApplyConfig(const AppConfig& config) {
    config_ = config;
    i18n::SetLocale(config_.ui.language);
    const bool dark = config_.ui.theme == "dark" ||
        (config_.ui.theme != "light" && wxSystemSettings::GetAppearance().IsDark());
    const auto palette = NativeColors(dark);
    ApplyNativeColors(shell_, dark);
    sidebar_->SetBackgroundColour(palette.sidebar);
    if (auto* divider = wxWindow::FindWindowByName("spark.sidebar.divider", shell_))
        divider->SetBackgroundColour(palette.border);
    for (auto* child : sidebar_->GetChildren()) child->SetBackgroundColour(palette.sidebar);
    history_->SetBackgroundColour(sidebar_->GetBackgroundColour());
    project_list_->SetBackgroundColour(sidebar_->GetBackgroundColour());
    input_panel_->SetBackgroundColour(palette.panel);
    for (auto* child : input_panel_->GetChildren()) child->SetBackgroundColour(palette.panel);
    composer_->SetBackgroundColour(palette.panel);
    composer_hint_->SetBackgroundColour(palette.panel);
    composer_hint_->SetForegroundColour(palette.muted);
    composer_hint_->SetLabel(T(L"Message Spark"));
    chat_->SetDark(dark);
    side_panel_->SetDark(dark);
    for (const auto& label : labels_) label.first->SetLabel(T(label.second.c_str()));
    for (const auto& label : tooltips_) label.first->SetToolTip(T(label.second.c_str()));
    const int permission = permissions_->GetSelection();
    permissions_->Clear();
    for (const auto* label : {L"Ask for approval", L"Approve for me", L"Full access"})
        permissions_->Append(T(label));
    permissions_->SetSelection(permission);
    if (active_.empty()) {
        title_->SetLabel(T(L"New chat"));
        chat_->SetEmptyTitle(project_name_.empty() ? T(L"What do you want to do?") :
            wxString(i18n::Format(L"What would you like to do in {project}?",
                                 {{L"project", Utf8ToWide(project_name_)}})));
    }
    RefreshModels();
    RefreshSidebar();
    UpdateControls();
    LayoutShell();
    Refresh(false);
}
void NativeFrame::OpenSettings() {
    RestoreWindow();
    SaveDraft();
    shell_->Hide();
    if (!settings_) {
        settings_ = new NativeSettings(this, config_, [this](const AppConfig& config) { ApplyConfig(config); },
                                       [this] { settings_->Hide(); shell_->Show(); Layout(); });
        settings_->SetName("spark.settings");
        root_->Add(settings_, 1, wxEXPAND);
    } else settings_->Reload(config_);
    settings_->Show();
    Layout();
}
void NativeFrame::RestoreWindow() { Show(); if (IsIconized()) Iconize(false); Raise(); }
void NativeFrame::Error(const std::string& message) { wxMessageBox(U(message), "Spark", wxOK | wxICON_ERROR, this); }
void NativeFrame::UpdateControls() {
    const bool running = runtime_.Running(active_) || external_.count(active_) != 0;
    const auto remote = external_.find(active_);
    if (remote != external_.end()) {
        const auto mode = remote->second.value("permission_mode", std::string("ask"));
        permissions_->SetSelection(mode == "full_access" ? 2 : mode == "approve_for_me" ? 1 : 0);
    }
    permissions_->Enable(remote == external_.end());
    const bool paused = Active() && Active()->journal.State().metadata.contains("paused_turn");
    send_->SetArt(running ? wxART_CROSS_MARK : paused && composer_->IsEmpty() ?
        wxArtID("spark.play") : wxART_GO_FORWARD);
    send_->SetToolTip(running ? T(L"Pause") : paused && composer_->IsEmpty() ? T(L"Resume") : T(L"Send"));
    send_->Enable(loading_path_.empty() && (running || paused || !composer_->IsEmpty() || !attachments_.empty()));
    composer_->Enable(loading_path_.empty());
    composer_hint_->Show(composer_->IsEmpty());
    const auto* model = SelectedModel();
    const uint64_t limit = model ? EffectiveContextLimit(config_.context, *model) :
        static_cast<uint64_t>(config_.context.fallback_context_window);
    const uint64_t used = Active() ? Active()->context : 0;
    const auto tokens = [](uint64_t value) {
        return wxString::FromUTF8(std::to_string(value < 1000 ? value : value / 1000 + (value % 1000 >= 500)) +
                                 (value < 1000 ? "" : "K"));
    };
    context_->SetLabel(T(L"Context ") + tokens(used) + " / " + tokens(limit));
    LayoutHeader();
}
void NativeFrame::OnClose(wxCloseEvent& event) {
    if (!quitting_ && event.CanVeto() && tray_ && wxTaskBarIcon::IsAvailable()) { Hide(); event.Veto(); return; }
    if (event.CanVeto() && !summary_thumbnails_.Drain()) {
        close_pending_ = true;
        Hide();
        event.Veto();
        return;
    }
    SaveDraft();
    timer_.Stop();
    loader_.Stop();
    side_loader_.Stop();
    history_loader_.Stop();
    runtime_.Stop();
    tray_.reset();
    Destroy();
}
}  // namespace spark
