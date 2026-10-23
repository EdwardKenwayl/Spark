#include "gui/native/settings.h"

#include "core/provider_probe.h"
#include "core/session.h"
#include "gui/localization.h"
#include "gui/native/provider_editor.h"

#include <wx/artprov.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/datetime.h>
#include <wx/dcbuffer.h>
#include <wx/dirdlg.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/simplebook.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/srchctrl.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>
#include <wx/weakref.h>

#include <algorithm>
#include <array>
#include <atomic>
#include "platform/filesystem.h"
#include <ctime>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace spark {
namespace {
namespace fs = spark::fs;
constexpr size_t kMaxProviders = 256;
constexpr size_t kMaxArchives = 1000;
constexpr size_t kScanEntries = 4096;
constexpr size_t kMetadataBytes = 32768;
wxString U(const std::string& value) { return wxString::FromUTF8(value.data(), value.size()); }
std::string S(const wxString& value) { return value.ToStdString(wxConvUTF8); }
wxString T(const wchar_t* value) { return wxString(i18n::Text(value)); }

std::string ArchiveTimeLabel(uint64_t timestamp) {
    if (timestamp == 0) return {};
    const wxDateTime time(static_cast<std::time_t>(timestamp));
    if (!time.IsValid()) return {};
    return S(i18n::Format(L"{date}, {time}",
        {{L"date", time.FormatDate().ToStdWstring()}, {L"time", time.Format(L"%H:%M").ToStdWstring()}}));
}

class SettingsCard final : public wxPanel {
public:
    SettingsCard(wxWindow* parent, const char* name) : wxPanel(parent, wxID_ANY, wxDefaultPosition,
        wxDefaultSize, wxBORDER_NONE) {
        SetName(name);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            const wxColour background = GetParent()->GetBackgroundColour();
            const auto palette = NativeColors(background.Red() < 128);
            dc.SetBackground(wxBrush(background));
            dc.Clear();
            dc.SetPen(wxPen(palette.border));
            dc.SetBrush(wxBrush(palette.panel));
            dc.DrawRoundedRectangle(GetClientRect().Deflate(1), FromDIP(8));
        });
    }
};

class SettingsNavigationButton final : public NativeButton {
public:
    SettingsNavigationButton(wxWindow* parent, const wxString& label, const wxArtID& art = {})
        : NativeButton(parent, label, art) {
        Bind(wxEVT_PAINT, [this, back = art == wxART_GO_BACK](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            const auto p = NativeColors(GetForegroundColour().Red() > 180);
            dc.SetBackground(wxBrush(p.sidebar)); dc.Clear();
            if (!back && IsSelected()) {
                dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush(p.input));
                dc.DrawRectangle(GetClientRect().Deflate(FromDIP(8), 0));
            }
            const auto color = (back ? HasCapture() : IsSelected()) ? p.text : p.muted;
            dc.SetFont(GetFont()); dc.SetTextForeground(color);
            if (back) {
                dc.SetPen(wxPen(color));
                const int x = FromDIP(8), y = GetClientSize().y / 2;
                dc.DrawLine(x + FromDIP(14), y, x, y);
                dc.DrawLine(x, y, x + FromDIP(5), y - FromDIP(5));
                dc.DrawLine(x, y, x + FromDIP(5), y + FromDIP(5));
            }
            const int left = FromDIP(back ? 32 : 24);
            const auto label = wxControl::Ellipsize(GetLabel(), dc, wxELLIPSIZE_END,
                std::max(1, GetClientSize().x - left - FromDIP(8)));
            dc.DrawText(label, left, (GetClientSize().y - dc.GetTextExtent(label).y) / 2);
        });
    }
};

void Paint(wxWindow* window, bool dark) {
    ApplyNativeColors(window, dark);
}

wxStaticText* Label(wxWindow* parent, const wchar_t* key) {
    auto* label = new wxStaticText(parent, wxID_ANY, T(key), wxDefaultPosition,
                                    wxDefaultSize, wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    label->SetName("spark.locale:" + wxString(key));
    return label;
}
void RepairSelection(AppConfig& config) {
    const auto* provider = FindProvider(config, config.active.provider);
    if (!provider) {
        if (config.providers.empty()) { config.active = {}; return; }
        provider = &config.providers.front();
        config.active.provider = provider->id;
    }
    const auto* model = FindModel(*provider, config.active.model);
    if (!model) {
        config.active.model = provider->models.empty() ? "" : provider->models.front().id;
        model = FindModel(*provider, config.active.model);
    }
    const auto efforts = model ? EffectiveReasoningEfforts(*model) : std::vector<std::string>{};
    if (std::find(efforts.begin(), efforts.end(), config.active.reasoning_effort) == efforts.end())
        config.active.reasoning_effort = model ? EffectiveDefaultReasoningEffort(*model) : "";
}

fs::path DataPath(const AppConfig& config) {
    return NativePath(AbsolutePath(ResolveConfigPath(config.storage.data_dir, config.config_path),
                                   ParentPath(config.config_path)));
}

fs::path WorkspacePath(const AppConfig& config) {
    return NativePath(AbsolutePath(ResolveConfigPath(config.workspace.path, config.config_path),
                                   ParentPath(config.config_path)));
}

bool ArchivePathAllowed(const fs::path& file, const fs::path& data) {
    std::error_code error;
    const auto info = fs::symlink_status(file, error);
    if (error || !fs::is_regular_file(info) || file.extension() != ".jsonl") return false;
    const auto parent = fs::canonical(file.parent_path(), error);
    if (error) return false;
    for (const auto& root : {data / "archive", data / "sessions" / "archived"}) {
        const auto canonical = fs::canonical(root, error);
        if (!error && canonical == parent && IsPathWithin(PathToUtf8(file), PathToUtf8(data))) return true;
    }
    return false;
}

void ReadMetadata(const fs::path& file, std::string& title, std::string& workspace, std::string& scope) {
    spark::fs_ifstream input(file, std::ios::binary);
    if (!input) return;
    auto parse = [&](const std::string& bytes) {
        size_t start = 0;
        while (start < bytes.size()) {
            const size_t end = bytes.find('\n', start);
            if (end == std::string::npos) break;
            if (end - start <= 16384) {
                try {
                    const auto event = nlohmann::json::parse(bytes.substr(start, end - start),
                        [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
                            if (depth > 16) throw std::invalid_argument("depth");
                            return true;
                        });
                    if (event.is_object()) {
                        const auto type = event.value("type", std::string{});
                        const auto payload = event.value("payload", nlohmann::json::object());
                        if (payload.is_object() && (type == "session.created" || type == "session.title")) {
                            if (payload.contains("title") && payload["title"].is_string())
                                title = TruncateUtf8(SanitizeDisplayText(payload["title"].get<std::string>()), 512);
                            if (payload.contains("workspace") && payload["workspace"].is_string())
                                workspace = TruncateUtf8(SanitizeDisplayText(payload["workspace"].get<std::string>()), 2048);
                        }
                        if (payload.is_object() && type == "session.scope" && payload.contains("scope") && payload["scope"].is_string())
                            scope = TruncateUtf8(payload["scope"].get<std::string>(), 32);
                    }
                } catch (...) {}
            }
            start = end + 1;
        }
    };
    std::string bytes(kMetadataBytes, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<size_t>(input.gcount()));
    parse(bytes);
    input.clear();
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size > static_cast<std::streamoff>(kMetadataBytes)) {
        input.seekg(size - static_cast<std::streamoff>(kMetadataBytes));
        bytes.assign(kMetadataBytes, '\0');
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<size_t>(input.gcount()));
        const auto newline = bytes.find('\n');
        if (newline != std::string::npos) parse(bytes.substr(newline + 1));
    }
}
}  // namespace

struct NativeSettings::Probe {
    CancellationToken cancellation;
    std::atomic<bool> done{false};
    Result<ProviderProbeResult> result;
    std::string provider_name;
    std::string model;
};

struct NativeSettings::ArchiveJob {
    CancellationToken cancellation;
    std::atomic<bool> done{false};
    bool action = false;
    bool ok = true;
    std::vector<ArchiveEntry> entries;
    std::vector<ArchiveProject> projects;
    std::string status;
};

NativeSettings::NativeSettings(wxWindow* parent, const AppConfig& config,
                                 std::function<void(const AppConfig&)> saved, std::function<void()> back)
    : wxPanel(parent, wxID_ANY), config_(config), saved_(std::move(saved)), back_(std::move(back)), timer_(this) {
    syncing_ = true;
    navigation_ = new wxPanel(this);
    navigation_->SetName("spark.settings.navigation");
    back_button_ = new SettingsNavigationButton(navigation_, T(L"Back to app"), wxART_GO_BACK);
    back_button_->SetName("spark.locale:Back to app");
    for (size_t index = 0; index < 3; ++index) {
        const wchar_t* names[] = {L"General", L"Provider configuration", L"Archived chats"};
        nav_[index] = new SettingsNavigationButton(navigation_, T(names[index]));
        nav_[index]->SetName("spark.locale:" + wxString(names[index]));
        nav_[index]->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) {
            pages_->SetSelection(index);
            Localize();
            if (index == 2 && archives_dirty_) StartArchiveScan();
        });
    }
    section_title_ = Label(this, L"General");
    section_title_->SetName("spark.settings.title");
    section_title_->SetFont(wxFont(wxFontInfo(16).Bold()));
    delete_all_archives_ = new NativeButton(this, T(L"Delete all"));
    delete_all_archives_->SetName("spark.archives.delete-all");
    delete_all_archives_->SetMinSize(FromDIP(wxSize(-1, 34)));
    delete_all_archives_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { DeleteAllArchives(); });
    pages_ = new wxSimplebook(this, wxID_ANY);
    pages_->SetName("spark.settings.pages");
    general_page_ = new wxScrolledWindow(pages_, wxID_ANY);
    general_page_->SetName("spark.settings.general");
    general_page_->SetScrollRate(0, FromDIP(12));
    general_page_->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_DEFAULT);
    auto* providers = new wxPanel(pages_);
    auto* archived = new wxPanel(pages_);
    CreateGeneral(general_page_);
    CreateProviders(providers);
    CreateArchives(archived);
    pages_->AddPage(general_page_, T(L"General"));
    pages_->AddPage(providers, T(L"Providers"));
    pages_->AddPage(archived, T(L"Archived"));
    status_ = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                               wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    status_->SetName("spark.settings.status");
    back_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (probe_) probe_->cancellation.Cancel();
        if (archive_job_) { archive_job_->cancellation.Cancel(); archives_dirty_ = true; }
        const auto callback = back_;
        if (callback) callback();
    });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { Layout(); event.Skip(); });
    Bind(wxEVT_TIMER, &NativeSettings::Poll, this, timer_.GetId());
    Reload(config);
    timer_.Start(100);
}

NativeSettings::~NativeSettings() {
    timer_.Stop();
    if (probe_) probe_->cancellation.Cancel();
    if (archive_job_) archive_job_->cancellation.Cancel();
    if (probe_worker_.joinable()) probe_worker_.join();
    if (archive_worker_.joinable()) archive_worker_.join();
}

void NativeSettings::CreateGeneral(wxWindow* page) {
    const char* card_names[] = {"spark.settings.card.workspace",
        "spark.settings.card.preferences", "spark.settings.card.data"};
    const wchar_t* label_names[] = {L"Default task folder", L"Preferences", L"Data folder"};
    for (size_t i = 0; i < 3; ++i) {
        general_cards_[i] = new SettingsCard(page, card_names[i]);
        general_labels_[i] = Label(general_cards_[i], label_names[i]);
    }
    output_ = new wxTextCtrl(general_cards_[0], wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                             wxTE_PROCESS_ENTER);
    output_->SetName("spark.workspace.path");
    output_->SetMaxLength(8192);
    browse_ = new NativeButton(general_cards_[0], T(L"Choose"));
    browse_->SetName("spark.locale:Choose");
    browse_->SetToolTip(T(L"Choose the default task folder"));

    language_ = new NativeChoice(general_cards_[1]);
    language_->SetName("spark.language");
    language_->Append(T(L"Automatic")); languages_.push_back("system");
    for (const auto& locale : i18n::SupportedLocales()) {
        language_->Append(wxString(locale.name)); languages_.push_back(locale.code);
    }
    theme_ = new NativeChoice(general_cards_[1]);
    theme_->SetName("spark.theme");
    for (const wchar_t* value : {L"Dark", L"Light", L"System"}) theme_->Append(T(value));
    send_key_ = new NativeChoice(general_cards_[1]);
    send_key_->SetName("spark.send-key");
    send_key_->Append("Enter"); send_key_->Append("Ctrl+Enter");
    shell_ = new NativeChoice(general_cards_[1]);
    shell_->SetName("spark.shell");
    data_path_ = new wxStaticText(general_cards_[2], wxID_ANY, wxEmptyString, wxDefaultPosition,
                                  wxDefaultSize, wxST_ELLIPSIZE_MIDDLE | wxST_NO_AUTORESIZE);
    data_path_->SetName("spark.data.path");
    clear_cache_ = new NativeButton(general_cards_[2], T(L"Clear cache"));
    clear_cache_->SetName("spark.locale:Clear cache");

    for (auto* choice : {language_, theme_, shell_, send_key_})
        choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { SaveGeneral(); });
    output_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { SaveGeneral(); });
    output_->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) { SaveGeneral(); event.Skip(); });
    browse_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxDirDialog dialog(this, T(L"Choose the default task folder"), output_->GetValue(), wxDD_DIR_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK) { output_->ChangeValue(dialog.GetPath()); SaveGeneral(); }
    });
    clear_cache_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { CleanCache(); });
}

void NativeSettings::CreateProviders(wxWindow* page) {
    provider_pages_ = new wxSimplebook(page);
    provider_pages_->SetName("spark.provider.pages");
    provider_list_page_ = new wxPanel(provider_pages_);
    provider_list_page_->SetName("spark.provider.list");
    add_ = new NativeButton(this, T(L"Add provider"), wxART_PLUS);
    add_->SetName("spark.locale:Add provider");
    add_->SetToolTip(T(L"Add provider"));
    model_label_ = Label(provider_list_page_, L"Model configuration status");
    config_status_ = new wxStaticText(provider_list_page_, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                      wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    config_status_->SetName("spark.settings.provider.summary");
    config_label_ = Label(provider_list_page_, L"Configuration file");
    config_path_ = new wxStaticText(provider_list_page_, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                   wxDefaultSize, wxST_ELLIPSIZE_MIDDLE | wxST_NO_AUTORESIZE);
    config_path_->SetName("spark.settings.config.path");
    open_config_ = new NativeButton(provider_list_page_, T(L"Open config"));
    open_config_->SetName("spark.locale:Open config");
    open_config_->SetToolTip(T(L"Open config"));

    provider_list_ = new NativeProviderList(provider_list_page_);
    provider_list_->SetMinSize(provider_list_page_->FromDIP(wxSize(-1, 120)));
    provider_empty_ = new wxStaticText(provider_list_page_, wxID_ANY, T(L"No providers configured."),
                                        wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
    provider_empty_->SetName("spark.locale:No providers configured.");
    provider_pages_->AddPage(provider_list_page_, T(L"Provider configuration"));

    open_config_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const auto file = NativePath(AbsolutePath(config_.config_path, GetExecutableDirectory()));
        std::error_code error;
        if (config_.config_path.empty() || !fs::is_regular_file(file, error) ||
            !wxLaunchDefaultApplication(U(PathToUtf8(fs::absolute(file, error)))))
            Status("Could not open the config file.");
    });
    add_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EditProvider(true); });
    provider_list_->SetActionTooltips(
        {T(L"Send hi test"), T(L"Edit provider"), T(L"Duplicate provider"), T(L"Delete provider")},
        T(L"Reorder providers"));
    provider_list_->SetActionHandler(
        [this](size_t index, ProviderListAction action) { ProviderAction(index, action); });
    provider_list_->SetReorderHandler(
        [this](size_t source, size_t destination) { ReorderProvider(source, destination); });
}

void NativeSettings::CreateArchives(wxWindow* page) {
    archive_search_ = new wxSearchCtrl(page, wxID_ANY);
    archive_search_->SetName("spark.archives.search");
    archive_search_->SetDescriptiveText(T(L"Search archived chats"));
    archive_search_->SetMaxLength(256);
    archive_search_->SetMinSize(FromDIP(wxSize(80, 36)));
    archive_scope_ = new NativeChoice(page);
    archive_scope_->SetName("spark.archives.scope");
    archive_scope_->SetMinSize(FromDIP(wxSize(210, 36)));
    archive_pages_ = new wxSimplebook(page);
    archive_list_ = new NativeArchiveList(archive_pages_);
    archive_pages_->AddPage(archive_list_, {});
    archive_status_ = new wxStaticText(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                      wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    archive_status_->SetName("spark.archives.status");
    archive_search_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { FilterArchives(); });
    archive_scope_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        const int selection = archive_scope_->GetSelection();
        if (selection >= 0 && static_cast<size_t>(selection) < archive_scope_keys_.size())
            archive_scope_key_ = archive_scope_keys_[selection];
        FilterArchives();
    });
    archive_list_->SetActionHandler([this](size_t index, bool restore) { ArchiveAction(index, restore); });
}

bool NativeSettings::Layout() {
    if (laying_out_ || !status_) return false;
    LayoutSettings();
    return true;
}

void NativeSettings::LayoutSettings() {
    laying_out_ = true;
    struct Reset { bool& active; ~Reset() { active = false; } } reset{laying_out_};
    const auto d = [this](int value) { return FromDIP(value); };
    const auto place = [](wxWindow* control, int x, int y, int w, int h) {
        control->SetSize(x, y, std::max(1, w), std::max(1, h));
    };
    const auto measure = [&](wxWindow* control, const wxString& label, int minimum, int padding) {
        return std::max(d(minimum), control->GetTextExtent(label).x + d(padding));
    };
    const int width = GetClientSize().x, height = GetClientSize().y;
    int scroll_x = 0, scroll_y = 0;
    general_page_->GetViewStart(&scroll_x, &scroll_y);
    const int selected = pages_->GetSelection();
    const bool archived = selected == 2, providers = selected == 1;
    const bool editing = providers && provider_pages_->GetSelection() != 0;
    const int nav_width = archived ? d(238) : std::min(d(238), std::max(d(140), width / 3));
    place(navigation_, 0, 0, nav_width, height);
    place(back_button_, d(18), d(14), nav_width - d(36), d(40));
    for (int i = 0; i < 3; ++i)
        place(nav_[i], 0, d(70 + i * 48), nav_width, d(48));
    const int margin = d(!archived && width < d(800) ? 24 : 42);
    const int x = nav_width + margin;
    const int content_width = std::max(1, std::min(d(980),
        width - x - (providers ? d(24) : width < d(800) ? margin : d(64))));
    const int scrollbar = std::max(0, wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, this));
    const bool compact_providers = content_width - scrollbar < d(480);
    const int add_width = measure(add_, add_->GetLabel(), 130, 54);
    const int delete_width = measure(delete_all_archives_, delete_all_archives_->GetLabel(), 118, 44);
    const int title_width = archived ? content_width - delete_width - d(16)
        : providers && !compact_providers ? content_width - add_width - d(16) : content_width;
    section_title_->Show(!editing);
    place(section_title_, x, d(28), title_width, d(30));
    add_->Show(providers && !editing);
    place(add_, compact_providers ? x : x + std::max(0, content_width - add_width),
          d(compact_providers ? 72 : 24), std::min(add_width, content_width), d(36));
    delete_all_archives_->Show(archived);
    place(delete_all_archives_, x + std::max(0, content_width - delete_width), d(24),
          std::min(delete_width, content_width), d(34));
    const int page_top = d(editing ? 24 : 82);
    place(pages_, x, page_top, content_width, height - page_top - (editing ? d(24) : 0));
    pages_->Layout();
    status_->Show(!editing && (!providers || !status_->GetLabel().empty()));

    if (selected == 0) {
        // A second pass accounts for a vertical scrollbar appearing in a short window.
        for (int pass = 0; pass < 2; ++pass) {
            const int w = pass == 0 ? content_width : std::max(1, general_page_->GetClientSize().x);
            const int label_width = std::min(d(168), std::max(d(96), w / 3));
            const int inner_width = std::max(1, w - d(24));
            const int field_x = std::min(label_width, std::max(0, w - d(64)));
            const int field_width = std::max(1, w - field_x - d(12));
            const bool compact = w < d(650);
            std::array<NativeChoice*, 4> choices{language_, theme_, shell_, send_key_};
            std::array<int, 4> widths{};
            for (size_t i = 0; i < choices.size(); ++i) {
                widths[i] = d(100);
                for (unsigned j = 0; j < choices[i]->GetCount(); ++j)
                    widths[i] = std::max(widths[i], measure(choices[i], choices[i]->GetString(j).Left(128), 100, 42));
            }
            const int area_x = compact ? d(12) : field_x;
            const int area_width = compact ? inner_width : field_width;
            const int gap = d(8);
            if (compact && area_width >= d(220))
                for (int& choice_width : widths) choice_width = std::min(choice_width, (area_width - gap) / 2);
            std::array<wxRect, 4> rects{};
            int choice_y = d(compact ? 50 : 16);
            for (size_t first = 0; first < choices.size();) {
                size_t end = first + 1;
                int used = std::min(widths[first], area_width);
                while (end < choices.size() &&
                       used + gap + std::min(widths[end], area_width) <= area_width)
                    used += gap + std::min(widths[end++], area_width);
                int extra = std::max(0, area_width - used), choice_x = area_x;
                for (size_t i = first; i < end; ++i) {
                    const int share = extra / static_cast<int>(end - i);
                    const int choice_width = std::min(area_width - (choice_x - area_x),
                                                      std::min(widths[i], area_width) + share);
                    rects[i] = wxRect(choice_x, choice_y, std::max(1, choice_width), d(30));
                    choice_x += choice_width + gap;
                    extra -= share;
                }
                first = end;
                if (first < choices.size()) choice_y += d(34);
            }
            const int card_heights[] = {d(62), choice_y + d(46), d(62)};
            const int card_tops[] = {0, card_heights[0] + d(10),
                card_heights[0] + card_heights[1] + d(20)};
            const int total = card_tops[2] + card_heights[2];
            const int status_y = std::max(0, std::min(
                std::max(d(82) + total + d(12), height - d(48)), height - d(26)));
            place(pages_, x, d(82), content_width, status_y - d(94));
            general_page_->SetVirtualSize(1, total);
            for (int i = 0; i < 3; ++i) {
                const wxPoint at = general_page_->CalcScrolledPosition(wxPoint(0, card_tops[i]));
                place(general_cards_[i], at.x, at.y, w, card_heights[i]);
                general_labels_[i]->Show(w >= d(160));
                place(general_labels_[i], d(12), d(16),
                      i == 1 && compact ? inner_width : label_width - d(20), d(30));
            }
            const auto field = [&](wxWindow* text, NativeButton* button, int minimum) {
                const int button_width = std::min(std::max(1, field_width - 1),
                    std::min(measure(button, button->GetLabel(), minimum, 24),
                             std::max(d(52), field_width / 2)));
                const int field_gap = std::min(d(10), std::max(0, field_width - button_width - 1));
                text->Show(field_width > 1);
                place(text, field_x, d(16), field_width - button_width - field_gap, d(30));
                place(button, field_x + field_width - button_width, d(16), button_width, d(30));
            };
            field(output_, browse_, 80);
            field(data_path_, clear_cache_, 86);
            for (size_t i = 0; i < choices.size(); ++i)
                choices[i]->SetSize(rects[i]);
            place(status_, x + d(12), status_y, inner_width, d(26));
        }
        general_page_->Scroll(0, scroll_y);
    } else if (providers) {
        place(provider_pages_, 0, 0, content_width, pages_->GetClientSize().y);
        provider_pages_->Layout();
        if (!editing) {
            const int metadata = d(compact_providers ? 116 : 84) - page_top;
            const int open_width = std::min(content_width, measure(open_config_, open_config_->GetLabel(), 116, 24));
            place(model_label_, 0, metadata, content_width, d(24));
            place(config_status_, 0, metadata + d(30), content_width, d(compact_providers ? 24 : 42));
            place(config_label_, 0, metadata + d(compact_providers ? 66 : 78),
                  compact_providers ? content_width - open_width - d(16) : content_width, d(24));
            place(config_path_, 0, metadata + d(compact_providers ? 100 : 110),
                  compact_providers ? content_width : content_width - open_width - d(16), d(24));
            place(open_config_, content_width - open_width, metadata + d(compact_providers ? 60 : 104),
                  open_width, d(30));
            const int provider_top = metadata + d(compact_providers ? 138 : 152);
            const int available = std::max(1, height - page_top - provider_top - d(16));
            const int status_height = status_->GetLabel().empty() ? 0 :
                std::min(d(76), std::max(d(36), available / 3));
            const int list_height = std::max(1, available - (status_height ? status_height + d(12) : 0));
            place(provider_list_, 0, provider_top, content_width, list_height);
            place(provider_empty_, 0, provider_top + d(32), content_width, d(30));
            place(status_, x, page_top + provider_top + list_height + d(12), content_width, status_height);
        }
    } else if (archived) {
        const int gap = std::min(d(12), std::max(0, content_width - 2));
        const int scope = std::clamp(content_width - d(192), 1,
                                    std::max(1, std::min(d(210), content_width - gap - 1)));
        const int search = std::max(1, content_width - scope - gap);
        place(archive_search_, 0, d(104) - page_top, search, d(36));
        place(archive_scope_, search + gap, d(104) - page_top, scope, d(36));
        place(archive_pages_, 0, d(152) - page_top, content_width, height - d(204));
        archive_pages_->Layout();
        place(archive_status_, 0, height - page_top - d(46), content_width, d(24));
        status_->Show(!status_->GetLabel().empty());
        place(status_, x, std::max(0, height - d(20)), content_width, d(18));
    }
    if (section_title_->IsShown()) section_title_->Raise();
    if (add_->IsShown()) add_->Raise();
    if (delete_all_archives_->IsShown()) delete_all_archives_->Raise();
    if (status_->IsShown()) status_->Raise();
}

void NativeSettings::ApplyTheme() {
    const bool dark = config_.ui.theme == "dark" ||
                      (config_.ui.theme == "system" && wxSystemSettings::GetAppearance().IsDark());
    Paint(this, dark);
    const auto palette = NativeColors(dark);
    navigation_->SetBackgroundColour(palette.sidebar);
    for (auto* child : navigation_->GetChildren()) child->SetBackgroundColour(palette.sidebar);
    for (auto* card : general_cards_) {
        if (!card) continue;
        card->SetBackgroundColour(palette.panel);
        for (auto* child : card->GetChildren())
            if (!dynamic_cast<wxTextCtrl*>(child)) child->SetBackgroundColour(palette.panel);
        card->Refresh(false);
    }
}

void NativeSettings::Localize() {
    i18n::SetLocale(config_.ui.language);
    const auto visit = [&](auto&& self, wxWindow* window) -> void {
        const wxString name = window->GetName();
        if (name.StartsWith("spark.locale:")) {
            const auto key = name.Mid(13).ToStdWstring();
            if (auto* button = dynamic_cast<NativeButton*>(window); button && button->GetLabel().empty())
                button->SetToolTip(T(key.c_str()));
            else window->SetLabel(T(key.c_str()));
        }
        for (auto* child : window->GetChildren()) self(self, child);
    };
    visit(visit, this);
    const wchar_t* names[] = {L"General", L"Provider configuration", L"Archived chats"};
    section_title_->SetLabel(T(names[pages_->GetSelection()]));
    for (int index = 0; index < 3; ++index) nav_[index]->SetSelected(index == pages_->GetSelection());
    language_->SetString(0, T(L"Automatic"));
    for (size_t index = 0; index < 3; ++index)
        theme_->SetString(index, T(index == 0 ? L"Dark" : index == 1 ? L"Light" : L"System"));
    delete_all_archives_->SetLabel(T(L"Delete all"));
    delete_all_archives_->Show(pages_->GetSelection() == 2);
    archive_list_->SetLabels(T(L"Unarchive"), T(L"Delete archived chat"));
    RefreshArchiveScopes();
    FilterArchives();
    archive_search_->SetDescriptiveText(T(L"Search archived chats"));
    Layout();
}

void NativeSettings::Reload(const AppConfig& config) {
    if (probe_) probe_->cancellation.Cancel();
    const bool data_changed = DataPath(config_) != DataPath(config);
    config_ = config;
    syncing_ = true;
    auto language = std::find(languages_.begin(), languages_.end(), config_.ui.language);
    if (language == languages_.end()) {
        languages_.push_back(config_.ui.language);
        language_->Append(U(config_.ui.language));
        language = languages_.end() - 1;
    }
    language_->SetSelection(static_cast<int>(language - languages_.begin()));
    theme_->SetSelection(config_.ui.theme == "light" ? 1 : config_.ui.theme == "system" ? 2 : 0);
    send_key_->SetSelection(config_.ui.send_key == "ctrl+enter" ? 1 : 0);
    shell_->Clear();
    for (const auto* path : {"/bin/sh", "/bin/bash", "/bin/zsh"})
        if (PathExists(path)) shell_->Append(U(path));
    if (shell_->FindString(U(config_.ui.shell), true) == wxNOT_FOUND) shell_->Append(U(config_.ui.shell));
    shell_->SetStringSelection(U(config_.ui.shell));
    output_->ChangeValue(U(PathToUtf8(WorkspacePath(config_))));
    const auto data_path = PathToUtf8(DataPath(config_));
    data_path_->SetLabel(U(data_path));
    data_path_->SetToolTip(U(data_path));
    const auto config_path = AbsolutePath(config_.config_path, GetExecutableDirectory());
    config_path_->SetLabel(U(config_path));
    config_path_->SetToolTip(U(config_path));
    RefreshProviders();
    syncing_ = false;
    if (data_changed) {
        if (archive_job_) archive_job_->cancellation.Cancel();
        archives_.clear(); archive_projects_.clear(); archive_scan_status_.clear(); archives_dirty_ = true;
        RefreshArchiveScopes();
        FilterArchives();
    }
    Localize();
    ApplyTheme();
    if (pages_->GetSelection() == 2 && archives_dirty_) StartArchiveScan();
}

void NativeSettings::SaveGeneral() {
    if (syncing_) return;
    AppConfig updated = config_;
    const auto language = language_->GetSelection();
    if (language >= 0) updated.ui.language = languages_[language];
    const char* themes[] = {"dark", "light", "system"};
    updated.ui.theme = themes[std::max(0, theme_->GetSelection())];
    updated.ui.send_key = send_key_->GetSelection() == 1 ? "ctrl+enter" : "enter";
    updated.ui.shell = Trim(S(shell_->GetStringSelection()));
    const std::string workspace_input = Trim(S(output_->GetValue()));
    if (updated.ui.shell.empty() || workspace_input.empty()) {
        Status("Shell and no-project output folder cannot be empty."); return;
    }
    const std::string config_directory = updated.config_path.empty()
        ? GetExecutableDirectory() : ParentPath(updated.config_path);
    updated.workspace.path = AbsolutePath(ResolveConfigPath(workspace_input, updated.config_path),
                                          config_directory);
    if (updated.workspace.path.empty()) {
        Status("The default task folder path is invalid."); return;
    }
    if (ConfigToJson(updated) != ConfigToJson(config_)) Commit(std::move(updated), false);
}

void NativeSettings::CleanCache() {
    std::error_code error;
    fs::remove_all(DataPath(config_) / "cache", error);
    if (error) {
        Status(wxString(i18n::Format(L"Cache cleanup failed: {error}",
            {{L"error", Utf8ToWide(error.message())}})).ToStdString(wxConvUTF8));
        return;
    }
    Status(S(T(L"Cleared data/cache. Sessions and archives were kept.")));
}

bool NativeSettings::Commit(AppConfig updated, bool providers) {
    if (providers) {
        RepairSelection(updated);
        // Ignore unrelated project/credential errors; settings never fabricate a project.
        const auto errors = ValidateConfig(updated, false);
        for (const auto& error : errors) {
            if (error.rfind("providers", 0) == 0 || error.rfind("active.", 0) == 0) {
                Status(error); return false;
            }
        }
    }
    if (updated.config_path.empty()) { Status("No config file path is available."); return false; }
    const auto result = SaveConfigAtomically(updated);
    if (!result.ok) { Status(result.error); return false; }
    if (probe_) probe_->cancellation.Cancel();
    // Resolve environment references again without reinterpreting other application settings.
    if (providers) {
        const auto resolved = ConfigFromJson(ConfigToJson(updated), updated.config_path);
        updated.providers = resolved.providers;
    }
    config_ = std::move(updated);
    Localize();
    ApplyTheme();
    RefreshProviders();
    Status("Settings saved.");
    // A controller may synchronously call Reload() or replace this page.
    const auto callback = saved_;
    const auto snapshot = config_;
    if (callback) callback(snapshot);
    return true;
}

void NativeSettings::RefreshProviders() {
    std::vector<ProviderCard> cards;
    size_t model_count = 0;
    for (size_t index = 0; index < std::min(config_.providers.size(), kMaxProviders); ++index) {
        const auto& provider = config_.providers[index];
        ProviderCard card;
        card.id = U(provider.id);
        card.name = U(TruncateUtf8(SanitizeDisplayText(provider.label.empty() ? provider.id : provider.label), 512));
        card.protocol = U(TruncateUtf8(provider.protocol, 64));
        card.endpoint = U(TruncateUtf8(provider.base_url, 1024));
        cards.push_back(std::move(card));
        model_count += provider.models.size();
    }
    provider_list_->SetProviders(std::move(cards));
    provider_list_->Show(!config_.providers.empty());
    provider_empty_->Show(config_.providers.empty());
    add_->Enable(config_.providers.size() < kMaxProviders);
    const auto errors = ValidateConfig(config_, false);
    const wxString summary = wxString::Format("%zu providers, %zu models; %zu configuration issues.",
                                              config_.providers.size(), model_count, errors.size());
    config_status_->SetLabel(summary);
    config_status_->SetToolTip(errors.empty() ? summary : summary + "\n" + U(TruncateUtf8(errors.front(), 1024)));
    if (config_.providers.size() > kMaxProviders) Status("Provider list is limited to 256 entries.");
}

void NativeSettings::EditProvider(bool add, size_t index) {
    if (add && config_.providers.size() >= kMaxProviders) return;
    if (!add && index >= config_.providers.size()) return;
    if (provider_editor_page_ != wxNOT_FOUND) {
        provider_pages_->DeletePage(static_cast<size_t>(provider_editor_page_));
        provider_editor_ = nullptr;
        provider_editor_page_ = wxNOT_FOUND;
    }
    ProviderConfig provider;
    if (!add) provider = config_.providers[index];
    else {
        provider.id = "provider-" + NewSessionId();
        provider.protocol = "responses";
    }
    const auto id = provider.id;
    provider_editor_ = new NativeProviderEditor(provider_pages_, std::move(provider),
                                                config_.context, config_.ui.theme);
    provider_editor_->SetName("spark.provider.editor");
    provider_editor_->SetHandlers([this, add, id](const ProviderConfig& edited) {
        auto updated = config_;
        if (add) updated.providers.push_back(edited);
        else {
            const auto found = std::find_if(updated.providers.begin(), updated.providers.end(),
                                           [&](const ProviderConfig& item) { return item.id == id; });
            if (found == updated.providers.end())
                return Result<void>::Failure("Provider no longer exists.");
            *found = edited;
        }
        return Commit(std::move(updated), true)
            ? Result<void>::Success()
            : Result<void>::Failure(S(status_->GetLabel()));
    }, [this] { ShowProviderList(); });
    provider_editor_page_ = static_cast<int>(provider_pages_->GetPageCount());
    provider_pages_->AddPage(provider_editor_, T(L"Edit provider"), true);
    provider_pages_->Layout();
    Layout();
}

void NativeSettings::ShowProviderList() {
    if (provider_pages_ && provider_pages_->GetPageCount() > 0)
        provider_pages_->SetSelection(0);
    Layout();
}

void NativeSettings::ProviderAction(size_t index, ProviderListAction action) {
    if (index >= config_.providers.size()) return;
    const auto id = config_.providers[index].id;
    if (action == ProviderListAction::Delete) {
        const auto& provider = config_.providers[index];
        const auto message = wxString(i18n::Format(L"Delete provider \"{name}\"? Existing chats will be kept.",
            {{L"name", Utf8ToWide(provider.label.empty() ? id : provider.label)}}));
        wxWeakRef<NativeSettings> owner(this);
        const int choice = wxMessageBox(message, T(L"Delete provider"),
            wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
        if (!owner || owner->IsBeingDeleted() || choice != wxYES) return;
    }
    if (action == ProviderListAction::Test) {
        StartProbe(index);
        return;
    }
    if (action == ProviderListAction::Edit) {
        EditProvider(false, index);
        return;
    }
    auto updated = config_;
    const auto found = std::find_if(updated.providers.begin(), updated.providers.end(),
                                   [&](const ProviderConfig& item) { return item.id == id; });
    if (found == updated.providers.end()) return;
    const auto found_index = static_cast<size_t>(found - updated.providers.begin());
    if (action == ProviderListAction::Duplicate && updated.providers.size() < kMaxProviders) {
        auto copy = *found;
        copy.id = "provider-" + NewSessionId();
        copy.label = (copy.label.empty() ? id : copy.label) + " (copy)";
        updated.providers.insert(updated.providers.begin() + static_cast<std::ptrdiff_t>(found_index + 1), std::move(copy));
    } else if (action == ProviderListAction::Delete && updated.providers.size() > 1) {
        updated.providers.erase(found);
    }
    else return;
    Commit(std::move(updated), true);
}

void NativeSettings::ReorderProvider(size_t source, size_t destination) {
    if (source >= config_.providers.size() || destination >= config_.providers.size() || source == destination) return;
    auto updated = config_;
    auto provider = std::move(updated.providers[source]);
    updated.providers.erase(updated.providers.begin() + static_cast<std::ptrdiff_t>(source));
    updated.providers.insert(updated.providers.begin() + static_cast<std::ptrdiff_t>(destination), std::move(provider));
    Commit(std::move(updated), true);
}

void NativeSettings::StartProbe(size_t index) {
    if (probe_) { probe_->cancellation.Cancel(); Status("Cancelling provider test..."); return; }
    if (index >= config_.providers.size()) return;
    const auto provider = config_.providers[index];
    auto state = std::make_shared<Probe>();
    state->provider_name = provider.label.empty() ? provider.id : provider.label;
    state->model = ProviderProbeModelId(provider);
    try {
        probe_worker_ = std::thread([state, provider] {
            try { state->result = ProbeProvider(provider, &state->cancellation); }
            catch (...) { state->result = Result<ProviderProbeResult>::Failure("Provider test failed."); }
            state->done.store(true, std::memory_order_release);
        });
    } catch (...) { Status("Could not start provider test."); return; }
    probe_ = std::move(state);
    Status("Sending hi to " + (provider.label.empty() ? provider.id : provider.label) +
           " using " + ProviderProbeModelId(provider) + "...");
}

void NativeSettings::StartArchiveScan() {
    if (archive_job_) return;
    auto state = std::make_shared<ArchiveJob>();
    const auto data = DataPath(config_);
    try {
        archive_worker_ = std::thread([state, data] {
            try {
                const auto projects_text = ReadFileUtf8(PathToUtf8(data / "projects.json"));
                const auto projects = projects_text ? nlohmann::json::parse(*projects_text, nullptr, false) : nlohmann::json{};
                if (projects.is_object() && projects.contains("projects") && projects["projects"].is_array())
                    for (const auto& value : projects["projects"]) {
                        if (!value.is_object() || state->projects.size() >= 256) continue;
                        ArchiveProject project;
                        project.name = value.value("name", std::string{});
                        const auto workspace = value.value("workspace_path", std::string{});
                        if (!workspace.empty()) project.folders.push_back(workspace);
                        if (value.contains("folders") && value["folders"].is_array())
                            for (const auto& path : value["folders"]) if (path.is_string()) project.folders.push_back(path.get<std::string>());
                        if (project.folders.empty()) continue;
                        project.key = "project:" + NormalizePathForComparison(project.folders.front());
                        if (project.name.empty())
                            project.name = PathToUtf8(NativePath(project.folders.front()).filename());
                        state->projects.push_back(std::move(project));
                    }
                size_t scanned = 0, unreadable = 0;
                for (const auto& root : {data / "archive", data / "sessions" / "archived"}) {
                    std::error_code error;
                    if (!fs::exists(root, error)) { if (error) ++unreadable; continue; }
                    fs::directory_iterator iterator(root, error), end;
                    if (error) { ++unreadable; continue; }
                    for (; iterator != end && !error; iterator.increment(error)) {
                        if (state->cancellation.IsCancelled()) break;
                        if (++scanned > kScanEntries || state->entries.size() >= kMaxArchives) break;
                        const auto file = iterator->path();
                        if (!ArchivePathAllowed(file, data)) continue;
                        ArchiveEntry entry;
                        entry.path = PathToUtf8(file);
                        entry.title = TruncateUtf8(PathToUtf8(file.stem()), 512);
                        ReadMetadata(file, entry.title, entry.workspace, entry.scope);
                        if (entry.scope != "recent" && !entry.workspace.empty()) {
                            for (size_t index = 0; index < state->projects.size() &&
                                 entry.project == static_cast<size_t>(-1); ++index)
                                for (const auto& folder : state->projects[index].folders)
                                    if (NormalizePathForComparison(folder) == NormalizePathForComparison(entry.workspace)) {
                                        entry.project = index; break;
                                    }
                        }
                        const wxDateTime modified = wxFileName(U(entry.path)).GetModificationTime();
                        if (modified.IsValid()) {
                            const auto timestamp = modified.GetTicks();
                            if (timestamp > 0) entry.modified = static_cast<uint64_t>(timestamp);
                        }
                        state->entries.push_back(std::move(entry));
                    }
                    if (error) ++unreadable;
                    if (scanned > kScanEntries || state->entries.size() >= kMaxArchives) break;
                }
                std::sort(state->entries.begin(), state->entries.end(), [](const ArchiveEntry& a, const ArchiveEntry& b) {
                    return a.modified > b.modified;
                });
                state->status = (scanned > kScanEntries || state->entries.size() >= kMaxArchives)
                    ? "Limit reached: 1000 chats / 4096 directory entries. Search covers loaded chats." : "";
                if (unreadable) state->status += " Some archive folders could not be read.";
            } catch (...) { state->ok = false; state->status = "Could not read archived chats."; }
            state->done.store(true, std::memory_order_release);
        });
    } catch (...) { Status("Could not start archive scan."); return; }
    archive_job_ = std::move(state);
    archives_dirty_ = false;
    archive_list_->Disable(); delete_all_archives_->Disable();
    archive_status_->SetLabel(T(L"Loading archived chats..."));
}

void NativeSettings::RefreshArchiveScopes() {
    archive_scope_->Clear();
    archive_scope_keys_ = {"all"};
    archive_scope_->Append(T(L"All projects"));
    for (const auto& project : archive_projects_) {
        archive_scope_keys_.push_back(project.key);
        archive_scope_->Append(U(project.name));
    }
    archive_scope_keys_.push_back("recent");
    archive_scope_->Append(T(L"Recent"));
    const auto found = std::find(archive_scope_keys_.begin(), archive_scope_keys_.end(), archive_scope_key_);
    const int selected = found == archive_scope_keys_.end() ? 0
        : static_cast<int>(found - archive_scope_keys_.begin());
    archive_scope_key_ = archive_scope_keys_[selected];
    archive_scope_->SetSelection(selected);
}

void NativeSettings::FilterArchives() {
    const auto query = archive_search_->GetValue().Lower();
    std::vector<std::vector<ArchiveListRow>> groups(archive_projects_.size() + 1);
    size_t count = 0;
    for (size_t index = 0; index < archives_.size(); ++index) {
        const auto& entry = archives_[index];
        const bool project = entry.project < archive_projects_.size();
        const std::string scope = project ? archive_projects_[entry.project].key : "recent";
        if (archive_scope_key_ != "all" && archive_scope_key_ != scope) continue;
        if (!query.empty() && !U(entry.title + " " + entry.workspace).Lower().Contains(query)) continue;
        const size_t group = project ? entry.project : archive_projects_.size();
        groups[group].push_back({false, entry.title.empty() ? T(L"New chat") : U(entry.title),
                                U(ArchiveTimeLabel(entry.modified)), U(entry.title + "\n" + entry.workspace), index});
        ++count;
    }
    std::vector<ArchiveListRow> rows;
    for (size_t group = 0; group < groups.size(); ++group) {
        if (groups[group].empty()) continue;
        rows.push_back({true, group < archive_projects_.size() ? U(archive_projects_[group].name) : T(L"No project"),
                       wxString(i18n::Format(L"Chats: {count}", {{L"count", std::to_wstring(groups[group].size())}})), {}, 0});
        rows.insert(rows.end(), groups[group].begin(), groups[group].end());
    }
    archive_list_->SetRows(std::move(rows));
    const bool loading = archive_job_ && !archive_job_->action;
    archive_pages_->SetSelection(0);
    archive_list_->Enable(!archive_job_);
    delete_all_archives_->Enable(!archive_job_ && !archives_.empty());
    wxString status = loading ? T(L"Loading archived chats...") : count == 0 ? T(L"No archived chats.")
        : wxString(i18n::Format(L"Archived chats: {count}.", {{L"count", std::to_wstring(count)}}));
    if (!loading && !archive_scan_status_.empty()) {
        if (!status.empty()) status += " ";
        status += U(archive_scan_status_);
    }
    archive_status_->SetLabel(status);
    archive_status_->SetToolTip(archive_status_->GetLabel());
}

void NativeSettings::ArchiveAction(size_t index, bool restore) {
    if (archive_job_) return;
    if (index >= archives_.size()) return;
    const auto file = NativePath(archives_[index].path);
    if (!restore) {
        const auto data_before = DataPath(config_);
        wxWeakRef<NativeSettings> owner(this);
        const int choice = wxMessageBox(
            T(L"Permanently delete this archived chat? The session JSONL will be deleted immediately. This cannot be undone."),
            T(L"Delete permanently"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
        if (!owner || owner->IsBeingDeleted() || choice != wxYES || archive_job_ ||
            DataPath(config_) != data_before) return;
        const auto entry = std::find_if(archives_.begin(), archives_.end(),
            [&](const ArchiveEntry& archive) { return NativePath(archive.path) == file; });
        if (entry == archives_.end()) return;
    }
    const auto data = DataPath(config_);
    auto state = std::make_shared<ArchiveJob>();
    state->action = true;
    try {
        archive_worker_ = std::thread([state, file, data, restore] {
            try {
                if (state->cancellation.IsCancelled()) throw std::runtime_error("cancelled");
                if (!ArchivePathAllowed(file, data)) throw std::runtime_error("archive path");
                std::error_code error;
                if (restore) {
                    const auto sessions = data / "sessions";
                    fs::create_directories(sessions, error);
                    if (error || !IsPathWithin(PathToUtf8(sessions), PathToUtf8(data)))
                        throw std::runtime_error("sessions path");
                    const auto destination = sessions / file.filename();
                    wxFile input(U(PathToUtf8(file)), wxFile::read), output;
                    // Exclusive creation protects existing sessions, including dangling symlinks.
                    if (!input.IsOpened() || !output.Create(U(PathToUtf8(destination)), false, 0600)) {
                        state->ok = false; state->status = "Restore failed; a live session may already have this filename.";
                    } else {
                        std::array<char, 65536> buffer{};
                        bool copied = true;
                        while (!state->cancellation.IsCancelled()) {
                            const auto count = input.Read(buffer.data(), buffer.size());
                            if (count == 0) break;
                            if (count < 0 || output.Write(buffer.data(), static_cast<size_t>(count)) != static_cast<size_t>(count)) {
                                copied = false; break;
                            }
                        }
                        if (copied && !state->cancellation.IsCancelled()) copied = output.Flush();
                        copied = output.Close() && copied && !state->cancellation.IsCancelled();
                        input.Close();
                        if (!copied) {
                            fs::remove(destination, error);
                            state->ok = false;
                            state->status = error ? "Restore failed; archive kept, but incomplete session copy could not be removed."
                                                  : "Restore failed; archive kept.";
                        } else if (!fs::remove(file, error) || error) {
                            state->ok = false; state->status = "Session restored, but the archive copy could not be removed.";
                        } else state->status = "Chat restored.";
                    }
                } else if (!fs::remove(file, error) || error) {
                    state->ok = false; state->status = "Could not delete archived chat.";
                } else state->status = "Archived chat deleted.";
            } catch (...) { state->ok = false; state->status = "Archive action failed; check the file and storage paths."; }
            state->done.store(true, std::memory_order_release);
        });
    } catch (...) { Status("Could not start archive action."); return; }
    archive_job_ = std::move(state);
    archive_list_->Disable(); delete_all_archives_->Disable();
    Status(restore ? "Restoring chat..." : "Deleting archived chat...");
}

void NativeSettings::DeleteAllArchives() {
    if (archive_job_ || archives_.empty()) return;
    const auto data = DataPath(config_);
    wxWeakRef<NativeSettings> owner(this);
    const int choice = wxMessageBox(
        T(L"Permanently delete all archived chats? Current chats, projects, configuration files, and workspace files will be kept."),
        T(L"Delete all"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
    if (!owner || owner->IsBeingDeleted() || choice != wxYES || archive_job_ ||
        archives_.empty() || DataPath(config_) != data) return;
    auto state = std::make_shared<ArchiveJob>();
    state->action = true;
    try {
        archive_worker_ = std::thread([state, data] {
            size_t scanned = 0;
            try {
                for (const auto& root : {data / "archive", data / "sessions" / "archived"}) {
                    std::error_code error;
                    if (!fs::exists(root, error)) {
                        if (error) state->ok = false;
                        continue;
                    }
                    fs::directory_iterator iterator(root, error), end;
                    if (error) state->ok = false;
                    for (; iterator != end && !error; iterator.increment(error)) {
                        if (state->cancellation.IsCancelled()) break;
                        if (++scanned > kScanEntries) { state->ok = false; break; }
                        const auto file = iterator->path();
                        if (!ArchivePathAllowed(file, data)) continue;
                        if (!fs::remove(file, error) || error) { state->ok = false; error.clear(); }
                    }
                    if (error) state->ok = false;
                    if (state->cancellation.IsCancelled() || scanned > kScanEntries) break;
                }
                state->status = state->ok ? "All archived chats deleted."
                    : "Some archived chats could not be deleted; refresh and try again.";
            } catch (...) { state->ok = false; state->status = "Could not delete archived chats."; }
            state->done.store(true, std::memory_order_release);
        });
    } catch (...) { Status("Could not start archive action."); return; }
    archive_job_ = std::move(state);
    archive_list_->Disable(); delete_all_archives_->Disable();
    Status("Deleting archived chats...");
}

void NativeSettings::Poll(wxTimerEvent&) {
    if (probe_ && probe_->done.load(std::memory_order_acquire)) {
        if (probe_worker_.joinable()) probe_worker_.join();
        auto state = std::move(probe_);
        const auto& result = state->result;
        if (state->cancellation.IsCancelled()) Status("Provider test cancelled.");
        else {
            const auto& value = result.value;
            const std::string model = value.model_id.empty() ? state->model : value.model_id;
            const std::string http = value.http_status > 0
                ? "HTTP " + std::to_string(value.http_status) : "No HTTP response";
            const std::string reply = result.ok ? value.text : "hi test failed: " + result.error;
            Status(state->provider_name + " / " + model + ": " + http + ", " +
                   std::to_string(value.elapsed_ms) + " ms\n" + reply);
        }
    }
    if (archive_job_ && archive_job_->done.load(std::memory_order_acquire)) {
        if (archive_worker_.joinable()) archive_worker_.join();
        auto state = std::move(archive_job_);
        archive_list_->Enable();
        if (!state->cancellation.IsCancelled()) {
            if (state->action) {
                archives_dirty_ = true;
                Status(state->status);
            } else {
                archives_ = std::move(state->entries);
                archive_projects_ = std::move(state->projects);
                RefreshArchiveScopes();
                archive_scan_status_ = state->status;
            }
        }
        FilterArchives();
        if (archives_dirty_ && pages_->GetSelection() == 2) StartArchiveScan();
        // Reuse the supplied callback so the shell can refresh history after file actions.
        if (state->action && !state->cancellation.IsCancelled()) {
            const auto callback = saved_;
            const auto snapshot = config_;
            if (callback) callback(snapshot);
        }
    }
}

void NativeSettings::Status(const std::string& text) {
    const auto safe = TruncateUtf8(SanitizeDisplayText(RedactSecrets(text, CollectConfiguredSecrets(config_))), 2048);
    status_->SetLabel(U(safe));
    status_->SetToolTip(U(safe));
    Layout();
}

}  // namespace spark
