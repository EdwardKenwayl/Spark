#include "gui/add_menu.h"
#include "gui/app.h"
#include "gui/localization.h"
#include "gui/settings_window.h"
#include "platform/smart_snapshot.h"
#include "platform/window_text.h"
#include "core/session.h"

#include <objidl.h>
using byte = BYTE;
#include <gdiplus.h>
#include <shellapi.h>

#include "platform/filesystem.h"
#include <iostream>

namespace {
int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

LRESULT CALLBACK FixtureProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PRINT || message == WM_PRINTCLIENT) {
        RECT rect{};
        GetClientRect(window, &rect);
        HBRUSH brush = CreateSolidBrush(RGB(70, 150, 90));
        FillRect(reinterpret_cast<HDC>(wparam), &rect, brush);
        DeleteObject(brush);
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC device = BeginPaint(window, &paint);
        HBRUSH brush = CreateSolidBrush(RGB(70, 150, 90));
        FillRect(device, &paint.rcPaint, brush);
        DeleteObject(brush);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void SaveVisualSnapshot(HBITMAP bitmap, const wchar_t* name) {
    const wchar_t* directory = _wgetenv(L"SPARK_SUMMARY_SNAPSHOT_DIR");
    if (!directory) return;
    UINT count = 0, bytes = 0;
    Gdiplus::GetImageEncodersSize(&count, &bytes);
    std::vector<BYTE> storage(bytes);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok) return;
    for (UINT index = 0; index < count; ++index) {
        if (wcscmp(encoders[index].MimeType, L"image/jpeg") != 0) continue;
        Gdiplus::Bitmap image(bitmap, nullptr);
        Check(image.Save((spark::fs::path(directory) / name).c_str(), &encoders[index].Clsid) == Gdiplus::Ok,
              "menu visual snapshot saves");
        break;
    }
}

spark::Result<spark::SmartSnapshot> WaitForCapture(spark::SnapshotCapture& capture) {
    const uint64_t deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (auto result = capture.Poll()) return *result;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    capture.Cancel();
    return spark::Result<spark::SmartSnapshot>::Failure("test deadline exceeded");
}

void CheckSnapshot(const spark::Result<spark::SmartSnapshot>& result) {
    Check(result.ok, "background snapshot succeeds");
    if (!result.ok) {
        std::cerr << "Capture detail: " << result.error << '\n';
        return;
    }
    Gdiplus::Bitmap image(spark::Utf8ToWide(result.value.image_path).c_str());
    Gdiplus::Color pixel;
    Check(image.GetLastStatus() == Gdiplus::Ok && image.GetWidth() == 320 && image.GetHeight() == 180,
          "background snapshot has the target dimensions");
    Check(image.GetPixel(160, 90, &pixel) == Gdiplus::Ok &&
              pixel.GetG() > pixel.GetR() + 30 && pixel.GetG() > pixel.GetB() + 30,
          "occluded target pixels are captured instead of the covering window");
}

void TestSettings(HWND owner, const std::string& directory) {
    auto config = spark::ConfigFromJson(spark::ParseConfigJson(spark::DefaultConfigExampleJson()),
                                       spark::JoinPath(directory, "config.json"));
    config.storage.data_dir = directory;
    config.workspace.path = directory;
    int saves = 0;
    spark::SettingsWindow settings;
    settings.Open(owner, config, [&](const auto&) { ++saves; });
    HWND page = FindWindowExW(owner, nullptr, L"SparkSettingsWindow", nullptr);
    Check(GetDlgItem(page, 454) == nullptr, "settings has no leftover save button");
    const auto task_folder = spark::JoinPath(directory, "task-output");
    SetWindowTextW(GetDlgItem(page, 462), spark::Utf8ToWide(task_folder).c_str());
    SendMessageW(page, WM_COMMAND, MAKEWPARAM(462, EN_KILLFOCUS),
                 reinterpret_cast<LPARAM>(GetDlgItem(page, 462)));
    const auto saved = spark::LoadConfig(config.config_path, false);
    Check(saves == 1 && saved.ok &&
              spark::AbsolutePath(spark::ResolveConfigPath(saved.value.workspace.path, config.config_path), directory) ==
                  task_folder,
          "editing the task folder automatically saves");
    SendMessageW(page, WM_COMMAND, MAKEWPARAM(462, EN_KILLFOCUS),
                 reinterpret_cast<LPARAM>(GetDlgItem(page, 462)));
    Check(saves == 1, "unchanged text losing focus does not save again");
    settings.Close();
}

void TestDisabledMenu(HWND owner) {
    spark::AddMenu menu;
    menu.Create(owner, GetModuleHandleW(nullptr), static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
    RECT rect{30, 300, 600, 400};
    HDC device = GetDC(owner);
    const int dpi = GetDeviceCaps(device, LOGPIXELSX);
    ReleaseDC(owner, device);
    const auto scaled = [&](int value) { return MulDiv(value, dpi, 96); };
    for (const auto* theme : {"dark", "light"}) {
        menu.Show(rect, theme, L"", L"", false);
        HDC screen = GetDC(menu.hwnd());
        HDC drawing = CreateCompatibleDC(screen);
        RECT client{};
        GetClientRect(menu.hwnd(), &client);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, client.right, client.bottom);
        HGDIOBJ old = SelectObject(drawing, bitmap);
        SendMessageW(menu.hwnd(), WM_PRINTCLIENT, reinterpret_cast<WPARAM>(drawing), PRF_CLIENT);
        const auto palette = spark::theme::For(theme);
        const COLORREF icon = GetPixel(drawing, scaled(19), scaled(88));
        Check(icon != palette.text && icon != palette.panel, "disabled snapshot icon is visibly muted");
        SelectObject(drawing, old);
        SaveVisualSnapshot(bitmap, std::string(theme) == "dark" ? L"spark-add-disabled-dark.jpg" : L"spark-add-disabled-light.jpg");
        DeleteObject(bitmap);
        DeleteDC(drawing);
        ReleaseDC(menu.hwnd(), screen);
        SendMessageW(menu.hwnd(), WM_KEYDOWN, VK_DOWN, 0);
        SendMessageW(menu.hwnd(), WM_KEYDOWN, VK_DOWN, 0);
        SendMessageW(menu.hwnd(), WM_KEYDOWN, VK_RETURN, 0);
        MSG message{};
        Check(PeekMessageW(&message, owner, spark::kAddActionMessage, spark::kAddActionMessage, PM_REMOVE) &&
                  message.wParam == static_cast<WPARAM>(spark::AddAction::Goal),
              "keyboard navigation skips the disabled snapshot item");
        menu.Show(rect, theme, L"", L"", false);
        SendMessageW(menu.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(scaled(70), scaled(95)));
        Check(!PeekMessageW(&message, owner, spark::kAddActionMessage, spark::kAddActionMessage, PM_REMOVE),
              "empty snapshot row cannot dispatch an action");
        menu.Show(rect, theme, L"Fixture app", L"", false);
        SendMessageW(menu.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(scaled(70), scaled(95)));
        Check(PeekMessageW(&message, owner, spark::kAddActionMessage, spark::kAddActionMessage, PM_REMOVE) &&
                  message.wParam == static_cast<WPARAM>(spark::AddAction::Snapshot),
              "snapshot row activates when an application is available");
    }
}

void TestIsolatedCapture(HWND target, const std::string& directory) {
    spark::SnapshotCapture capture;
    const HWND foreground = GetForegroundWindow();
    Check(capture.Start(target, directory).ok, "isolated background helper starts");
    Check(!capture.Start(target, directory).ok, "duplicate snapshot requests are bounded");
    CheckSnapshot(WaitForCapture(capture));
    Check(!capture.Pending() && GetForegroundWindow() == foreground,
          "completed helper closes without activating the target");
    Check(capture.Start(target, directory).ok, "another capture can start");
    capture.Cancel();
    Check(!capture.Pending(), "explicit cancellation closes the helper");
    SetWindowTextW(target, L"Unresponsive fixture");
    Check(capture.Start(target, directory).ok, "unresponsive helper starts in isolation");
    const auto result = WaitForCapture(capture);
    if (result.ok || result.error.find("超时") == std::string::npos) {
        std::cerr << "Timeout detail: " << result.error << '\n';
    }
    Check(!result.ok && result.error.find("超时") != std::string::npos && !capture.Pending(),
          "unresponsive target times out and releases the helper");
    SetWindowTextW(target, L"Snapshot fixture");
    ShowWindow(target, SW_MINIMIZE);
    Check(!capture.Start(target, directory).ok, "minimized target is rejected without starting a helper");
    ShowWindow(target, SW_SHOWNOACTIVATE);
}
}  // namespace

namespace spark {

struct AppWindowSettingsTest {
    static void Check(bool condition, const char* message) {
        if (!condition) {
            ++failures;
            std::cerr << "FAIL: " << message << '\n';
        }
    }

    static bool HasVisibleStyle(HWND window) {
        return window != nullptr && (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0;
    }

    static int Run() {
        const auto directory =
            spark::fs::temp_directory_path() / Utf8ToWide("spark-settings-layer-" + NewSessionId());
        spark::fs::create_directories(directory);
        AppWindow app;
        app.instance_ = GetModuleHandleW(nullptr);
        app.config_path_ = JoinPath(WideToUtf8(directory.wstring()), "config.json");
        app.config_ = ConfigFromJson(ParseConfigJson(DefaultConfigExampleJson()), app.config_path_);
        app.config_.context.fallback_context_window = 272000;
        app.config_.workspace.command_approval = "deny";
        app.config_.workspace.allow_outside_reads = true;
        app.config_.providers.front().models.front().capabilities.context_window = 128000;
        app.config_.storage.data_dir = WideToUtf8(directory.wstring());
        app.config_.workspace.path = WideToUtf8(directory.wstring());
        app.active_workspace_path_ = app.config_.workspace.path;
        app.active_session_scope_ = "recent";
        app.pending_new_conversation_ = true;
        app.configuration_initialized_ = true;
        app.window_ = CreateWindowW(L"STATIC", L"Settings layer fixture",
                                    WS_OVERLAPPEDWINDOW, 0, 0, 1280, 900,
                                    nullptr, nullptr, app.instance_, nullptr);
        app.empty_mark_ = CreateWindowW(L"STATIC", L"Empty mark", WS_CHILD | WS_VISIBLE,
                                        0, 0, 20, 20, app.window_, nullptr, app.instance_, nullptr);
        app.empty_title_ = CreateWindowW(L"STATIC", L"Empty title", WS_CHILD | WS_VISIBLE,
                                         0, 0, 200, 30, app.window_, nullptr, app.instance_, nullptr);
        app.model_combo_ = CreateWindowW(L"COMBOBOX", L"", WS_CHILD, 0, 0, 100, 100,
                                         app.window_, nullptr, app.instance_, nullptr);
        app.model_button_ = CreateWindowW(L"BUTTON", L"", WS_CHILD, 0, 0, 100, 30,
                                          app.window_, nullptr, app.instance_, nullptr);
        app.chat_view_ = std::make_unique<ChatView>();
        const bool chat_created =
            app.chat_view_->Create(app.window_, app.instance_, 509,
                                   static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)),
                                   static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));
        Check(app.window_ != nullptr && app.empty_mark_ != nullptr && app.empty_title_ != nullptr && chat_created,
              "settings layering fixture controls were created");
        if (app.window_ == nullptr || app.empty_mark_ == nullptr || app.empty_title_ == nullptr || !chat_created) {
            return failures;
        }
        auto runtime = std::make_shared<AppWindow::TurnRuntime>();
        runtime->permission_mode = "full_access";
        Check(!app.AskApproval(runtime, {ApprovalKind::FileWrite, "explicit ask", "test"}),
              "full access does not bypass an explicit approval request");

        app.SetSettingsMode(true);
        app.settings_.Open(app.window_, app.config_,
                           [&](const AppConfig& saved) { app.ApplyConfiguration(saved); });
        HWND settings = FindWindowExW(app.window_, nullptr, L"SparkSettingsWindow", nullptr);
        Check(settings != nullptr && HasVisibleStyle(settings) && GetTopWindow(app.window_) == settings,
              "settings surface starts above the chat controls");
        Check(app.settings_.ChoiceValue(app.settings_.language_combo_) == "system" &&
                  ReadWindowText(app.settings_.language_combo_.button) == i18n::Text(L"Automatic"),
              "automatic system language is the first settings option on a fresh configuration");
        app.LayoutControls();
        Check(GetTopWindow(app.window_) == settings,
              "main layout does not raise chat placeholders above settings");
        Check(!HasVisibleStyle(app.empty_mark_) && !HasVisibleStyle(app.empty_title_),
              "chat empty-state controls stay hidden while settings are open");
        Check(!HasVisibleStyle(app.chat_view_->hwnd()),
              "layout does not reveal the chat transcript over settings");

        app.settings_.SetChoiceIndex(app.settings_.theme_combo_, 1);
        app.settings_.SaveGeneral();
        Check(app.config_.ui.theme == "light",
              "automatic settings save updates the live application configuration");
        const auto saved_config = LoadConfig(app.config_path_, false);
        Check(saved_config.ok &&
                  ConfigToJson(saved_config.value)["context"] == ConfigToJson(app.config_)["context"] &&
                  ConfigToJson(saved_config.value)["providers"] == ConfigToJson(app.config_)["providers"] &&
                  saved_config.value.context.fallback_context_window == 272000 &&
                  saved_config.value.providers.front().models.front().capabilities.context_window == 128000,
              "saving general settings preserves global compaction and per-model context configuration");
        Check(saved_config.ok && saved_config.value.workspace.command_approval == "deny" &&
                  saved_config.value.workspace.allow_outside_reads &&
                  app.config_.workspace.allow_outside_reads,
              "saving unrelated settings preserves advanced permission overrides");
        AppConfig network_config = app.config_;
        network_config.permissions.allow_network = true;
        app.mcp_discovered_ = true;
        app.ApplyConfiguration(network_config);
        Check(!app.mcp_discovered_ && app.mcp_ != nullptr && !app.mcp_reload_pending_,
              "network changes refresh MCP even while settings are open");
        network_config.permissions.allow_network = false;
        app.ApplyConfiguration(network_config);
        AppConfig resumed_config = network_config;
        ApplyPermissionMode(&resumed_config, "full_access");
        McpServerConfig remote;
        remote.id = "remote";
        remote.enabled = true;
        remote.transport = "streamable_http";
        remote.url = "http://127.0.0.1:1/mcp";
        resumed_config.mcp_servers.push_back(remote);
        McpManager* resumed_mcp = app.McpForTurn(resumed_config);
        Check(resumed_mcp != app.mcp_.get() && resumed_mcp->AllowsNetwork() &&
                  app.McpForTurn(resumed_config) == resumed_mcp,
              "restored full access turns select and reuse an appropriate MCP manager");
        app.external_running_sessions_.insert("pending-test");
        network_config.permissions.allow_network = true;
        app.ApplyConfiguration(network_config);
        Check(app.mcp_reload_pending_ && app.mcp_network_override_ != nullptr,
              "MCP clients stay alive while an external task is running");
        app.external_running_sessions_.clear();
        app.PollExternalRuns();
        Check(!app.mcp_reload_pending_ && app.mcp_network_override_ == nullptr && app.mcp_->AllowsNetwork(),
              "external-task polling consumes pending MCP reload after the final task ends");
        network_config.permissions.allow_network = false;
        app.ApplyConfiguration(network_config);
        app.LayoutControls();
        Check(GetTopWindow(app.window_) == settings,
              "automatic save and subsequent layout keep settings above chat");
        Check(!HasVisibleStyle(app.empty_mark_) && !HasVisibleStyle(app.empty_title_),
              "saving settings does not reveal the chat empty state");
        Check(!HasVisibleStyle(app.chat_view_->hwnd()),
              "automatic save does not reveal the chat transcript");

        app.settings_.SetChoiceIndex(app.settings_.language_combo_, 0);
        app.settings_.SaveGeneral();
        const auto saved_automatic_language = LoadConfig(app.config_path_, false);
        Check(saved_automatic_language.ok && saved_automatic_language.value.ui.language == "system" &&
                  app.config_.ui.language == "system" &&
                  app.settings_.ChoiceValue(app.settings_.language_combo_) == "system",
              "automatic language selection persists as a system preference");

        for (size_t index = 0; index < i18n::SupportedLocales().size(); ++index) {
            const auto& locale = i18n::SupportedLocales()[index];
            app.settings_.SetChoiceIndex(app.settings_.language_combo_, static_cast<int>(index + 1));
            app.settings_.SaveGeneral();
            const auto saved_language = LoadConfig(app.config_path_, false);
            Check(saved_language.ok && saved_language.value.ui.language == locale.code &&
                      app.config_.ui.language == locale.code && i18n::CurrentLocale() == locale.code,
                  "language selection applies and persists by locale code");
            Check(app.settings_.window_ == settings && app.settings_.page_ == SettingsWindow::Page::General &&
                      GetTopWindow(app.window_) == settings && !HasVisibleStyle(app.chat_view_->hwnd()),
                  "language changes preserve the existing settings page above chat");
            Check(ReadWindowText(GetDlgItem(settings, 491)) == i18n::Text(L"Default task folder") &&
                      ReadWindowText(GetDlgItem(settings, 449)) == i18n::Text(L"Back to app"),
                  "visible settings labels refresh in every language");
            Check(ReadWindowText(GetDlgItem(settings, 469)) == locale.name,
                  "language selector displays native names after applying a language");
            for (int id : {463, 466, 476, 477, 469}) {
                HWND button = GetDlgItem(settings, id);
                HDC device = GetDC(button);
                const auto old_font = SelectObject(device, reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0)));
                const std::wstring text = ReadWindowText(button);
                SIZE size{};
                GetTextExtentPoint32W(device, text.c_str(), static_cast<int>(text.size()), &size);
                SelectObject(device, old_font);
                ReleaseDC(button, device);
                RECT bounds{};
                GetClientRect(button, &bounds);
                if (size.cx + app.settings_.Dpi(id == 469 ? 32 : 18) > bounds.right) {
                    std::cerr << "Button overflow: locale=" << locale.code << " id=" << id
                              << " text=" << WideToUtf8(text) << " width=" << size.cx
                              << " available=" << bounds.right << '\n';
                }
                Check(size.cx + app.settings_.Dpi(id == 469 ? 32 : 18) <= bounds.right,
                      "translated button labels fit their control bounds");
            }
        }
        app.settings_.SwitchPage(SettingsWindow::Page::Archived);
        app.settings_.SetChoiceIndex(app.settings_.language_combo_, 4);
        app.settings_.SaveGeneral();
        Check(app.settings_.page_ == SettingsWindow::Page::Archived && app.settings_.window_ == settings &&
                  GetTopWindow(app.window_) == settings &&
                  ReadWindowText(GetDlgItem(settings, 451)) == i18n::Text(L"Archived chats"),
              "changing language preserves the archived page and its title");

        const std::string language = app.config_.ui.language;
        app.settings_.Close();
        app.settings_.Open(app.window_, app.config_,
                           [&](const AppConfig& saved) { app.ApplyConfiguration(saved); });
        Check(app.settings_.config_.ui.language == language &&
                  app.settings_.ChoiceValue(app.settings_.language_combo_) == language,
              "reopening settings restores the saved language selection");
        AppConfig provider_update = app.config_;
        ProviderConfig added = provider_update.providers.front();
        added.id = "settings-added-provider";
        added.models.front().display_name = "Settings added model";
        provider_update.active = {added.id, added.models.front().id, ""};
        provider_update.providers.push_back(added);
        app.ApplyConfiguration(provider_update);
        size_t model_count = 0;
        for (const auto& provider : provider_update.providers) model_count += provider.models.size();
        Check(SendMessageW(app.model_combo_, CB_GETCOUNT, 0, 0) == model_count &&
                  ReadWindowText(app.model_button_).find(Utf8ToWide(added.models.front().id)) != std::wstring::npos &&
                  ReadWindowText(app.model_button_).find(L"Settings added model") == std::wstring::npos &&
                  !HasVisibleStyle(app.model_button_) && GetTopWindow(app.window_) == app.settings_.window_,
              "provider saves refresh model choices immediately without revealing chat over settings");
        app.settings_.Close();
        DestroyWindow(app.window_);
        app.window_ = nullptr;
        app.empty_mark_ = nullptr;
        app.empty_title_ = nullptr;
        std::error_code error;
        spark::fs::remove_all(directory, error);
        return failures;
    }

    inline static int failures = 0;
};

}  // namespace spark

int main() {
    // Simulate a renderer stuck inside an external application. The parent must
    // terminate this test helper; no system window or production config is used.
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    const bool stalled = arguments && count == 4 && wcscmp(arguments[1], L"--spark-capture-window") == 0 &&
        spark::ReadWindowText(reinterpret_cast<HWND>(wcstoull(arguments[2], nullptr, 10))) == L"Unresponsive fixture";
    const bool settings_layer_only =
        arguments && count == 2 && wcscmp(arguments[1], L"--settings-layer-only") == 0;
    if (arguments) LocalFree(arguments);
    if (stalled) Sleep(6000);
    if (const auto helper = spark::RunSnapshotHelper()) return *helper;
    SetProcessDPIAware();
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
    WNDCLASSW cls{};
    cls.lpfnWndProc = FixtureProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"SparkSnapshotFixture";
    RegisterClassW(&cls);
    if (settings_layer_only) {
        if (spark::AppWindowSettingsTest::Run() != 0) ++failures;
        Gdiplus::GdiplusShutdown(token);
        return failures ? 1 : 0;
    }
    const auto directory = spark::fs::temp_directory_path() / spark::Utf8ToWide("spark-background-" + spark::NewSessionId());
    spark::fs::create_directories(directory);
    HWND target = CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, L"Snapshot fixture",
                                  WS_POPUP | WS_VISIBLE, 30, 30, 320, 180, nullptr, nullptr, cls.hInstance, nullptr);
    UpdateWindow(target);
    HWND cover = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Covering window",
                                 WS_POPUP | WS_VISIBLE, 30, 30, 320, 180, nullptr, nullptr, cls.hInstance, nullptr);
    SetWindowPos(cover, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    const HWND foreground = GetForegroundWindow();
    CheckSnapshot(spark::CaptureSmartSnapshot(target, spark::JoinPath(spark::WideToUtf8(directory.wstring()), "direct.jpg")));
    Check(GetForegroundWindow() == foreground, "capturing leaves the foreground window unchanged");
    TestIsolatedCapture(target, spark::WideToUtf8(directory.wstring()));
    TestDisabledMenu(cover);
    HWND owner = CreateWindowW(L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0, 1280, 900,
                               nullptr, nullptr, cls.hInstance, nullptr);
    TestSettings(owner, spark::WideToUtf8(directory.wstring()));
    if (spark::AppWindowSettingsTest::Run() != 0) ++failures;
    DestroyWindow(owner);
    DestroyWindow(cover);
    DestroyWindow(target);
    UnregisterClassW(cls.lpszClassName, cls.hInstance);
    std::error_code error;
    spark::fs::remove_all(directory, error);
    Gdiplus::GdiplusShutdown(token);
    if (failures == 0) std::cout << "Snapshot and settings checks passed.\n";
    return failures ? 1 : 0;
}
