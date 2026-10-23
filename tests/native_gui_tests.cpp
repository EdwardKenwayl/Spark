#include "gui/native/app.h"
#include "gui/localization.h"
#include "gui/native/provider_list.h"
#include "gui/native/provider_editor.h"
#include "gui/native/archive_list.h"
#include "gui/native/settings.h"
#include "gui/native/popup_menu.h"
#include "gui/native/add_menu.h"
#include "gui/native/attachment_picker.h"
#include "gui/native/model_picker.h"
#include "gui/native/chat_usage.h"
#include "gui/native/project_dialog.h"
#include "gui/native/text_prompt.h"
#include "gui/native/approval_prompt.h"
#include "gui/native/summary_panel.h"
#include "gui/native/summary_popup.h"

#include <wx/app.h>
#include <wx/choice.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/filedlg.h>
#include <wx/image.h>
#include <wx/simplebook.h>
#include <wx/srchctrl.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <algorithm>
#include <fstream>
#include <set>
#include <iostream>
#include <iterator>

class SmokeApplication : public wxApp {
public:
    bool OnInit() override { wxInitAllImageHandlers(); return true; }
};
wxIMPLEMENT_APP_NO_MAIN(SmokeApplication);
namespace {
int failures = 0;
void Check(bool condition, const char* text) {
    if (!condition) { ++failures; std::cerr << text << '\n'; }
}
void Events() {
    for (int count = 0; count < 5; ++count) { wxYield(); wxMilliSleep(20); }
}
void Capture(wxWindow* window, const spark::fs::path& path, wxSize minimum = wxSize(700, 400)) {
    wxBitmap bitmap(window->GetClientSize());
    wxMemoryDC output(bitmap);
    wxClientDC input(window);
    output.Blit(0, 0, bitmap.GetWidth(), bitmap.GetHeight(), &input, 0, 0);
    output.SelectObject(wxNullBitmap);
    const auto image = bitmap.ConvertToImage();
    Check(image.IsOk() && image.GetWidth() >= minimum.x && image.GetHeight() >= minimum.y,
          "native window has a usable image");
    if (image.IsOk()) {
        std::set<unsigned> colors;
        const auto* pixels = image.GetData();
        const size_t count = static_cast<size_t>(image.GetWidth()) * image.GetHeight();
        for (size_t index = 0; index < count && colors.size() < 16; index += 13)
            colors.insert((pixels[index * 3] << 16) | (pixels[index * 3 + 1] << 8) | pixels[index * 3 + 2]);
        Check(colors.size() >= 6, "native screenshot contains rendered controls and text");
    }
    Check(image.SaveFile(wxString::FromUTF8(spark::PathToUtf8(path)), wxBITMAP_TYPE_PNG), "native screenshot saved");
}
}  // namespace
int main(int argc, char** argv) {
    using namespace spark;
    wxSetAssertHandler([](const wxString&, int line, const wxString& function,
                         const wxString& condition, const wxString& message) {
        ++failures;
        std::cerr << "wx assertion " << function << ':' << line << ' ' << condition << ' ' << message << '\n';
    });
    if (!wxEntryStart(argc, argv) || !wxTheApp->CallOnInit()) return 2;
    const auto temporary = fs::temp_directory_path() / NativePath("spark-gui-" + NewSessionId());
    fs::create_directories(temporary);
    const auto output_root = argc > 1 ? NativePath(AbsolutePath(argv[1])) : temporary / "screenshots";
    const auto output = output_root / NativePath("run-" + NewSessionId());
    fs::create_directories(output);
    std::cout << "Screenshot run: " << PathToUtf8(output) << std::endl;
    AppConfig config;
    config.config_path = PathToUtf8(temporary / "config.json");
    config.storage.data_dir = PathToUtf8(temporary / "data");
    config.workspace.path = PathToUtf8(temporary / "outputs");
    config.ui.language = "en-US";
    config.ui.theme = "dark";
    ProviderConfig provider;
    provider.id = "fixture";
    provider.label = "Fixture provider";
    provider.base_url = "http://127.0.0.1:1";
    for (int index = 0; index < 30; ++index) {
        ModelConfig model;
        model.id = "test-model-" + std::to_string(index);
        model.capabilities.reasoning_efforts = {"low", "medium", "high"};
        model.capabilities.default_reasoning_effort = "medium";
        provider.models.push_back(std::move(model));
    }
    config.providers.push_back(std::move(provider));
    config.active = {"fixture", "test-model-0", "medium"};
    if (argc > 3 && std::string(argv[2]) == "--settings-smoke") {
        const auto existing = LoadConfig(argv[3], false);
        Check(existing.ok, "settings fixture loads the supplied configuration read-only");
        if (!existing.ok) return 2;
        config = existing.value;
        config.config_path = PathToUtf8(temporary / "config.json");
        config.storage.data_dir = PathToUtf8(temporary / "data");
        config.workspace.path = PathToUtf8(temporary / "outputs");
        config.mcp_servers.clear();
    }
    Check(SaveConfigAtomically(config).ok, "isolated GUI config is created");
    fs::create_directories(temporary / "data" / "cache");
    fs::create_directories(temporary / "data" / "sessions");
    fs::create_directories(temporary / "data" / "archive");
    std::ofstream(temporary / "data" / "cache" / "stale.bin") << "stale";
    std::ofstream(temporary / "data" / "sessions" / "keep.jsonl") << "session";
    std::ofstream(temporary / "data" / "archive" / "keep.jsonl") << "archive";
    std::cout << "Constructing isolated frame" << std::endl;
    auto* frame = new NativeFrame(config);
    frame->SetSize(900, 650);
    std::cout << "Showing isolated frame" << std::endl;
    frame->Show();
    Events();
    if (argc > 2 && std::string(argv[2]) == "--settings-smoke") {
        auto* composer = dynamic_cast<wxTextCtrl*>(wxWindow::FindWindowByName("spark.composer", frame));
        Check(composer && (composer->GetWindowStyleFlag() & wxTE_NO_VSCROLL),
              "composer does not add a platform scrollbar");
        Check(composer && composer->GetBackgroundColour() == composer->GetParent()->GetBackgroundColour(),
              "composer has the same flat surface as the Windows input");
        std::cout << "Opening settings" << std::endl;
        frame->OpenSettings();
        Events();
        auto* settings = wxWindow::FindWindowByName("spark.settings", frame);
        Check(settings && settings->IsShown(), "settings opens without closing the app");
        auto* archives = dynamic_cast<NativeArchiveList*>(
            wxWindow::FindWindowByName("spark.archives.list", settings));
        Check(archives && archives->GetItemCount() == 0 && archives->GetVisibleRowsEnd() == 0,
              "empty archive lists have a bounded initialized visible range before focus");
        if (archives) {
            wxFocusEvent focus(wxEVT_SET_FOCUS, archives->GetId());
            archives->GetEventHandler()->ProcessEvent(focus);
        }
        auto* theme = dynamic_cast<NativeChoice*>(wxWindow::FindWindowByName("spark.theme", settings));
        Check(theme != nullptr, "settings theme control exists");
        if (theme) {
            for (int selection : {1, 0, 2}) {
                theme->SetSelection(selection);
                wxCommandEvent change(wxEVT_CHOICE, theme->GetId());
                change.SetEventObject(theme);
                theme->GetEventHandler()->ProcessEvent(change);
                Events();
                Check(settings->IsShown(), "saving appearance keeps settings open");
            }
        }
        for (const auto* name : {"spark.locale:Provider configuration",
                                 "spark.locale:Archived chats", "spark.locale:General"}) {
            std::cout << "Selecting " << name << std::endl;
            auto* button = wxWindow::FindWindowByName(name, settings);
            Check(button != nullptr, "settings navigation exists");
            if (button) {
                wxCommandEvent click(wxEVT_BUTTON, button->GetId());
                button->GetEventHandler()->ProcessEvent(click);
                Events();
            }
        }
        auto* back = wxWindow::FindWindowByName("spark.locale:Back to app", settings);
        Check(back != nullptr, "settings has a return action");
        if (back) {
            wxCommandEvent click(wxEVT_BUTTON, back->GetId());
            back->GetEventHandler()->ProcessEvent(click);
            Events();
        }
        auto* open = wxWindow::FindWindowByName("spark.sidebar.settings", frame);
        Check(open != nullptr, "sidebar settings action exists");
        if (open) {
            wxCommandEvent click(wxEVT_BUTTON, open->GetId());
            click.SetEventObject(open);
            open->GetEventHandler()->ProcessEvent(click);
            Events();
            Check(settings->IsShown(), "settings can be reopened from its sidebar button");
        }
        auto* send = wxWindow::FindWindowByName("spark.send", frame);
        Check(send && send->GetToolTipText() == wxString(i18n::Text(L"Send")) && !send->IsEnabled(),
              "an idle empty conversation is not running");
        frame->Destroy();
        Events();
        wxTheApp->OnExit();
        wxEntryCleanup();
        fs::remove_all(temporary);
        std::cout << "Settings smoke failures: " << failures << std::endl;
        return failures == 0 ? 0 : 1;
    }
    for (const auto decision : {NativeApprovalDecision::Once, NativeApprovalDecision::Deny,
                                 NativeApprovalDecision::Turn}) {
        NativeApprovalPrompt prompt(frame, {ApprovalKind::FileWrite, "Write fixture?", "  fixture.txt  "});
        wxTimer choose;
        choose.Bind(wxEVT_TIMER, [&](wxTimerEvent&) {
            choose.Stop();
            const int id = decision == NativeApprovalDecision::Once ? wxID_YES :
                decision == NativeApprovalDecision::Turn ? wxID_CANCEL : wxID_NO;
            wxCommandEvent click(wxEVT_BUTTON, id);
            prompt.GetEventHandler()->ProcessEvent(click);
        });
        choose.StartOnce(50);
        prompt.ShowModal();
        Check(prompt.Decision() == decision, "approval returns only the explicit selected permission");
    }
    {
        NativeApprovalPrompt prompt(frame, {ApprovalKind::FileWrite, "Write fixture?", std::string(5000, 'x')});
        auto* message = wxWindow::FindWindowByName("spark.approval.message", &prompt);
        Check(message && message->GetLabel().Contains("application-level approval") &&
                  !message->GetLabel().Contains(wxString(4001, 'x')),
              "approval retains the Windows warning and bounds its details");
        wxTimer close;
        close.Bind(wxEVT_TIMER, [&](wxTimerEvent&) { close.Stop(); prompt.Close(); });
        close.StartOnce(50);
        prompt.ShowModal();
        Check(prompt.Decision() == NativeApprovalDecision::Deny, "closing approval cannot grant a turn");
    }
    {
        NativeApprovalPrompt prompt(frame, {ApprovalKind::FileWrite, "Write fixture?", "fixture.txt"});
        wxTimer escape;
        escape.Bind(wxEVT_TIMER, [&](wxTimerEvent&) {
            escape.Stop();
            wxKeyEvent key(wxEVT_CHAR_HOOK);
            key.m_keyCode = WXK_ESCAPE;
            prompt.GetEventHandler()->ProcessEvent(key);
        });
        escape.StartOnce(50);
        prompt.ShowModal();
        Check(prompt.Decision() == NativeApprovalDecision::Deny, "Escape denies approval");
    }
    Capture(frame, output / "chat.png");
    auto* sidebar = wxWindow::FindWindowByName("spark.sidebar", frame);
    auto* input_panel = wxWindow::FindWindowByName("spark.input", frame);
    auto* composer = wxWindow::FindWindowByName("spark.composer", frame);
    const int sidebar_width = std::min(frame->FromDIP(300), std::max(frame->FromDIP(190),
        std::min(frame->FromDIP(236), frame->GetClientSize().x * 24 / 100)));
    Check(sidebar && sidebar->GetSize().x == sidebar_width, "sidebar matches the Windows responsive width");
    Check(input_panel && composer && composer->GetParent() == input_panel,
          "input and toolbar share a single composer surface");
    Check(input_panel && input_panel->GetSize().x <= frame->FromDIP(900), "composer width is constrained");
    auto* context = wxWindow::FindWindowByName("spark.context", frame);
    auto* summary = wxWindow::FindWindowByName("spark.summary", frame);
    if (context && summary) {
        context->SetLabel("1000000 / 1000000");
        Check(context->GetScreenRect().GetRight() < summary->GetScreenRect().GetLeft(),
              "growing context counters do not overlap the summary button");
    }
    auto* history = dynamic_cast<NativeList*>(wxWindow::FindWindowByName("spark.history", frame));
    Check(history && history->GetSelection() == wxNOT_FOUND, "an empty chat does not select the Recent heading");
    auto* permissions = dynamic_cast<NativeChoice*>(wxWindow::FindWindowByName("spark.permissions", frame));
    Check(permissions != nullptr, "conversation permission selector exists");
    if (permissions) {
        permissions->SetSelection(1);
        wxCommandEvent change(wxEVT_CHOICE, permissions->GetId());
        change.SetEventObject(permissions);
        permissions->GetEventHandler()->ProcessEvent(change);
        const auto loaded = LoadConfig(config.config_path, false);
        Check(loaded.ok && loaded.value.permissions.default_mode == "approve_for_me",
              "conversation permission changes persist in the isolated configuration");
        wxTimer choose;
        choose.Bind(wxEVT_TIMER, [&](wxTimerEvent&) {
            auto* canvas = wxWindow::FindWindowByName("spark.menu.canvas");
            if (!canvas || !canvas->IsShownOnScreen()) return;
            choose.Stop();
            Check(canvas->GetScreenRect().GetBottom() < permissions->GetScreenPosition().y,
                  "composer permission menu opens above its button");
            Capture(canvas->GetParent(), output / "permission-popup.png", wxSize(100, 60));
            for (const int code : {WXK_DOWN, WXK_RETURN}) {
                wxKeyEvent key(wxEVT_KEY_DOWN);
                key.m_keyCode = code; key.SetEventObject(canvas);
                canvas->GetEventHandler()->ProcessEvent(key);
            }
        });
        choose.Start(50);
        permissions->Open();
        choose.Stop();
        const auto changed = LoadConfig(config.config_path, false);
        Check(permissions->GetSelection() == 0 && changed.ok &&
              changed.value.permissions.default_mode == "ask",
              "shared choice popup dispatches its selection and persists behavior");
        wxTimer dismiss;
        dismiss.Bind(wxEVT_TIMER, [&](wxTimerEvent&) {
            auto* canvas = wxWindow::FindWindowByName("spark.menu.canvas");
            if (!canvas || !canvas->IsShownOnScreen()) return;
            dismiss.Stop();
            wxKeyEvent key(wxEVT_KEY_DOWN);
            key.m_keyCode = WXK_ESCAPE; key.SetEventObject(canvas);
            canvas->GetEventHandler()->ProcessEvent(key);
        });
        dismiss.Start(50);
        permissions->Open();
        dismiss.Stop();
        Check(permissions->GetSelection() == 0, "dismissing a choice popup preserves its selection");
    }
    auto* models = dynamic_cast<NativeButton*>(wxWindow::FindWindowByName("spark.model", frame));
    Check(models && models->GetLabel() == "test-model-0", "model and effort use one combined button");
    if (models) {
        wxTimer driver;
        int stage = 0;
        driver.Bind(wxEVT_TIMER, [&](wxTimerEvent&) {
            auto* canvas = wxWindow::FindWindowByName(
                stage == 0 ? "spark.model-picker.canvas" : "spark.menu.canvas");
            if (!canvas || !canvas->IsShownOnScreen()) return;
            const auto key = [&](int code) {
                wxKeyEvent event(wxEVT_KEY_DOWN);
                event.m_keyCode = code; event.SetEventObject(canvas);
                canvas->GetEventHandler()->ProcessEvent(event);
            };
            if (stage++ == 0) {
                Check(canvas->GetScreenRect().GetHeight() <= 1080,
                      "large model popup is bounded by the display");
                Check(canvas->GetSize().x == canvas->FromDIP(380) &&
                          canvas->GetSize().y == canvas->FromDIP(432),
                      "large model picker uses twelve 32-DIP rows and a fixed reasoning footer");
                Check(canvas->GetScreenRect().GetBottom() < models->GetScreenPosition().y,
                      "large model picker opens above its composer button");
                Capture(canvas->GetParent(), output / "composer-model-picker.png", wxSize(160, 180));
                key(WXK_END);
                Capture(canvas->GetParent(), output / "composer-model-picker-last.png", wxSize(160, 180));
                key(WXK_RETURN);
            } else { key(WXK_ESCAPE); driver.Stop(); }
        });
        driver.Start(50);
        wxCommandEvent click(wxEVT_BUTTON, models->GetId());
        click.SetEventObject(models);
        models->GetEventHandler()->ProcessEvent(click);
        driver.Stop();
        const auto selected = LoadConfig(config.config_path, false);
        Check(selected.ok && selected.value.active.model == "test-model-29",
              "the last model remains keyboard-selectable beyond the first popup page");
        Check(models->GetLabel() == "test-model-29", "combined model button refreshes after selection");
        std::vector<NativeModelPickerRow> picker_rows{
            {1, "Fixture provider", NativeModelPickerKind::Header}};
        for (int index = 0; index < 30; ++index)
            picker_rows.push_back({100 + index, "test-model-" + std::to_string(index), NativeModelPickerKind::Model});
        picker_rows.push_back({30000, "Reasoning effort", NativeModelPickerKind::Reasoning});
        wxTimer picker_driver;
        picker_driver.Bind(wxEVT_TIMER, [&](wxTimerEvent&) {
            auto* canvas = wxWindow::FindWindowByName("spark.model-picker.canvas");
            if (!canvas || !canvas->IsShownOnScreen()) return;
            picker_driver.Stop();
            const auto mouse = [&](wxEventType type, const wxPoint& point) {
                wxMouseEvent event(type);
                event.SetPosition(point);
                event.SetEventObject(canvas);
                canvas->GetEventHandler()->ProcessEvent(event);
            };
            const int track = canvas->FromDIP(32) * 12;
            const int thumb = std::max(canvas->FromDIP(24), track * 12 / 31);
            const int x = canvas->GetClientSize().x - canvas->FromDIP(8);
            const int y = canvas->FromDIP(4) + thumb / 2;
            mouse(wxEVT_LEFT_DOWN, {x, y});
            mouse(wxEVT_MOTION, {x, y + track - thumb});
            mouse(wxEVT_LEFT_UP, {x, y + track - thumb});
            const wxPoint last(canvas->FromDIP(60), canvas->FromDIP(4) + track - canvas->FromDIP(16));
            mouse(wxEVT_LEFT_DOWN, last);
            mouse(wxEVT_LEFT_UP, last);
        });
        picker_driver.Start(50);
        Check(ShowNativeModelPicker(frame, models->ClientToScreen({0, 0}), picker_rows, 100) == 129,
              "dragging the model picker scrollbar reaches and selects the final model");
        picker_driver.Stop();
        wxTimer footer_driver;
        footer_driver.Bind(wxEVT_TIMER, [&](wxTimerEvent&) {
            auto* canvas = wxWindow::FindWindowByName("spark.model-picker.canvas");
            if (!canvas || !canvas->IsShownOnScreen()) return;
            footer_driver.Stop();
            wxKeyEvent end(wxEVT_KEY_DOWN);
            end.m_keyCode = WXK_END; end.SetEventObject(canvas);
            canvas->GetEventHandler()->ProcessEvent(end);
            wxMouseEvent wheel(wxEVT_MOUSEWHEEL);
            wheel.SetEventObject(canvas);
            wheel.m_wheelDelta = 120; wheel.m_wheelRotation = 120; wheel.m_linesPerAction = 3;
            canvas->GetEventHandler()->ProcessEvent(wheel);
            const wxPoint footer(canvas->FromDIP(60), canvas->GetClientSize().y - canvas->FromDIP(20));
            for (const auto type : {wxEVT_LEFT_DOWN, wxEVT_LEFT_UP}) {
                wxMouseEvent click(type);
                click.SetPosition(footer); click.SetEventObject(canvas);
                canvas->GetEventHandler()->ProcessEvent(click);
            }
        });
        footer_driver.Start(50);
        Check(ShowNativeModelPicker(frame, models->ClientToScreen({0, 0}), picker_rows, 100) == 30000,
              "reasoning footer remains clickable while the model rows scroll");
        footer_driver.Stop();
    }
    Transcript transcript;
    const TranscriptTime now{1700000000, 1000};
    ChatItem usage;
    usage.kind = ChatItemKind::UsageStats;
    usage.input_tokens = 12345; usage.output_tokens = 6789; usage.cached_input_tokens = 9876;
    usage.context_tokens = 244800; usage.context_limit_tokens = 272000; usage.model_calls = 4;
    usage.elapsed_ms = 1500;
    Check(NativeUsageText(usage) == wxString::FromUTF8(
        "Turn total 19,134 \xc2\xb7 Input 12,345 \xc2\xb7 Output 6,789 \xc2\xb7 Cache read 9,876 "
        "\xc2\xb7 Cache hit 80.0% \xc2\xb7 Context 244,800/272,000 (90.0%) \xc2\xb7 Calls 4 \xc2\xb7 Elapsed 1.5s"),
        "usage formatting includes the same complete per-turn fields as Windows");
    Check(NativeUsageText(ChatItem{}).Contains("Cache hit 0.0%") &&
              NativeUsageText(ChatItem{}).Contains("Context 0/0 (0.0%)"),
          "zero-input usage avoids undefined percentage values");
    const auto add = [&](ChatItemKind kind, const std::string& text) {
        ChatItem item; item.kind = kind; item.text = text;
        return transcript.Apply({TranscriptEventKind::Add, kNoChatItem, item}, now);
    };
    add(ChatItemKind::User, "Inspect the project and summarize the result.");
    const auto status = add(ChatItemKind::TurnStatus, "");
    add(ChatItemKind::Assistant, "I will read the project.");
    ChatItem tool; tool.kind = ChatItemKind::Tool; tool.tool_name = "read_file"; tool.arguments = "README.md";
    const auto command = transcript.Apply({TranscriptEventKind::Add, kNoChatItem, tool}, now);
    tool.success = true; tool.result = "A small project."; tool.elapsed_ms = 150;
    transcript.Apply({TranscriptEventKind::Finish, command, tool}, {1700000001, 1150});
    const auto reply = add(ChatItemKind::Assistant,
        "**Result**\n\nThe project contains `src` and `tests`.\n\n"
        "- Changes stay inside the project.\n- The command completed successfully.\n\n"
        "```cpp\nint main() { return 0; }\n```");
    transcript.Apply({TranscriptEventKind::Finish, reply, {}}, {1700000002, 2000});
    ChatItem completed; completed.elapsed_ms = 1000;
    transcript.Apply({TranscriptEventKind::FinishTurn, status, completed}, {1700000002, 2000});
    auto* surface = dynamic_cast<NativeChatSurface*>(wxWindow::FindWindowByName("spark.transcript", frame));
    Check(surface != nullptr, "native transcript control exists");
    if (surface) {
        surface->SetTranscript(&transcript, true);
        Events();
        Capture(frame, output / "reply-dark.png");
        transcript.ToggleTurnStatus(status);
        surface->Changed();
        Events();
        Capture(frame, output / "tools-dark.png");
        const int transcript_height = surface->GetVirtualSize().y;
        frame->SetSize(980, 650);
        Events();
        frame->SetSize(900, 650);
        Events();
        Check(surface->GetVirtualSize().y == transcript_height,
              "resizing does not append completed Markdown to the transcript again");
    }
    frame->OpenSettings();
    Events();
    auto* settings = wxWindow::FindWindowByName("spark.settings", frame);
    auto* chat = wxWindow::FindWindowByName("spark.chat", frame);
    Check(settings && settings->IsShown() && chat && !chat->IsShown(), "settings replaces the chat page");
    auto* theme = dynamic_cast<NativeChoice*>(wxWindow::FindWindowByName("spark.theme", frame));
    Check(theme != nullptr, "theme setting exists");
    auto* workspace = dynamic_cast<wxTextCtrl*>(wxWindow::FindWindowByName("spark.workspace.path", settings));
    auto* data_path = wxWindow::FindWindowByName("spark.data.path", settings);
    auto* preferences_card = wxWindow::FindWindowByName("spark.settings.card.preferences", settings);
    auto* language = wxWindow::FindWindowByName("spark.language", settings);
    auto* shell = dynamic_cast<NativeChoice*>(wxWindow::FindWindowByName("spark.shell", settings));
    Check(shell && shell->GetStringSelection() == wxString::FromUTF8(config.ui.shell),
          "shell uses the same painted selector as the other preferences and preserves its configured value");
    Check(workspace && workspace->GetValue().ToStdString(wxConvUTF8) ==
              AbsolutePath(config.workspace.path),
          "workspace path is displayed as an absolute path");
    Check(data_path && data_path->GetLabel().ToStdString(wxConvUTF8) == AbsolutePath(config.storage.data_dir),
          "data path is displayed as an absolute path");
    Check(preferences_card && language && language->GetParent() == preferences_card,
          "preference controls are children of the preferences card");
    auto* preferences_label = wxWindow::FindWindowByName("spark.locale:Preferences", settings);
    if (theme) {
        theme->SetSelection(1);
        wxCommandEvent change(wxEVT_CHOICE, theme->GetId());
        change.SetEventObject(theme);
        theme->GetEventHandler()->ProcessEvent(change);
        Events();
        const auto loaded = LoadConfig(config.config_path, false);
        Check(loaded.ok && loaded.value.ui.theme == "light", "changing theme saves its behavior");
        Check(settings->IsShown() && !chat->IsShown(), "autosave keeps the settings page open");
    }
    frame->SetSize(1400, 900);
    Events();
    Check(preferences_label && language && language->GetScreenRect().y == preferences_label->GetScreenRect().y,
          "wide settings align preference labels and controls on the same row");
    auto* workspace_card = wxWindow::FindWindowByName("spark.settings.card.workspace", settings);
    Check(workspace_card && settings->ScreenToClient(workspace_card->GetScreenPosition()).y == frame->FromDIP(82),
          "general cards start at the Windows 82-DIP content origin");
    Capture(frame, output / "settings-light-wide.png");
    frame->SetSize(760, 520);
    Events();
    Check(preferences_label && language &&
              language->GetScreenRect().y == preferences_label->GetScreenRect().y + frame->FromDIP(34),
          "compact preferences place controls 34 DIP below the label");
    auto* clear_cache = dynamic_cast<NativeButton*>(wxWindow::FindWindowByName("spark.locale:Clear cache", settings));
    Check(clear_cache != nullptr, "clear-cache action exists on the data card");
    if (clear_cache) {
        wxCommandEvent click(wxEVT_BUTTON, clear_cache->GetId());
        click.SetEventObject(clear_cache);
        clear_cache->GetEventHandler()->ProcessEvent(click);
        Events();
        Check(!fs::exists(temporary / "data" / "cache"), "cache cleanup removes only the cache directory");
        Check(fs::exists(temporary / "data" / "sessions" / "keep.jsonl"),
              "cache cleanup keeps session data");
        Check(fs::exists(temporary / "data" / "archive" / "keep.jsonl"),
              "cache cleanup keeps archived chats");
    }
    auto* provider_navigation = dynamic_cast<NativeButton*>(
        wxWindow::FindWindowByName("spark.locale:Provider configuration", settings));
    if (provider_navigation) {
        wxCommandEvent open_providers(wxEVT_BUTTON, provider_navigation->GetId());
        open_providers.SetEventObject(provider_navigation);
        provider_navigation->GetEventHandler()->ProcessEvent(open_providers);
        Events();
        auto* add_provider = dynamic_cast<NativeButton*>(
            wxWindow::FindWindowByName("spark.locale:Add provider", settings));
        Check(add_provider && add_provider->IsShownOnScreen(),
              "provider list remains inside the settings page");
        if (add_provider) {
            wxCommandEvent add(wxEVT_BUTTON, add_provider->GetId());
            add.SetEventObject(add_provider);
            add_provider->GetEventHandler()->ProcessEvent(add);
            Events();
            auto* provider_pages = dynamic_cast<wxSimplebook*>(
                wxWindow::FindWindowByName("spark.provider.pages", settings));
            auto* provider_editor = wxWindow::FindWindowByName("spark.provider.editor", settings);
            auto* editor_back = dynamic_cast<NativeButton*>(
                wxWindow::FindWindowByName("spark.provider.editor.back", settings));
            Check(provider_pages && provider_pages->GetSelection() == 1 &&
                      provider_editor && provider_editor->IsShownOnScreen(),
                  "adding a provider opens an embedded editor page");
            Check(editor_back && wxWindow::FindWindowByName("spark.provider.editor.save", settings),
                  "embedded provider editor exposes in-page back and save actions");
            auto* protocol_chat = dynamic_cast<NativeButton*>(
                wxWindow::FindWindowByName("spark.provider.editor.protocol.chat_completions", settings));
            auto* protocol_responses = dynamic_cast<NativeButton*>(
                wxWindow::FindWindowByName("spark.provider.editor.protocol.responses", settings));
            auto* protocol_anthropic = dynamic_cast<NativeButton*>(
                wxWindow::FindWindowByName("spark.provider.editor.protocol.anthropic_messages", settings));
            auto* name_label = wxWindow::FindWindowByName("spark.provider.editor.label.name", settings);
            auto* protocol_label = wxWindow::FindWindowByName("spark.provider.editor.label.protocol", settings);
            auto* url_label = wxWindow::FindWindowByName("spark.provider.editor.label.url", settings);
            auto* key_label = wxWindow::FindWindowByName("spark.provider.editor.label.key", settings);
            auto* test_model = dynamic_cast<wxTextCtrl*>(
                wxWindow::FindWindowByName("spark.provider.editor.test-model", settings));
            auto* base_url = dynamic_cast<wxTextCtrl*>(
                wxWindow::FindWindowByName("spark.provider.editor.url", settings));
            auto* first_model = dynamic_cast<wxTextCtrl*>(
                wxWindow::FindWindowByName("spark.provider.editor.model.0.id", settings));
            Check(protocol_chat && protocol_responses && protocol_anthropic,
                  "provider editor exposes all three upstream protocol buttons");
            Check(test_model, "provider editor exposes the test-model field");
            Check(base_url && first_model, "provider editor exposes base URL and first model fields");
            if (base_url && first_model) {
                base_url->ChangeValue("https://fixture.example/v1");
                first_model->ChangeValue("probe-model");
                wxCommandEvent url_changed(wxEVT_TEXT, base_url->GetId());
                url_changed.SetEventObject(base_url);
                base_url->GetEventHandler()->ProcessEvent(url_changed);
                wxCommandEvent model_id_changed(wxEVT_TEXT, first_model->GetId());
                model_id_changed.SetEventObject(first_model);
                first_model->GetEventHandler()->ProcessEvent(model_id_changed);
            }
            if (protocol_chat && protocol_responses && protocol_anthropic) {
                Check(static_cast<int>(protocol_chat->IsSelected()) +
                          static_cast<int>(protocol_responses->IsSelected()) +
                          static_cast<int>(protocol_anthropic->IsSelected()) == 1,
                      "provider editor shows exactly one selected protocol");
                wxCommandEvent choose_responses(wxEVT_BUTTON, protocol_responses->GetId());
                choose_responses.SetEventObject(protocol_responses);
                protocol_responses->GetEventHandler()->ProcessEvent(choose_responses);
                Check(protocol_responses->IsSelected() && !protocol_chat->IsSelected() &&
                          !protocol_anthropic->IsSelected(),
                      "choosing a protocol updates the exclusive selected state");
                auto* preview = dynamic_cast<wxTextCtrl*>(
                    wxWindow::FindWindowByName("spark.provider.editor.preview", settings));
                Check(preview, "provider editor preview remains available");
                if (preview) {
                    for (int attempt = 0; attempt < 4; ++attempt) Events();
                    const auto document = ParseConfigJson(preview->GetValue().ToStdString(wxConvUTF8));
                    Check(document.is_object() && document.contains("providers") &&
                              document["providers"].is_object() &&
                              !document["providers"].empty() &&
                              document["providers"].begin().value().value("protocol", "") == "responses",
                          "provider preview serializes the selected protocol");
                    if (test_model) {
                        test_model->ChangeValue("probe-model");
                        wxCommandEvent model_changed(wxEVT_TEXT, test_model->GetId());
                        model_changed.SetEventObject(test_model);
                        test_model->GetEventHandler()->ProcessEvent(model_changed);
                        for (int attempt = 0; attempt < 4; ++attempt) Events();
                        const auto updated = ParseConfigJson(preview->GetValue().ToStdString(wxConvUTF8));
                        Check(updated["providers"].begin().value().value("test_model", "") == "probe-model",
                              "provider preview serializes the edited test model");
                    }
                }
                wxCommandEvent choose_anthropic(wxEVT_BUTTON, protocol_anthropic->GetId());
                choose_anthropic.SetEventObject(protocol_anthropic);
                protocol_anthropic->GetEventHandler()->ProcessEvent(choose_anthropic);
                Check(protocol_anthropic->IsSelected() && !protocol_chat->IsSelected() &&
                          !protocol_responses->IsSelected(),
                      "switching protocol clears the previous selection");
            }
            frame->SetSize(1400, 900);
            Events();
            Check(name_label && protocol_label && url_label && key_label &&
                      name_label->GetScreenRect().y == protocol_label->GetScreenRect().y &&
                      url_label->GetScreenRect().y == key_label->GetScreenRect().y,
                  "wide provider fields use the Windows two-column field groups");
            frame->SetSize(620, 650);
            Events();
            Check(name_label && protocol_label && url_label && key_label &&
                      name_label->GetScreenRect().y < protocol_label->GetScreenRect().y &&
                      protocol_label->GetScreenRect().y < url_label->GetScreenRect().y &&
                      url_label->GetScreenRect().y < key_label->GetScreenRect().y,
                  "compact provider fields stack into the Windows single-column layout");
            if (protocol_chat && protocol_responses && protocol_anthropic)
                Check(protocol_anthropic->GetScreenRect().y > protocol_chat->GetScreenRect().y,
                      "compact protocol buttons wrap to additional rows");
            frame->SetSize(900, 650);
            Events();
            Capture(frame, output / "settings-provider-editor.png");
            if (editor_back) {
                wxCommandEvent back_to_list(wxEVT_BUTTON, editor_back->GetId());
                back_to_list.SetEventObject(editor_back);
                editor_back->GetEventHandler()->ProcessEvent(back_to_list);
                Events();
                Check(provider_pages && provider_pages->GetSelection() == 0 &&
                          settings->IsShown() && !chat->IsShown(),
                      "provider editor back returns to the list without leaving settings");
            }
        }
    } else {
        Check(false, "provider configuration navigation exists");
    }
    Capture(frame, output / "settings-light.png");
    Check(composer && composer->GetSize().x > 100, "composer retains useful width at the minimum window size");
    auto* settings_navigation = wxWindow::FindWindowByName("spark.settings.navigation", frame);
    Check(settings_navigation && settings_navigation->GetSize().x == frame->FromDIP(238),
          "settings uses its Windows 238-DIP navigation width");
    auto* back = dynamic_cast<NativeButton*>(wxWindow::FindWindowByName("spark.locale:Back to app", settings));
    if (back) {
        wxCommandEvent click(wxEVT_BUTTON, back->GetId());
        back->GetEventHandler()->ProcessEvent(click);
        Events();
        auto* send = wxWindow::FindWindowByName("spark.send", frame);
        Check(chat->IsShown() && !settings->IsShown(), "back returns to the existing chat");
        Check(send && send->GetScreenRect().GetRight() < input_panel->GetScreenRect().GetRight(),
              "send button stays inside the composer at minimum width");
        Capture(frame, output / "chat-light-small.png");
        const auto display = wxGetDisplaySize();
        frame->SetSize(std::min(1600, display.x - 40), std::min(900, display.y - 80));
        Events();
        Check(input_panel->GetSize().x == std::min(frame->FromDIP(900), chat->GetClientSize().x - frame->FromDIP(40)),
              "wide windows retain the Windows composer width");
        Capture(frame, output / "chat-light-wide.png");
    }
    auto* provider_host = new wxFrame(frame, wxID_ANY, "provider-list-fixture");
    auto* archive_host = new wxFrame(frame, wxID_ANY, "archive-list-fixture");
    auto* archive_list = new NativeArchiveList(archive_host);
    archive_list->SetLabels("Unarchive", "Delete archived chat");
    archive_list->SetRows({
        {true, "Test", "Chats: 1", {}, 0},
        {false, "Project conversation", "2026-10-22 08:00:00", "/test/project.jsonl", 7},
        {true, "No project", "Chats: 1", {}, 0},
        {false, "Recent conversation", "2026-10-22 09:00:00", "/test/recent.jsonl", 9},
    });
    archive_host->SetSize(900, 650);
    archive_host->Show();
    Events();
    wxRect archive_header, archive_chat;
    archive_header = archive_list->GetItemRect(0);
    archive_chat = archive_list->GetItemRect(1);
    Check(!archive_header.IsEmpty() &&
              archive_header.height == archive_list->FromDIP(42),
          "archive group headers match the Windows height");
    Check(!archive_chat.IsEmpty() &&
              archive_chat.height == archive_list->FromDIP(64),
          "archive chat rows match the Windows height");
    size_t archive_action_index = 0;
    bool archive_restore = false;
    int archive_actions = 0;
    archive_list->SetActionHandler([&](size_t index, bool restore) {
        archive_action_index = index;
        archive_restore = restore;
        ++archive_actions;
    });
    const auto archive_click = [&](wxEventType type, const wxPoint& point) {
        wxMouseEvent event(type);
        event.SetPosition(point);
        event.SetEventObject(archive_list);
        archive_list->GetEventHandler()->ProcessEvent(event);
    };
    const wxPoint unarchive(archive_chat.GetRight() - archive_list->FromDIP(40),
                            archive_chat.y + archive_chat.height / 2);
    archive_click(wxEVT_LEFT_DOWN, unarchive);
    archive_click(wxEVT_LEFT_UP, unarchive);
    Check(archive_actions == 1 && archive_action_index == 7 && archive_restore,
          "inline unarchive action maps to the archive entry, not the grouped row");
    Check(archive_list->GetSelection() == wxNOT_FOUND,
          "archive actions do not create a selected row");
    const auto archive_key = [&](int code) {
        wxKeyEvent event(wxEVT_KEY_DOWN);
        event.m_keyCode = code;
        event.SetEventObject(archive_list);
        archive_list->GetEventHandler()->ProcessEvent(event);
    };
    archive_key(WXK_DOWN);
    archive_key(WXK_RETURN);
    archive_key(WXK_DELETE);
    Check(archive_list->GetSelection() == wxNOT_FOUND && archive_actions == 1,
          "archive keyboard navigation scrolls without selecting or activating a chat");
    const wxPoint unarchive_row_top(unarchive.x, archive_chat.y + archive_list->FromDIP(2));
    archive_click(wxEVT_LEFT_DOWN, unarchive_row_top);
    archive_click(wxEVT_LEFT_UP, unarchive_row_top);
    Check(archive_actions == 2 && archive_restore,
          "unarchive hit target spans the full row height");
    archive_click(wxEVT_LEFT_DCLICK,
                  {archive_chat.x + archive_list->FromDIP(30), unarchive.y});
    Check(archive_actions == 3 && archive_restore, "double-click restores an archived chat");
    archive_click(wxEVT_LEFT_DCLICK,
                  {archive_header.x + archive_list->FromDIP(30), archive_header.y + 20});
    Check(archive_actions == 3, "archive group headings never trigger file actions");
    archive_list->Enable(false);
    archive_click(wxEVT_LEFT_DOWN, unarchive);
    archive_click(wxEVT_LEFT_UP, unarchive);
    Check(archive_actions == 3, "archive actions are disabled while an archive job runs");
    archive_list->Enable();
    Capture(archive_host, output / "settings-archives.png");
    archive_host->Destroy();

    auto archive_config = config;
    const auto archive_data = temporary / "archive-fixture";
    archive_config.storage.data_dir = PathToUtf8(archive_data);
    fs::create_directories(archive_data / "archive");
    const auto project_path = PathToUtf8(temporary / "project");
    std::ofstream(archive_data / "projects.json") << nlohmann::json({
        {"projects", {{{"name", "Test"}, {"workspace_path", project_path}}}}
    }).dump();
    const auto write_archive = [&](const char* filename, const char* title, const char* scope) {
        std::ofstream stream(archive_data / "archive" / filename);
        stream << nlohmann::json({{"type", "session.created"},
            {"payload", {{"title", title}, {"workspace", project_path}}}}).dump() << '\n';
        stream << nlohmann::json({{"type", "session.scope"}, {"payload", {{"scope", scope}}}}).dump() << '\n';
    };
    write_archive("project.jsonl", "Project chat", "project");
    write_archive("recent.jsonl", "Recent chat", "recent");
    auto* archive_settings_host = new wxFrame(frame, wxID_ANY, "archive-settings-fixture");
    auto* archive_settings = new NativeSettings(archive_settings_host, archive_config, {}, {});
    archive_settings_host->SetSize(1100, 750);
    archive_settings_host->Show();
    auto* archive_navigation = wxWindow::FindWindowByName("spark.locale:Archived chats", archive_settings);
    Check(archive_navigation, "archive settings navigation exists");
    if (archive_navigation) {
        wxCommandEvent open_archives(wxEVT_BUTTON, archive_navigation->GetId());
        archive_navigation->GetEventHandler()->ProcessEvent(open_archives);
        auto* list = dynamic_cast<NativeArchiveList*>(
            wxWindow::FindWindowByName("spark.archives.list", archive_settings));
        for (int attempt = 0; attempt < 20 && list && !list->IsEnabled(); ++attempt) Events();
        auto* scope = dynamic_cast<NativeChoice*>(
            wxWindow::FindWindowByName("spark.archives.scope", archive_settings));
        auto* search = dynamic_cast<wxSearchCtrl*>(
            wxWindow::FindWindowByName("spark.archives.search", archive_settings));
        Check(list && list->GetItemCount() == 4, "archives group a project and explicit recent chat separately");
        Check(scope && search && scope->GetScreenRect().y == search->GetScreenRect().y,
              "archive search and project filter share the Windows toolbar row");
        Check(wxWindow::FindWindowByName("spark.archives.delete-all", archive_settings)->IsShownOnScreen(),
              "delete-all action is visible beside the archive page heading");
        if (scope && list) {
            scope->SetStringSelection("Recent");
            wxCommandEvent select_scope(wxEVT_CHOICE, scope->GetId());
            scope->GetEventHandler()->ProcessEvent(select_scope);
            Check(list->GetItemCount() == 2, "recent scope keeps explicitly recent chats in project workspaces");
            scope->SetStringSelection("Test");
            scope->GetEventHandler()->ProcessEvent(select_scope);
            Check(list->GetItemCount() == 2, "project scope excludes explicitly recent chats");
            archive_settings->Reload(archive_config);
            Check(scope->GetStringSelection() == "Test" && list->GetItemCount() == 2,
                  "reload preserves the semantic project filter");
        }
        if (search && list) {
            search->SetValue("missing chat");
            auto* pages = dynamic_cast<wxSimplebook*>(list->GetParent());
            auto* status = wxWindow::FindWindowByName("spark.archives.status", archive_settings);
            Check(list->GetItemCount() == 0 && pages && pages->GetPageCount() == 1 &&
                      pages->GetSelection() == 0 && status && status->IsShownOnScreen() &&
                      status->GetLabel().Find("0") == wxNOT_FOUND,
                  "empty archive results stay in the list and use the bottom status line without a zero count");
            search->SetValue("");
        }
        Capture(archive_settings_host, output / "settings-archives-grouped.png");
    }
    archive_settings_host->Destroy();

    auto sidebar_config = config;
    const auto sidebar_data = temporary / "sidebar-fixture";
    sidebar_config.storage.data_dir = PathToUtf8(sidebar_data);
    fs::create_directories(sidebar_data / "sessions");
    const auto second_folder = PathToUtf8(temporary / "project-second-folder");
    std::ofstream(sidebar_data / "projects.json") << nlohmann::json({
        {"projects", {
            {{"name", "Test"}, {"workspace_path", project_path}, {"folders", {project_path, second_folder}}},
            {{"name", "Empty"}, {"workspace_path", PathToUtf8(temporary / "empty-project")}}
        }}
    }).dump();
    const auto write_session = [&](const char* filename, const char* title, const char* scope) {
        std::ofstream stream(sidebar_data / "sessions" / filename);
        stream << nlohmann::json({{"type", "session.created"},
            {"payload", {{"title", title}, {"workspace", second_folder}}}}).dump() << '\n';
        stream << nlohmann::json({{"type", "session.scope"}, {"payload", {{"scope", scope}}}}).dump() << '\n';
        stream << nlohmann::json({{"type", "message"}, {"payload", {{"role", "user"}, {"content", "fixture"}}}}).dump() << '\n';
    };
    write_session("project.jsonl", "Project chat", "project");
    write_session("recent.jsonl", "Recent chat", "recent");
    auto* sidebar_frame = new NativeFrame(sidebar_config);
    sidebar_frame->SetSize(1100, 750);
    sidebar_frame->Show();
    auto* project_list = dynamic_cast<NativeList*>(
        wxWindow::FindWindowByName("spark.projects.list", sidebar_frame));
    auto* recent_list = dynamic_cast<NativeList*>(
        wxWindow::FindWindowByName("spark.history", sidebar_frame));
    for (int attempt = 0; attempt < 20 && recent_list && recent_list->GetItemCount() != 1; ++attempt) Events();
    Check(project_list && project_list->GetItemCount() == 2 && recent_list && recent_list->GetItemCount() == 1,
          "project and recent sidebar chats use independent lists and collapsed projects");
    auto* projects_menu = wxWindow::FindWindowByName("spark.sidebar.projects.menu", sidebar_frame);
    auto* projects_add = wxWindow::FindWindowByName("spark.sidebar.projects.add", sidebar_frame);
    Check(projects_menu && projects_add && projects_menu->GetPosition().x < projects_add->GetPosition().x,
          "sidebar section menu precedes the rightmost new-project button");
    if (project_list) {
        const auto click_project = [&](int index, int x) {
            wxRect rect;
            rect = project_list->GetItemRect(index);
            wxMouseEvent click(wxEVT_LEFT_DOWN);
            click.SetPosition({x, rect.y + rect.height / 2});
            click.SetEventObject(project_list);
            project_list->GetEventHandler()->ProcessEvent(click);
            Events();
        };
        click_project(0, project_list->FromDIP(30));
        Check(project_list->GetItemCount() == 3 && project_list->GetString(1) == "Project chat",
              "expanding a project includes chats from its secondary folders");
        wxRect rect;
        rect = project_list->GetItemRect(1);
        Check(!rect.IsEmpty() && rect.height == project_list->FromDIP(34),
              "sidebar chat rows use the Windows 34-DIP density");
        const auto hover_action = [&](int row, int distance, const char* expected) {
            const wxRect bounds = project_list->GetItemRect(row);
            wxMouseEvent motion(wxEVT_MOTION);
            motion.SetPosition({bounds.x + bounds.width - project_list->FromDIP(distance),
                                bounds.y + bounds.height / 2});
            motion.SetEventObject(project_list);
            project_list->GetEventHandler()->ProcessEvent(motion);
            Check(project_list->GetToolTipText() == expected,
                  "sidebar action tooltips follow the Windows click boundaries");
        };
        hover_action(1, 97, "Project chat");
        hover_action(1, 95, "Delete chat");
        hover_action(1, 64, "Pin");
        hover_action(1, 34, "Pin");
        hover_action(1, 32, "Archive chat");
        hover_action(0, 36, "Projects");
        hover_action(0, 33, "New chat");
        wxTimer context_menu;
        context_menu.Bind(wxEVT_TIMER, [&](wxTimerEvent&) {
            auto* canvas = wxWindow::FindWindowByName("spark.menu.canvas");
            if (!canvas || !canvas->IsShownOnScreen()) return;
            context_menu.Stop();
            Check(canvas->GetClientSize().y == canvas->FromDIP(36) * 2 + 2,
                  "chat context menu contains only the Windows Rename and Export rows");
            Capture(canvas->GetParent(), output / "sidebar-chat-context.png", wxSize(100, 60));
            wxKeyEvent escape(wxEVT_KEY_DOWN);
            escape.m_keyCode = WXK_ESCAPE;
            escape.SetEventObject(canvas);
            canvas->GetEventHandler()->ProcessEvent(escape);
        });
        context_menu.Start(50);
        wxContextMenuEvent context_event(wxEVT_CONTEXT_MENU, project_list->GetId(),
            project_list->ClientToScreen({rect.x + project_list->FromDIP(40), rect.y + rect.height / 2}));
        context_event.SetEventObject(project_list);
        project_list->GetEventHandler()->ProcessEvent(context_event);
        context_menu.Stop();
        click_project(2, project_list->FromDIP(30));
        Check(project_list->GetItemCount() == 3, "empty projects do not expand or create a chat");
        click_project(0, project_list->GetClientSize().x - project_list->FromDIP(28));
        Check(std::distance(fs::directory_iterator(sidebar_data / "sessions"), fs::directory_iterator{}) == 2,
              "project new-chat action remains a draft until a message is sent");
    }
    Capture(sidebar_frame, output / "sidebar-sections.png");
    sidebar_frame->Destroy();
    const wxRect add_anchor = input_panel->GetScreenRect();
    wxTimer add_driver;
    add_driver.Bind(wxEVT_TIMER, [&](wxTimerEvent&) {
        auto* canvas = wxWindow::FindWindowByName("spark.add-menu.canvas");
        if (!canvas || !canvas->IsShownOnScreen()) return;
        add_driver.Stop();
        Check(canvas->GetClientSize() == wxSize(add_anchor.width, canvas->FromDIP(208)),
              "Add panel matches the composer width and four Windows action rows");
        Check(canvas->GetScreenRect().GetBottom() < add_anchor.y,
              "Add panel opens above the composer without covering its text");
        Capture(canvas->GetParent(), output / "composer-add-menu.png", wxSize(180, 120));
        for (const int code : {WXK_DOWN, WXK_DOWN, WXK_RETURN}) {
            wxKeyEvent key(wxEVT_KEY_DOWN);
            key.m_keyCode = code;
            key.SetEventObject(canvas);
            canvas->GetEventHandler()->ProcessEvent(key);
        }
    });
    add_driver.Start(50);
    const auto add_action = ShowNativeAddMenu(frame, add_anchor, "", "", false);
    add_driver.Stop();
    Check(add_action == NativeAddAction::Goal,
          "Add panel keyboard navigation skips an unavailable snapshot target");
    wxTimer snapshot_driver;
    snapshot_driver.Bind(wxEVT_TIMER, [&](wxTimerEvent&) {
        auto* canvas = wxWindow::FindWindowByName("spark.add-menu.canvas");
        if (!canvas || !canvas->IsShownOnScreen()) return;
        snapshot_driver.Stop();
        Capture(canvas->GetParent(), output / "composer-add-menu-active.png", wxSize(180, 120));
        for (const int code : {WXK_DOWN, WXK_DOWN, WXK_RETURN}) {
            wxKeyEvent key(wxEVT_KEY_DOWN);
            key.m_keyCode = code;
            key.SetEventObject(canvas);
            canvas->GetEventHandler()->ProcessEvent(key);
        }
    });
    snapshot_driver.Start(50);
    const auto snapshot_action = ShowNativeAddMenu(frame, add_anchor, "Fixture application", "Read-only audit", true);
    snapshot_driver.Stop();
    Check(snapshot_action == NativeAddAction::Snapshot,
          "Add panel enables the snapshot action when a target is available");
    wxTimer folder_driver;
    const wxString attachment_directory = wxString::FromUTF8(PathToUtf8(temporary));
    folder_driver.Bind(wxEVT_TIMER, [&](wxTimerEvent&) {
        auto* picker = dynamic_cast<wxFileDialog*>(wxWindow::FindWindowByName("spark.attachment-picker"));
        if (!picker) return;
        if (picker->GetDirectory() != attachment_directory) return;
        auto* folder = wxWindow::FindWindowByName("spark.attachment-picker.folder", picker);
        if (!folder || !folder->IsShownOnScreen()) return;
        folder_driver.Stop();
        wxCommandEvent attach(wxEVT_BUTTON, folder->GetId());
        attach.SetEventObject(folder);
        folder->GetEventHandler()->ProcessEvent(attach);
    });
    folder_driver.Start(50);
    const auto picked_folder = PickNativeAttachments(frame, PathToUtf8(temporary));
    folder_driver.Stop();
    Check(picked_folder.ok && picked_folder.value == std::vector<std::string>{PathToUtf8(temporary)},
          "the file picker attaches its live current folder without opening a second dialog");
    {
        NativeTextPrompt prompt(frame, "Goal (clear to remove)", "Existing goal");
        wxTimer prompt_driver;
        prompt_driver.Bind(wxEVT_TIMER, [&](wxTimerEvent&) {
            if (!prompt.IsShownOnScreen()) return;
            prompt_driver.Stop();
            auto* edit = dynamic_cast<wxTextCtrl*>(
                wxWindow::FindWindowByName("spark.text-prompt.value", &prompt));
            auto* ok = wxWindow::FindWindowByName("spark.text-prompt.ok", &prompt);
            auto* cancel = wxWindow::FindWindowByName("spark.text-prompt.cancel", &prompt);
            Check(edit && ok && cancel, "text prompt exposes one edit and two actions");
            if (!edit || !ok || !cancel) { prompt.EndModal(wxID_CANCEL); return; }
            Check(!edit->IsMultiLine() && edit->GetValue() == "Existing goal",
                  "goal and rename prompts use a single-line initial value");
            Check(edit->GetRect() == wxRect(prompt.FromDIP(wxPoint(18, 22)), prompt.FromDIP(wxSize(484, 30))) &&
                      ok->GetRect() == wxRect(prompt.FromDIP(wxPoint(330, 76)), prompt.FromDIP(wxSize(80, 32))) &&
                      cancel->GetRect() == wxRect(prompt.FromDIP(wxPoint(420, 76)), prompt.FromDIP(wxSize(80, 32))),
                  "text prompt edit and actions match the Windows body coordinates");
            Capture(&prompt, output / "composer-goal-prompt.png", wxSize(400, 110));
            edit->ChangeValue("");
            wxCommandEvent enter(wxEVT_TEXT_ENTER, edit->GetId());
            enter.SetEventObject(edit);
            edit->GetEventHandler()->ProcessEvent(enter);
        });
        prompt_driver.Start(50);
        Check(prompt.ShowModal() == wxID_OK && prompt.GetValue().empty(),
              "accepting an empty goal remains distinct from cancelling it");
        prompt_driver.Stop();
    }
    {
        NativeTextPrompt prompt(frame, "Rename chat", "Original title");
        wxTimer cancel_driver;
        cancel_driver.Bind(wxEVT_TIMER, [&](wxTimerEvent&) {
            if (!prompt.IsShownOnScreen()) return;
            cancel_driver.Stop();
            wxKeyEvent escape(wxEVT_CHAR_HOOK);
            escape.m_keyCode = WXK_ESCAPE; escape.SetEventObject(&prompt);
            prompt.GetEventHandler()->ProcessEvent(escape);
        });
        cancel_driver.Start(50);
        Check(prompt.ShowModal() == wxID_CANCEL && prompt.GetValue() == "Original title",
              "Escape dismisses a rename prompt without accepting its value");
        cancel_driver.Stop();
    }
    auto* provider_list = new NativeProviderList(provider_host);
    provider_list->SetProviders({
        {"Alpha", "Alpha", "responses", "https://alpha.example"},
        {"Beta", "Beta", "chat_completions", "https://beta.example"},
    });
    const std::array<wxString, 4> provider_tooltips = {
        "Send hi test", "Edit provider", "Duplicate provider", "Delete provider"};
    provider_list->SetActionTooltips(provider_tooltips, "Reorder providers");
    provider_host->SetSize(700, 300);
    provider_list->SetSize(provider_host->GetClientSize());
    provider_host->Show();
    Events();
    Check(provider_list->GetItemCount() == 2, "provider cards retain every configured provider");
    Check(!provider_list->IsCompact(), "wide provider cards use the normal density");
    wxRect provider_row;
    provider_row = provider_list->GetItemRect(0);
    Check(provider_row.height == provider_host->FromDIP(88), "wide provider cards use 88 DIP rows");
    std::vector<ProviderListAction> provider_actions;
    std::pair<size_t, size_t> provider_reorder{wxNOT_FOUND, wxNOT_FOUND};
    provider_list->SetActionHandler([&](size_t index, ProviderListAction action) {
        Check(index == 0, "provider card action retains the clicked row index");
        provider_actions.push_back(action);
    });
    provider_list->SetReorderHandler([&](size_t from, size_t to) { provider_reorder = {from, to}; });
    auto provider_mouse = [&](wxEventType type, const wxPoint& point, bool left_down = false) {
        wxMouseEvent event(type);
        event.SetPosition(point);
        event.SetEventObject(provider_list);
        if (left_down) event.SetLeftDown(true);
        provider_list->GetEventHandler()->ProcessEvent(event);
    };
    for (int action = 0; action < 4; ++action) {
        const int size = provider_list->FromDIP(32);
        const int left = provider_row.GetRight() - provider_list->FromDIP(12 + (4 - action) * 32);
        const wxPoint point(left + size / 2, provider_row.y + provider_row.height / 2);
        provider_mouse(wxEVT_MOTION, point);
        Check(provider_list->GetToolTipText() == provider_tooltips[static_cast<size_t>(action)],
              "hovering a provider action exposes its tooltip");
        provider_mouse(wxEVT_LEFT_DOWN, point);
        provider_mouse(wxEVT_LEFT_UP, point);
    }
    Check(provider_actions == std::vector<ProviderListAction>{
              ProviderListAction::Test, ProviderListAction::Edit,
              ProviderListAction::Duplicate, ProviderListAction::Delete},
          "provider card exposes all four wired action callbacks");
    const wxPoint grip(provider_row.x + provider_list->FromDIP(12),
                       provider_row.y + provider_row.height / 2);
    provider_mouse(wxEVT_MOTION, grip);
    Check(provider_list->GetToolTipText() == "Reorder providers",
          "hovering the provider grip exposes its reorder tooltip");
    const auto next_provider_row = provider_list->GetItemRect(1);
    const wxPoint drop(provider_row.x + provider_list->FromDIP(12),
                       next_provider_row.y + next_provider_row.height * 3 / 4);
    provider_mouse(wxEVT_LEFT_DOWN, grip);
    provider_mouse(wxEVT_MOTION, drop, true);
    provider_mouse(wxEVT_LEFT_UP, drop);
    Check(provider_reorder == std::pair<size_t, size_t>{0, 1},
          "dragging the provider grip reports the expected reorder");
    provider_host->SetSize(440, 300);
    provider_list->SetSize(provider_host->GetClientSize());
    Events();
    Check(provider_list->IsCompact(), "narrow provider cards switch to compact density");
    provider_row = provider_list->GetItemRect(0);
    Check(provider_row.height == provider_host->FromDIP(112), "narrow provider cards use 112 DIP rows");
    provider_host->Destroy();
    Events();

    auto* editor_host = new wxFrame(frame, wxID_ANY, "provider-editor-layout-fixture");
    auto* model_editor = new NativeProviderEditor(
        editor_host, config.providers.front(), ContextConfig{}, "dark");
    editor_host->SetSize(1500, 900);
    model_editor->SetSize(editor_host->GetClientSize());
    editor_host->Show();
    Events();
    auto* first_model_id = dynamic_cast<wxTextCtrl*>(
        wxWindow::FindWindowByName("spark.provider.editor.model.0.id", editor_host));
    auto* first_model_context = dynamic_cast<wxTextCtrl*>(
        wxWindow::FindWindowByName("spark.provider.editor.model.0.context", editor_host));
    auto* first_model_compact = dynamic_cast<NativeChoice*>(
        wxWindow::FindWindowByName("spark.provider.editor.model.0.auto-compact", editor_host));
    auto* first_model_images = dynamic_cast<NativeChoice*>(
        wxWindow::FindWindowByName("spark.provider.editor.model.0.images", editor_host));
    auto* first_model_reasoning = dynamic_cast<NativeButton*>(
        wxWindow::FindWindowByName("spark.provider.editor.model.0.reasoning", editor_host));
    auto* first_model_remove = dynamic_cast<NativeButton*>(
        wxWindow::FindWindowByName("spark.provider.editor.model.0.remove", editor_host));
    auto* first_model_panel = wxWindow::FindWindowByName("spark.provider.editor.model.0", editor_host);
    auto* last_model_id = wxWindow::FindWindowByName("spark.provider.editor.model.29.id", editor_host);
    auto* second_model_id = wxWindow::FindWindowByName("spark.provider.editor.model.1.id", editor_host);
    auto* model_list_label = wxWindow::FindWindowByName(
        "spark.provider.editor.model-list-label", editor_host);
    auto* provider_name_label = wxWindow::FindWindowByName(
        "spark.provider.editor.label.name", editor_host);
    auto* provider_protocol_label = wxWindow::FindWindowByName(
        "spark.provider.editor.label.protocol", editor_host);
    auto* provider_url_label = wxWindow::FindWindowByName(
        "spark.provider.editor.label.url", editor_host);
    auto* provider_key_label = wxWindow::FindWindowByName(
        "spark.provider.editor.label.key", editor_host);
    auto* add_model_button = dynamic_cast<NativeButton*>(
        wxWindow::FindWindowByName("spark.provider.editor.add-model", editor_host));
    auto* fetch_models_button = dynamic_cast<NativeButton*>(
        wxWindow::FindWindowByName("spark.provider.editor.fetch-models", editor_host));
    auto* clear_models_button = dynamic_cast<NativeButton*>(
        wxWindow::FindWindowByName("spark.provider.editor.clear-models", editor_host));
    auto* preview_label = wxWindow::FindWindowByName(
        "spark.provider.editor.preview-label", editor_host);
    auto* load_configuration = wxWindow::FindWindowByName(
        "spark.provider.editor.load-configuration", editor_host);
    auto* preview = dynamic_cast<wxTextCtrl*>(
        wxWindow::FindWindowByName("spark.provider.editor.preview", editor_host));
    Check(first_model_id && first_model_context && last_model_id,
          "provider editor renders a separate row for every configured model");
    Check(model_list_label && add_model_button && fetch_models_button && clear_models_button &&
              add_model_button->GetLabel() == "Add model" &&
              fetch_models_button->GetLabel() == "Fetch from upstream" &&
              clear_models_button->GetLabel() == "Clear models" &&
              add_model_button->GetRect().y - model_list_label->GetRect().y ==
                  editor_host->FromDIP(34),
          "provider model actions match the Windows title-then-actions layout");
    Check(model_list_label && provider_name_label && provider_protocol_label &&
              provider_url_label && provider_key_label &&
              provider_name_label->GetScreenRect().y == provider_protocol_label->GetScreenRect().y &&
              provider_url_label->GetScreenRect().y == provider_key_label->GetScreenRect().y &&
              provider_url_label->GetScreenRect().y - provider_name_label->GetScreenRect().y ==
                  editor_host->FromDIP(86) &&
              model_list_label->GetScreenRect().y - provider_url_label->GetScreenRect().y ==
                  editor_host->FromDIP(98),
          "wide provider fields follow Windows label, row-gap, and model-section geometry");
    Check(wxWindow::FindWindowByName("spark.provider.editor.headers", editor_host) == nullptr,
          "provider editor does not add a separate headers form absent from Windows");
    Check(preview_label && load_configuration && preview &&
              load_configuration->GetRect().y > preview_label->GetRect().y &&
              preview->GetRect().y > load_configuration->GetRect().GetBottom() &&
              preview->GetMinSize().y == editor_host->FromDIP(260) &&
              wxWindow::FindWindowByName("spark.provider.editor.preview-action", editor_host) == nullptr,
          "configuration preview matches the Windows load-button order and fixed height");
    Check(first_model_panel && first_model_panel->GetBackgroundColour() == wxColour(28, 29, 31) &&
              first_model_id && first_model_id->GetBackgroundColour() == wxColour(48, 49, 52) &&
              first_model_compact && first_model_compact->GetForegroundColour().Red() > 180,
          "initial provider model controls inherit the dark theme");
    Check(first_model_id && first_model_context && second_model_id &&
              first_model_id->GetScreenRect().y == first_model_context->GetScreenRect().y,
          "wide provider model rows keep fields on one line");
    Check(first_model_id && second_model_id &&
              second_model_id->GetScreenRect().y - first_model_id->GetScreenRect().y ==
                  editor_host->FromDIP(50),
          "wide provider model rows follow the Windows 50-DIP row interval");
    Check(first_model_context && first_model_images && first_model_reasoning && first_model_remove &&
              first_model_context->GetSize().x == editor_host->FromDIP(82) &&
              first_model_images->GetSize().x == editor_host->FromDIP(136) &&
              first_model_reasoning->GetSize().x == editor_host->FromDIP(184) &&
              first_model_remove->GetSize().x == editor_host->FromDIP(32),
          "wide provider model inputs retain the Windows fixed-width columns and insets");
    const int initial_model_id_width = first_model_id ? first_model_id->GetSize().x : 0;
    editor_host->SetSize(1700, 900);
    model_editor->SetSize(editor_host->GetClientSize());
    Events();
    Check(first_model_id && first_model_context && first_model_images && first_model_reasoning &&
              first_model_remove && first_model_id->GetSize().x > initial_model_id_width &&
              first_model_context->GetSize().x == editor_host->FromDIP(82) &&
              first_model_images->GetSize().x == editor_host->FromDIP(136) &&
              first_model_reasoning->GetSize().x == editor_host->FromDIP(184) &&
              first_model_remove->GetSize().x == editor_host->FromDIP(32),
          "wide provider model rows assign extra width only to Model ID");
    editor_host->SetSize(1500, 900);
    model_editor->SetSize(editor_host->GetClientSize());
    Events();

    Check(first_model_reasoning != nullptr, "provider editor exposes a reasoning action");
    if (first_model_reasoning) {
        const auto click = [](wxWindow* window, const wxPoint& point) {
            wxMouseEvent down(wxEVT_LEFT_DOWN);
            down.SetPosition(point);
            down.SetEventObject(window);
            window->GetEventHandler()->ProcessEvent(down);
            wxMouseEvent up(wxEVT_LEFT_UP);
            up.SetPosition(point);
            up.SetEventObject(window);
            window->GetEventHandler()->ProcessEvent(up);
        };
        wxCommandEvent open(wxEVT_BUTTON, first_model_reasoning->GetId());
        open.SetEventObject(first_model_reasoning);
        first_model_reasoning->GetEventHandler()->ProcessEvent(open);
        Events();
        auto* reasoning_popup = wxWindow::FindWindowByName(
            "spark.reasoning.popup", editor_host);
        auto* reasoning_canvas = wxWindow::FindWindowByName(
            "spark.reasoning.popup.canvas", editor_host);
        Check(reasoning_popup && reasoning_popup->IsShownOnScreen() &&
                  reasoning_canvas && reasoning_canvas->IsShownOnScreen(),
              "reasoning action opens the native reasoning popup");
        if (reasoning_popup && reasoning_canvas) {
            Capture(reasoning_popup, output / "reasoning-popup.png", wxSize(250, 180));
            const int header_height = first_model_reasoning->FromDIP(42);
            const int effort_height = first_model_reasoning->FromDIP(30);
            const int chip_height = first_model_reasoning->FromDIP(28);
            const int chip_gap = first_model_reasoning->FromDIP(6);
            const int chip_y = header_height + effort_height * 3 +
                               first_model_reasoning->FromDIP(10 + 22);
            const int chip_center_y = chip_y + chip_height / 2;
            const auto chip_center_x = [&](int target) {
                wxClientDC dc(reasoning_canvas);
                dc.SetFont(reasoning_canvas->GetFont());
                int x = first_model_reasoning->FromDIP(12);
                const char* labels[] = {"Auto", "low", "medium", "high"};
                for (int index = 0; index <= target; ++index) {
                    const wxString label = wxString::FromUTF8(labels[index]);
                    const int width = std::max(first_model_reasoning->FromDIP(44),
                        dc.GetTextExtent(label).x + first_model_reasoning->FromDIP(22));
                    if (index == target) return x + width / 2;
                    x += width + chip_gap;
                }
                return x;
            };
            const int effort_y = header_height + effort_height / 2;
            click(reasoning_canvas,
                  wxPoint(first_model_reasoning->FromDIP(40), effort_y));
            Events();
            Check(reasoning_popup->IsShownOnScreen() &&
                      first_model_reasoning->GetLabel().Find("medium") != wxNOT_FOUND,
                  "reasoning effort selection keeps the remaining default effort");

            click(reasoning_canvas,
                  wxPoint(chip_center_x(3), chip_center_y));
            Events();
            Check(first_model_reasoning->GetLabel().Find("high") != wxNOT_FOUND,
                  "reasoning default selection updates the model row");

            click(reasoning_canvas,
                  wxPoint(chip_center_x(0), chip_center_y));
            Events();
            Check(first_model_reasoning->GetLabel().Find("Auto") != wxNOT_FOUND,
                  "reasoning auto selection clears the explicit default");

            const wxSize popup_size = reasoning_canvas->GetClientSize();
            click(reasoning_canvas,
                  wxPoint(popup_size.x / 2,
                          popup_size.y - first_model_reasoning->FromDIP(17)));
            Events();
            Check(reasoning_popup->IsShownOnScreen(),
                  "reasoning popup remains open after applying built-in defaults");
            Check(first_model_reasoning->GetLabel().Find("Auto") != wxNOT_FOUND,
                  "built-in reasoning defaults restore the automatic row label");

            wxKeyEvent escape(wxEVT_KEY_DOWN);
            escape.m_keyCode = WXK_ESCAPE;
            escape.SetEventObject(reasoning_canvas);
            reasoning_canvas->GetEventHandler()->ProcessEvent(escape);
            Events();
            Check(!reasoning_popup->IsShownOnScreen(),
                  "Escape closes the reasoning popup");
        }

        first_model_reasoning->GetEventHandler()->ProcessEvent(open);
        Events();
        reasoning_popup = wxWindow::FindWindowByName("spark.reasoning.popup", editor_host);
        Check(reasoning_popup && reasoning_popup->IsShownOnScreen(),
              "reasoning popup can be reopened after dismissal");
        if (reasoning_popup) {
            wxMouseEvent outside(wxEVT_LEFT_DOWN);
            outside.SetPosition({4, 4});
            outside.SetEventObject(editor_host);
            editor_host->GetEventHandler()->ProcessEvent(outside);
            Events();
            Check(!reasoning_popup->IsShownOnScreen(),
                  "clicking outside dismisses the reasoning popup");
        }
    }

    auto* editor_back = dynamic_cast<NativeButton*>(
        wxWindow::FindWindowByName("spark.provider.editor.back", editor_host));
    auto* editor_title = wxWindow::FindWindowByName("spark.provider.editor.title", editor_host);
    auto* editor_save = dynamic_cast<NativeButton*>(
        wxWindow::FindWindowByName("spark.provider.editor.save", editor_host));
    Check(editor_back && editor_back->GetLabel().empty(),
          "provider editor back action is the Windows-style icon-only control");
    const auto title_rect = editor_title ? editor_title->GetScreenRect() : wxRect{};
    const auto save_rect = editor_save ? editor_save->GetScreenRect() : wxRect{};
    Check(editor_title && editor_save &&
              title_rect.y + title_rect.height / 2 == save_rect.y + save_rect.height / 2,
          "wide provider editor keeps save beside the title");
    if (editor_save) {
        wxClientDC save_dc(editor_save);
        save_dc.SetFont(editor_save->GetFont());
        Check(editor_save->GetSize().x >=
                  editor_save->FromDIP(save_dc.GetTextExtent(editor_save->GetLabel()).x + 60),
              "provider editor save button reserves space for its icon and full label");
    }
    Capture(editor_host, output / "provider-editor-wide.png", wxSize(1000, 600));

    editor_host->SetSize(560, 900);
    model_editor->SetSize(editor_host->GetClientSize());
    Events();
    Check(editor_title && editor_save &&
              editor_save->GetScreenRect().y > editor_title->GetScreenRect().y &&
              editor_save->GetScreenRect().GetRight() <= model_editor->GetScreenRect().GetRight(),
          "compact provider editor moves the full save action below the title");
    Capture(editor_host, output / "provider-editor-compact.png", wxSize(500, 600));

    editor_host->SetSize(900, 900);
    model_editor->SetSize(editor_host->GetClientSize());
    Events();
    auto* compact_model_context = dynamic_cast<wxTextCtrl*>(
        wxWindow::FindWindowByName("spark.provider.editor.model.0.context", editor_host));
    auto* compact_model_auto = dynamic_cast<NativeChoice*>(
        wxWindow::FindWindowByName("spark.provider.editor.model.0.auto-compact", editor_host));
    Check(first_model_panel && first_model_panel->GetSize().y == editor_host->FromDIP(120),
          "compact provider model rows use three 36-DIP lines with six-DIP gaps");
    Check(first_model_id && compact_model_context && second_model_id &&
              second_model_id->GetScreenRect().y - first_model_id->GetScreenRect().y ==
                  editor_host->FromDIP(126),
          "compact provider model rows follow the Windows 126-DIP row interval");
    Check(first_model_id && compact_model_context &&
              first_model_id->GetScreenRect().y < compact_model_context->GetScreenRect().y,
          "narrow provider model rows stack fields below the model ID");
    Check(compact_model_context && compact_model_auto && first_model_images && first_model_reasoning &&
              compact_model_context->GetScreenRect().y + compact_model_context->GetSize().y / 2 ==
                  compact_model_auto->GetScreenRect().y + compact_model_auto->GetSize().y / 2 &&
              first_model_images->GetScreenRect().y == first_model_reasoning->GetScreenRect().y &&
              first_model_reasoning->GetLabel().StartsWith("Reasoning: ") &&
              first_model_images->GetStringSelection().StartsWith("Image: "),
          "compact provider model rows align paired controls and restore Windows prefixes");

    auto* add_model = dynamic_cast<NativeButton*>(
        wxWindow::FindWindowByName("spark.provider.editor.add-model", editor_host));
    Check(add_model != nullptr, "provider editor exposes an add-model action");
    if (add_model) {
        wxCommandEvent add(wxEVT_BUTTON, add_model->GetId());
        add.SetEventObject(add_model);
        add_model->GetEventHandler()->ProcessEvent(add);
        Events();
        auto* added_model = wxWindow::FindWindowByName(
            "spark.provider.editor.model.30.id", editor_host);
        Check(added_model != nullptr, "adding a model appends one editable model row");
        auto* added_model_panel = wxWindow::FindWindowByName(
            "spark.provider.editor.model.30", editor_host);
        auto* added_model_context = dynamic_cast<wxTextCtrl*>(wxWindow::FindWindowByName(
            "spark.provider.editor.model.30.context", editor_host));
        auto* added_model_compact = dynamic_cast<NativeChoice*>(wxWindow::FindWindowByName(
            "spark.provider.editor.model.30.auto-compact", editor_host));
        Check(added_model_panel && added_model_panel->GetBackgroundColour() == wxColour(28, 29, 31) &&
                  added_model && added_model->GetBackgroundColour() == wxColour(48, 49, 52) &&
                  added_model_context && added_model_compact &&
                  added_model_compact->GetForegroundColour().Red() > 180,
              "new provider model controls inherit the dark theme");
        auto* remove_model = dynamic_cast<NativeButton*>(
            wxWindow::FindWindowByName("spark.provider.editor.model.30.remove", editor_host));
        if (remove_model) {
            wxCommandEvent remove(wxEVT_BUTTON, remove_model->GetId());
            remove.SetEventObject(remove_model);
            remove_model->GetEventHandler()->ProcessEvent(remove);
            Events();
        }
        Check(wxWindow::FindWindowByName("spark.provider.editor.model.30.id", editor_host) == nullptr,
              "removing a model deletes its row and reindexes following rows");
    }
    if (clear_models_button) {
        wxCommandEvent clear(wxEVT_BUTTON, clear_models_button->GetId());
        clear.SetEventObject(clear_models_button);
        clear_models_button->GetEventHandler()->ProcessEvent(clear);
        for (int attempt = 0; attempt < 4; ++attempt) Events();
        Check(wxWindow::FindWindowByName("spark.provider.editor.model.0.id", editor_host) == nullptr,
              "clear-models removes all provider model rows");
        const auto cleared_preview = preview
            ? ParseConfigJson(preview->GetValue().ToStdString(wxConvUTF8))
            : nlohmann::json{};
        Check(cleared_preview.is_object() && cleared_preview.contains("providers") &&
                  cleared_preview["providers"]["fixture"]["models"].empty(),
              "clearing models refreshes the live configuration preview");
    }
    editor_host->Destroy();
    Events();

    auto* rename_host = new wxFrame(frame, wxID_ANY, "provider-model-rename-fixture");
    ProviderConfig rename_provider = config.providers.front();
    rename_provider.models.resize(2);
    rename_provider.models[0].id = rename_provider.models[0].display_name = "model-a";
    rename_provider.models[1].id = rename_provider.models[1].display_name = "model-b";
    nlohmann::json rename_raw = {
        {"base_url", "https://fixture.example"},
        {"protocol", "responses"},
        {"models", nlohmann::json::array({
            {{"id", "model-a"}, {"extension_marker", "extension-a"}},
            {{"id", "model-b"}, {"extension_marker", "extension-b"}},
        })},
    };
    auto* rename_editor = new NativeProviderEditor(
        rename_host, rename_provider, ContextConfig{}, "dark", rename_raw);
    rename_host->SetSize(1500, 900);
    rename_editor->SetSize(rename_host->GetClientSize());
    rename_host->Show();
    Events();
    auto* rename_a = dynamic_cast<wxTextCtrl*>(
        wxWindow::FindWindowByName("spark.provider.editor.model.0.id", rename_host));
    auto* rename_b = dynamic_cast<wxTextCtrl*>(
        wxWindow::FindWindowByName("spark.provider.editor.model.1.id", rename_host));
    auto* rename_preview = dynamic_cast<wxTextCtrl*>(
        wxWindow::FindWindowByName("spark.provider.editor.preview", rename_host));
    Check(rename_a && rename_b && rename_preview,
          "provider editor exposes model rename and preview controls");
    if (rename_a && rename_b && rename_preview) {
        auto notify_changed = [](wxTextCtrl* control) {
            wxCommandEvent event(wxEVT_TEXT, control->GetId());
            event.SetEventObject(control);
            control->GetEventHandler()->ProcessEvent(event);
        };
        auto refresh = [&] {
            for (int attempt = 0; attempt < 4; ++attempt) Events();
            return ParseConfigJson(rename_preview->GetValue().ToStdString(wxConvUTF8));
        };
        rename_a->ChangeValue("model-b");
        rename_b->ChangeValue("model-a");
        notify_changed(rename_a);
        notify_changed(rename_b);
        auto swapped = refresh();
        Check(swapped["providers"]["fixture"]["models"][0].value("extension_marker", "") == "extension-a" &&
                  swapped["providers"]["fixture"]["models"][1].value("extension_marker", "") == "extension-b",
              "swapping model IDs preserves each model's extension fields");

        rename_a->ChangeValue("model-c");
        rename_b->ChangeValue("model-d");
        notify_changed(rename_a);
        notify_changed(rename_b);
        auto renamed = refresh();
        Check(renamed["providers"]["fixture"]["models"][0].value("extension_marker", "") == "extension-a" &&
                  renamed["providers"]["fixture"]["models"][1].value("extension_marker", "") == "extension-b",
              "consecutive model renames preserve extension fields from the matching original rows");
    }
    rename_host->Destroy();
    Events();

    auto* strip_host = new wxFrame(nullptr, wxID_ANY, "Attachment fixture", wxDefaultPosition, wxSize(900, 500));
    auto* strip_parent = new wxPanel(strip_host);
    auto* strip = new NativeAttachmentStrip(strip_parent);
    strip->SetSize(0, 0, 860, 120);
    const auto picture = PathToUtf8(temporary / "fixture.png");
    wxImage image(16, 16);
    image.SetRGB(wxRect(0, 0, 16, 16), 200, 80, 100);
    image.SaveFile(wxString::FromUTF8(picture), wxBITMAP_TYPE_PNG);
    NativeThumbnailCache thumbnails;
    thumbnails.Request(picture, 28);
    for (int attempt = 0; attempt < 50 && !thumbnails.Get(picture, 28).IsOk(); ++attempt) {
        thumbnails.Poll(); wxMilliSleep(10);
    }
    Check(thumbnails.Get(picture, 28).IsOk(), "summary image previews decode asynchronously");
    thumbnails.Clear();
    Check(!thumbnails.Get(picture, 28).IsOk(), "changing summary parent drops old image previews");
    strip->SetAttachments({picture, PathToUtf8(temporary / "example.cpp")});
    std::string previewed, removed;
    strip->preview = [&](const std::string& path) { previewed = path; };
    strip->remove = [&](const std::string& path) { removed = path; };
    strip_host->Show(); Events();
    const auto strip_click = [&](wxPoint point) {
        for (const auto type : {wxEVT_LEFT_DOWN, wxEVT_LEFT_UP}) {
            wxMouseEvent event(type);
            event.SetPosition(point); event.SetEventObject(strip);
            strip->GetEventHandler()->ProcessEvent(event);
        }
    };
    strip_click(wxPoint(30, 50));
    Check(previewed == picture, "attachment tile opens its own image preview");
    strip_click(wxPoint(105, 15));
    Check(removed == picture, "top-right attachment X removes only the selected image");
    Capture(strip_host, output / "attachment-strip.png");
    const auto image_named_folder = temporary / "folder.png";
    fs::create_directories(image_named_folder);
    const auto folder_path = PathToUtf8(image_named_folder);
    strip->SetAttachments({folder_path, picture});
    removed.clear();
    const int folder_width = std::max(strip->GetClientSize().y, strip->FromDIP(196));
    strip_click({folder_width - strip->FromDIP(15), strip->FromDIP(15)});
    Check(removed == folder_path, "image-extension folders retain a full file-tile close target");
    strip_host->Destroy(); Events();
    auto* usage_host = new wxFrame(frame, wxID_ANY, "Usage fixture");
    auto* usage_surface = new NativeChatSurface(usage_host);
    auto* usage_sizer = new wxBoxSizer(wxVERTICAL);
    usage_sizer->Add(usage_surface, 1, wxEXPAND);
    usage_host->SetSizer(usage_sizer);
    usage_host->SetClientSize(usage_host->FromDIP(wxSize(980, 480)));
    Transcript usage_transcript;
    for (int index = 0; index < 6; ++index)
        usage_transcript.Apply({TranscriptEventKind::Add, kNoChatItem, usage}, now);
    ChatItem compacted;
    compacted.kind = ChatItemKind::Compaction;
    usage_transcript.Apply({TranscriptEventKind::Add, kNoChatItem, compacted}, now);
    usage_surface->SetDark(true);
    usage_surface->SetTranscript(&usage_transcript, true);
    usage_host->Show(); Events();
    usage_surface->Scroll(0, 0);
    usage_surface->Refresh(); usage_surface->Update();
    const int wide_usage_height = usage_surface->GetVirtualSize().y;
    {
        wxClientDC dc(usage_surface);
        wxColour border;
        dc.GetPixel(usage_surface->FromDIP(56), usage_surface->FromDIP(18), &border);
        Check(border == wxColour(111, 190, 140), "completed usage paints the Windows green border");
    }
    Capture(usage_host, output / "usage-stats-dark.png");
    usage_host->SetClientSize(usage_host->FromDIP(wxSize(420, 480)));
    usage_surface->SetDark(false);
    Events();
    usage_surface->Scroll(0, 0);
    usage_surface->Refresh(); usage_surface->Update();
    Check(usage_surface->GetVirtualSize().y > wide_usage_height,
          "usage statistics wrap and reserve more vertical space in a narrow transcript");
    Capture(usage_host, output / "usage-stats-light-narrow.png", wxSize(350, 350));
    usage_host->Destroy(); Events();

    {
        NativeProjectDialog dialog(frame, "", {project_path}, NativeProjectDialog::Mode::Create);
        wxTimer accept;
        accept.Bind(wxEVT_TIMER, [&](wxTimerEvent&) {
            accept.Stop();
            wxCommandEvent click(wxEVT_BUTTON, wxID_OK);
            dialog.GetEventHandler()->ProcessEvent(click);
        });
        accept.StartOnce(50);
        Check(dialog.ShowModal() == wxID_OK && dialog.GetProjectName() == "project",
              "unnamed projects use the first folder basename");
        Check(dialog.GetFolders() == std::vector<std::string>{project_path},
              "project dialog preserves its supplied folders");
    }

    auto* summary_host = new wxFrame(nullptr, wxID_ANY, "Summary fixture", wxDefaultPosition, wxSize(900, 600));
    std::string selected_child;
    NativeSummaryPanel::Callbacks summary_callbacks;
    summary_callbacks.active_child = [&](const std::string& id) { selected_child = id; };
    auto* panel = new NativeSummaryPanel(summary_host, std::move(summary_callbacks));
    panel->SetSize(summary_host->GetClientRect());
    std::vector<SourceReference> summary_sources{
        {"image", "Fixture image", picture},
        {"web", "Example", "https://example.com"},
    };
    for (int index = 0; index < 8; ++index)
        summary_sources.push_back({"web_search", "Search result " + std::to_string(index + 1),
                                   "https://example.com/search/" + std::to_string(index + 1)});
    panel->SetSources(summary_sources);
    SubAgentInfo child; child.id = "fixture-child"; child.name = "Read-only reviewer";
    child.provider = "fixture"; child.model = "test-model-0";
    panel->SetAgents({child});
    summary_host->Show(); Events();
    auto* summary_rows = wxWindow::FindWindowByName("spark.summary.rows", panel);
    Check(summary_rows != nullptr, "summary list exposes a stable test target");
    if (summary_rows) {
        const auto summary_key = [&](int code) {
            wxKeyEvent event(wxEVT_KEY_DOWN);
            event.m_keyCode = code;
            event.SetEventObject(summary_rows);
            summary_rows->GetEventHandler()->ProcessEvent(event);
        };
        summary_key(WXK_DOWN);
        summary_key(WXK_RETURN);
        Check(panel->ActiveChildId().empty() && selected_child.empty(),
              "summary keyboard scrolling does not select or activate rows");
    }
    Check(panel->SelectAgentTab(child.id) && selected_child == child.id,
              "summary child selection requests the matching transcript");
    panel->ChildChatSurface()->SetTranscript(&transcript, true);
    panel->ShowSources();
    Check(selected_child.empty() && panel->ActiveChildId().empty(),
              "leaving child chat detaches it and invalidates its loader");
    Events();
    Capture(summary_host, output / "summary-sources.png");
    panel->ShowAgents(); Events();
    Capture(summary_host, output / "summary-agents.png");
    int popup_selections = 0;
    const auto popup_callback = [&](NativeSummaryPopupAction, const std::string&) { ++popup_selections; };
    const auto popup_anchor = summary_host->ClientToScreen(wxPoint(800, 40));
    ShowNativeSummaryPopup(summary_host, popup_anchor, {child}, {}, popup_callback);
    Events();
    auto* summary_popup = wxWindow::FindWindowByName("spark.summary.popup", summary_host);
    Check(summary_popup && summary_popup->IsShownOnScreen(), "summary opens a dedicated grouped popup");
    Capture(summary_host, output / "summary-popup.png");
    ShowNativeSummaryPopup(summary_host, popup_anchor, {child}, {}, popup_callback);
    Events();
    summary_popup = wxWindow::FindWindowByName("spark.summary.popup", summary_host);
    Check((!summary_popup || !summary_popup->IsShown()) && popup_selections == 0,
              "clicking summary again dismisses the same-owner popup without navigation");
    summary_host->Destroy(); Events();

    frame->Destroy();
    Events();
    wxTheApp->OnExit();
    wxEntryCleanup();
    std::error_code error;
    fs::remove_all(temporary, error);
    return failures ? 1 : 0;
}
