#include "gui/native/app.h"
#include "gui/localization.h"
#include "core/subagents.h"

#include <wx/app.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/display.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/stattext.h>
#include <wx/utils.h>

#include <iostream>

class LocalizationApplication : public wxApp {
public:
    bool OnInit() override { wxInitAllImageHandlers(); return true; }
};
wxIMPLEMENT_APP_NO_MAIN(LocalizationApplication);

namespace {
int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << message << '\n'; }
}
void Events() {
    for (int index = 0; index < 4; ++index) { wxYield(); wxMilliSleep(20); }
}
template<class Predicate>
wxWindow* Find(wxWindow* root, const Predicate& predicate) {
    if (predicate(root)) return root;
    for (auto* child : root->GetChildren())
        if (auto* found = Find(child, predicate)) return found;
    return nullptr;
}
void CheckText(wxWindow* root, const wchar_t* key) {
    const wxString expected(spark::i18n::Text(key));
    Check(Find(root, [&](wxWindow* window) {
        return dynamic_cast<spark::NativeButton*>(window) && window->GetLabel() == expected;
    }) != nullptr, "summary tab follows the selected application language");
}
void Capture(wxWindow* window, const wxString& name) {
    const wxString directory = wxGetenv("SPARK_LOCALE_SCREENSHOTS");
    if (directory.empty()) return;
    wxFileName::Mkdir(directory, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    const auto size = window->GetClientSize();
    wxBitmap bitmap(size.x, size.y);
    wxMemoryDC output(bitmap);
    wxClientDC input(window);
    output.Blit(0, 0, size.x, size.y, &input, 0, 0);
    output.SelectObject(wxNullBitmap);
    const auto scale = wxString::Format("-%.0fx", window->GetDPIScaleFactor());
    Check(bitmap.ConvertToImage().SaveFile(directory + "/" + name + scale + ".jpg", wxBITMAP_TYPE_JPEG),
          "localized GUI screenshot is saved");
}
}  // namespace

int main(int argc, char** argv) {
    using namespace spark;
    wxSetAssertHandler([](const wxString&, int, const wxString&, const wxString&, const wxString& message) {
        ++failures; std::cerr << message << '\n';
    });
    if (!wxEntryStart(argc, argv) || !wxTheApp->CallOnInit()) return 2;
    const auto temporary = fs::temp_directory_path() / NativePath("spark-locale-" + NewSessionId());
    fs::create_directories(temporary / "data");
    AppConfig config;
    config.config_path = PathToUtf8(temporary / "config.json");
    config.storage.data_dir = PathToUtf8(temporary / "data");
    config.workspace.path = PathToUtf8(temporary / "outputs");
    config.ui.language = "ko-KR";
    config.ui.theme = "dark";
    config.ui.send_key = "ctrl+enter";
    config.permissions.default_mode = "approve_for_me";
    config.raw = {{"fixture_extension", {{"preserve", true}}}};
    ProviderConfig provider;
    provider.id = "fixture";
    provider.label = "Synthetic provider";
    provider.base_url = "http://127.0.0.1:1";
    ModelConfig model;
    model.id = "deepseek-flash";
    provider.models.push_back(model);
    config.providers.push_back(provider);
    config.active = {"fixture", model.id, "high"};
    McpServerConfig mcp;
    mcp.id = "disabled-fixture";
    mcp.enabled = false;
    mcp.command = "never-start-this-disabled-fixture";
    config.mcp_servers.push_back(mcp);
    const auto initial = ConfigToJson(config);
    Check(SaveConfigAtomically(config).ok, "isolated localization fixture configuration is saved");
    i18n::SetLocale("zh-CN");
    auto* frame = new NativeFrame(config);
    frame->Show();
    Events();
    const int display_index = wxDisplay::GetFromWindow(frame);
    const wxRect work = wxDisplay(display_index == wxNOT_FOUND ? 0 : display_index).GetClientArea();
    Check(work.Contains(frame->GetScreenRect()),
          "initial Native window fits the monitor work area");
    Check(frame->GetMinSize().x <= work.width && frame->GetMinSize().y <= work.height,
          "minimum Native window fits the monitor work area");
    std::cout << "Native scale " << frame->GetDPIScaleFactor() << ", window "
              << frame->GetSize().x << 'x' << frame->GetSize().y << ", work "
              << work.width << 'x' << work.height << '\n';
    auto* summary = dynamic_cast<NativeSummaryPanel*>(Find(frame, [](wxWindow* window) {
        return dynamic_cast<NativeSummaryPanel*>(window) != nullptr;
    }));
    Check(summary != nullptr, "application creates its summary panel");
    if (summary) {
        CheckText(summary, L"Subagents");
        CheckText(summary, L"Sources");
    }
    Capture(frame, "korean-chat");
    auto* summary_host = new wxFrame(nullptr, wxID_ANY, "Synthetic retained summary");
    auto* retained_summary = new NativeSummaryPanel(summary_host, {});
    auto* summary_sizer = new wxBoxSizer(wxVERTICAL);
    summary_sizer->Add(retained_summary, 1, wxEXPAND);
    summary_host->SetSizer(summary_sizer);
    summary_host->SetSize(500, 400);
    SubAgentInfo running;
    running.id = "synthetic-running"; running.name = "Synthetic running";
    SubAgentInfo finished = running;
    finished.id = "synthetic-finished"; finished.name = "Synthetic finished";
    finished.state = SubAgentState::Completed;
    const std::vector<SubAgentInfo> unchanged_agents{running, finished};
    retained_summary->SetAgents(unchanged_agents);
    retained_summary->SelectAgentTab(running.id);
    summary_host->Show();
    Events();
    frame->OpenSettings();
    Events();
    auto* settings = wxWindow::FindWindowByName("spark.settings", frame);
    auto* language = dynamic_cast<NativeChoice*>(wxWindow::FindWindowByName("spark.language", frame));
    Check(settings && settings->IsShownOnScreen() && language,
          "localized General settings open without leaving the page");
    if (settings && language) {
        const auto locales = i18n::SupportedLocales();
        for (size_t index = 0; index < locales.size(); ++index) {
            language->SetSelection(static_cast<int>(index + 1));
            wxCommandEvent choose(wxEVT_CHOICE, language->GetId());
            choose.SetEventObject(language); choose.SetInt(static_cast<int>(index + 1));
            language->GetEventHandler()->ProcessEvent(choose);
            Events();
            Check(i18n::CurrentLocale() == locales[index].code,
                  "changing the language applies the chosen catalog");
            Check(settings->IsShownOnScreen(), "changing language keeps settings open");
            Check(work.Contains(frame->GetScreenRect()),
                  "language changes preserve on-screen frame bounds");
            if (summary) {
                CheckText(summary, L"Subagents");
                CheckText(summary, L"Sources");
            }
            retained_summary->SetAgents(unchanged_agents);
            for (const auto& agent : unchanged_agents) {
                retained_summary->SelectAgentTab(agent.id);
                Events();
                auto* back = wxWindow::FindWindowByName("spark.summary.back", retained_summary);
                auto* detail = back ? back->GetParent() : nullptr;
                const wxString status(i18n::Text(
                    agent.state == SubAgentState::Running ? L"Running" : L"Completed"));
                Check(detail && detail->GetToolTipText().Contains(status),
                      "unchanged retained subagent statuses follow the new language");
                Check(back && back->GetToolTipText() == wxString(i18n::Text(L"Back")),
                      "summary navigation help relocalizes");
            }
            auto* title = wxWindow::FindWindowByName("spark.settings.title", settings);
            Check(title && title->GetLabel() == wxString(i18n::Text(L"General")),
                  "General settings title relocalizes");
            Check(language->GetString(0) == wxString(i18n::Text(L"Automatic")),
                  "automatic language label relocalizes");
            auto* browse = wxWindow::FindWindowByName("spark.locale:Choose", settings);
            Check(browse && browse->GetToolTipText() == wxString(i18n::Text(L"Choose the default task folder")),
                  "General settings help relocalizes");
            Check(language->IsShownOnScreen() && language->GetSize().y >= language->FromDIP(30) &&
                      browse && browse->IsShownOnScreen(),
                  "essential General controls remain visible at the current DPI");
            const auto saved = LoadConfig(config.config_path, false);
            auto expected = initial;
            expected["ui"]["language"] = locales[index].code;
            Check(saved.ok && ConfigToJson(saved.value) == expected,
                  "language changes preserve populated provider, permission, MCP and extension settings");
            if (locales[index].code == "ko-KR") Capture(frame, "korean-settings");
        }
    }
    summary_host->Destroy();
    frame->Destroy();
    Events();
    wxTheApp->OnExit();
    wxEntryCleanup();
    std::error_code error;
    fs::remove_all(temporary, error);
    std::cout << "Native localization failures: " << failures << '\n';
    return failures ? 1 : 0;
}
