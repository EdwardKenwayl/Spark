#include "gui/settings_window.h"
#include "platform/window_text.h"

#include <commctrl.h>
#include "platform/filesystem.h"
#include <iostream>

namespace spark {
struct SettingsLayoutTest {
static int Run() {
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
    };
    const auto directory = fs::temp_directory_path() /
        ("spark-settings-layout-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(directory);
    AppConfig config;
    config.ui.language = "en";
    config.ui.shell = "C:/fixture/tools/custom-shell.exe";
    config.config_path = WideToUtf8((directory / "config.json").wstring());
    config.storage.data_dir = WideToUtf8((directory / "data").wstring());
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    HWND owner = CreateWindowW(L"STATIC", L"settings-layout-fixture", WS_OVERLAPPEDWINDOW,
        0, 0, 720, 280, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    SettingsWindow settings;
    settings.Open(owner, config, {});
    const auto rect = [&](HWND control) {
        RECT value{}; GetWindowRect(control, &value);
        MapWindowPoints(nullptr, settings.window_, reinterpret_cast<POINT*>(&value), 2);
        return value;
    };
    const auto shown = [](HWND control) { return (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0; };
    check(settings.ChoiceValue(settings.shell_combo_) == config.ui.shell,
          "opening preferences preserves a custom configured shell");
    settings.ApplyLocalization();
    check(settings.ChoiceValue(settings.shell_combo_) == config.ui.shell,
          "relocalizing preferences preserves custom shell selection");
    check(settings.general_scroll_max_ > 0 && shown(settings.general_scrollbar_),
          "short General pages expose a scrollbar");
    SendMessageW(settings.window_, WM_VSCROLL, SB_BOTTOM, reinterpret_cast<LPARAM>(settings.general_scrollbar_));
    RECT client{}; GetClientRect(settings.window_, &client);
    check(settings.general_scroll_ == settings.general_scroll_max_ &&
              rect(settings.clean_cache_).bottom <= client.bottom &&
              rect(settings.status_).bottom <= client.bottom,
          "scrolling to the end exposes cache controls and status");
    const int bottom = settings.general_scroll_;
    SendMessageW(settings.window_, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
    check(settings.general_scroll_ < bottom, "mouse wheel scrolls General preferences");
    for (HWND path : {settings.config_path_, settings.data_path_}) {
        check((GetWindowLongPtrW(path, GWL_STYLE) & SS_NOTIFY) != 0,
              "path labels receive mouse events for hover disclosure");
        NMTTDISPINFOW tip{};
        tip.hdr.hwndFrom = settings.archive_tooltip_;
        tip.hdr.idFrom = reinterpret_cast<UINT_PTR>(path);
        tip.hdr.code = TTN_GETDISPINFOW;
        SendMessageW(settings.window_, WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&tip));
        check(tip.lpszText && std::wstring(tip.lpszText) == ReadWindowText(path),
              "ellipsized path labels disclose the full path in tooltips");
    }
    settings.SwitchPage(SettingsWindow::Page::Archived);
    check(!shown(settings.general_scrollbar_), "General scrollbar is hidden on other pages");
    settings.SwitchPage(SettingsWindow::Page::General);
    SetWindowPos(owner, nullptr, 0, 0, 1100, 850, SWP_NOZORDER | SWP_NOACTIVATE);
    settings.Resize();
    check(settings.general_scroll_ == 0 && settings.general_scroll_max_ == 0 &&
              !shown(settings.general_scrollbar_),
          "expanding the window resets obsolete General scrolling");
    check(!fs::exists(directory / "config.json"), "layout and navigation do not rewrite configuration");
    ProviderConfig provider;
    provider.id = "fixture";
    provider.base_url = "https://example.invalid/v1";
    ModelConfig model;
    model.id = "fixture-model";
    provider.models.push_back(model);
    settings.config_.providers = {provider};
    settings.config_.active.provider = provider.id;
    settings.config_.active.model = model.id;
    settings.SetChoiceIndex(settings.send_key_combo_, 1);
    settings.SaveGeneral();
    const auto saved = ReadFileUtf8(config.config_path);
    check(saved && ParseConfigJson(*saved)["ui"]["shell"] == config.ui.shell,
          "saving another preference never replaces the custom shell");
    settings.Close(); DestroyWindow(owner);
    std::error_code error; fs::remove_all(directory, error);
    if (!failures) std::cout << "Settings layout checks passed.\n";
    return failures ? 1 : 0;
}
};
}  // namespace spark

int main() { return spark::SettingsLayoutTest::Run(); }
