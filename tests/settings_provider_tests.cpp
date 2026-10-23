#include <winsock2.h>
#include "gui/settings_window.h"
#include "gui/localization.h"
#include "core/session.h"
#include "platform/window_text.h"

#include <commctrl.h>
#include <objidl.h>
using byte = BYTE;
#include <gdiplus.h>
#include "platform/filesystem.h"
#include <iostream>
#include <thread>

namespace {
class ModelsServer {
public:
    explicit ModelsServer(std::string body, int status = 200) {
        WSADATA data{}; WSAStartup(MAKEWORD(2, 2), &data);
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)); listen(socket_, 1);
        int size = sizeof(address); getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size);
        url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/v1";
        thread_ = std::thread([this, body, status] {
            fd_set set; FD_ZERO(&set); FD_SET(socket_, &set); timeval timeout{3, 0};
            if (select(0, &set, nullptr, nullptr, &timeout) <= 0) return;
            SOCKET client = accept(socket_, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            DWORD wait = 1000; setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&wait), sizeof(wait));
            char request[4096]{}; recv(client, request, sizeof(request), 0);
            Sleep(200);
            const auto response = "HTTP/1.1 " + std::to_string(status) + " Fixture\r\nContent-Length: " + std::to_string(body.size()) +
                "\r\nConnection: close\r\n\r\n" + body;
            send(client, response.c_str(), static_cast<int>(response.size()), 0); closesocket(client);
        });
    }
    ~ModelsServer() { if (thread_.joinable()) thread_.join(); closesocket(socket_); WSACleanup(); }
    std::string url;
private:
    SOCKET socket_ = INVALID_SOCKET;
    std::thread thread_;
};

template <typename Predicate>
bool PumpUntil(Predicate predicate, DWORD timeout = 3000) {
    const auto deadline = GetTickCount64() + timeout;
    while (GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (predicate()) return true;
        Sleep(5);
    }
    return predicate();
}
}  // namespace

namespace spark {
struct SettingsProviderTest {
static LRESULT CALLBACK CaptionProbe(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                      UINT_PTR, DWORD_PTR data) {
    if (message == WM_SETTEXT) ++*reinterpret_cast<int*>(data);
    return DefSubclassProc(window, message, wparam, lparam);
}
struct PercentMenuProbe { bool opened = false, populated = false, numeric_default = false; };
static LRESULT CALLBACK PercentMenuProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                       UINT_PTR, DWORD_PTR data) {
    if (message == WM_INITMENUPOPUP) {
        auto& probe = *reinterpret_cast<PercentMenuProbe*>(data);
        HMENU menu = reinterpret_cast<HMENU>(wparam);
        probe.opened = true;
        wchar_t caption[64]{};
        MENUITEMINFOW item{sizeof(item)}; item.fMask = MIIM_STRING;
        item.dwTypeData = caption; item.cch = static_cast<UINT>(std::size(caption));
        probe.populated = GetMenuItemCount(menu) >= 11 && GetMenuItemInfoW(menu, 103, FALSE, &item) &&
            std::wstring(caption) == L"75%";
        item.cch = static_cast<UINT>(std::size(caption));
        probe.numeric_default = GetMenuItemInfoW(menu, 1, FALSE, &item) &&
            std::wstring(caption).find(L"82%") != std::wstring::npos;
        SetTimer(window, 99, 10, [](HWND owner, UINT, UINT_PTR id, DWORD) {
            KillTimer(owner, id); EndMenu();
        });
    }
    return DefSubclassProc(window, message, wparam, lparam);
}
static void Click(HWND list, POINT point) {
    SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
    SendMessageW(list, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
}

static POINT Action(SettingsWindow& settings, int row, int action) {
    const RECT rect = settings.ProviderActionRect(row, action);
    return {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
}

static bool ContextHintVisible(ProviderEditor& form) {
    HWND field = form.rows_[0].context;
    RECT rect{}; GetClientRect(field, &rect);
    HDC dc = GetDC(field), buffer = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, rect.right, rect.bottom);
    HGDIOBJ previous = SelectObject(buffer, bitmap);
    FillRect(buffer, &rect, form.input_);
    SendMessageW(field, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(buffer), PRF_CLIENT);
    bool found = false;
    for (int y = 0; y < rect.bottom && !found; ++y)
        for (int x = 0; x < std::min(100L, rect.right) && !found; ++x)
            found = GetPixel(buffer, x, y) == form.palette_.muted;
    SelectObject(buffer, previous); DeleteObject(bitmap); DeleteDC(buffer); ReleaseDC(field, dc);
    return found;
}

static bool ProtocolButtonsFit(ProviderEditor& form) {
    RECT bounds{}; GetClientRect(form.body_, &bounds);
    HDC dc = GetDC(form.body_);
    HGDIOBJ old = SelectObject(dc, form.font_);
    std::vector<RECT> placed;
    bool fits = true;
    for (const wchar_t* caption : {L"Responses API", L"Chat Completions", L"Anthropic Messages"}) {
        HWND button = FindWindowExW(form.body_, nullptr, L"BUTTON", caption);
        if (!button) { fits = false; continue; }
        RECT rect{}; GetWindowRect(button, &rect);
        MapWindowPoints(nullptr, form.body_, reinterpret_cast<POINT*>(&rect), 2);
        SIZE text{}; GetTextExtentPoint32W(dc, caption, static_cast<int>(wcslen(caption)), &text);
        fits = fits && rect.left >= 0 && rect.right <= bounds.right &&
            rect.right - rect.left >= text.cx + form.Scale(16);
        for (const RECT& previous : placed) {
            RECT overlap{};
            fits = fits && !IntersectRect(&overlap, &previous, &rect);
        }
        placed.push_back(rect);
    }
    SelectObject(dc, old); ReleaseDC(form.body_, dc);
    return fits;
}

static bool ProtocolButtonSelected(ProviderEditor& form, HWND button) {
    if (!button) return false;
    RECT rect{}; GetClientRect(button, &rect);
    HDC dc = GetDC(button), buffer = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, rect.right, rect.bottom);
    HGDIOBJ previous = SelectObject(buffer, bitmap);
    DRAWITEMSTRUCT item{};
    item.hwndItem = button; item.hDC = buffer; item.rcItem = rect;
    form.DrawButton(item);
    const bool selected = GetPixel(buffer, form.Scale(4), rect.bottom / 2) == form.palette_.selection;
    SelectObject(buffer, previous); DeleteObject(bitmap); DeleteDC(buffer); ReleaseDC(button, dc);
    return selected;
}

static void Snapshot(HWND window, const wchar_t* name) {
    const wchar_t* folder = _wgetenv(L"SPARK_PROVIDER_SNAPSHOT_DIR");
    if (!folder) return;
    HWND root = GetAncestor(window, GA_ROOT);
    const bool popup = (GetWindowLongPtrW(root, GWL_STYLE) & WS_POPUP) != 0;
    if (!popup) SetWindowPos(root, HWND_BOTTOM, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RECT rect{};
    GetClientRect(window, &rect);
    HDC device = GetDC(window), buffer = CreateCompatibleDC(device);
    HBITMAP bitmap = CreateCompatibleBitmap(device, rect.right, rect.bottom);
    HGDIOBJ previous = SelectObject(buffer, bitmap);
    SendMessageW(window, WM_PRINT, reinterpret_cast<WPARAM>(buffer), PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    UINT count = 0, bytes = 0;
    Gdiplus::GetImageEncodersSize(&count, &bytes);
    std::vector<BYTE> storage(bytes);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    if (Gdiplus::GetImageEncoders(count, bytes, encoders) == Gdiplus::Ok) {
        Gdiplus::Bitmap image(bitmap, nullptr);
        for (UINT index = 0; index < count; ++index) {
            if (wcscmp(encoders[index].MimeType, L"image/jpeg") == 0)
                image.Save((spark::fs::path(folder) / name).c_str(), &encoders[index].Clsid);
        }
    }
    SelectObject(buffer, previous);
    DeleteObject(bitmap);
    DeleteDC(buffer);
    ReleaseDC(window, device);
    if (!popup) ShowWindow(root, SW_HIDE);
}

static int Run() {
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    const auto directory = spark::fs::temp_directory_path() / Utf8ToWide("spark-providers-" + NewSessionId());
    spark::fs::create_directories(directory);
    AppConfig config;
    config.config_path = WideToUtf8((directory / "config.json").wstring());
    config.storage.data_dir = WideToUtf8(directory.wstring());
    config.ui.language = "zh-CN";
    ProviderConfig provider;
    provider.id = "first";
    provider.label = "Example Provider";
    provider.protocol = "responses";
    provider.base_url = "https://example.invalid/v1";
    config.providers.push_back(provider);
    provider.id = "second";
    provider.label.clear();
    config.providers.push_back(provider);
    const auto original = ConfigToJson(config);
    HWND owner = CreateWindowW(L"STATIC", L"Provider settings fixture", WS_OVERLAPPEDWINDOW,
                               0, 0, 1280, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    int failures = 0, saves = 0;
    const auto check = [&](bool success, const char* message) {
        if (!success) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    const auto visible = [](HWND control) {
        return (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0;
    };
    SettingsWindow settings;
    settings.Open(owner, config, [&](const AppConfig&) { ++saves; });
    HWND surface = FindWindowExW(owner, nullptr, L"SparkSettingsWindow", nullptr);
    HWND nav = GetDlgItem(surface, 450);
    check(GetDlgItem(surface, 460) == nullptr && GetDlgItem(surface, 461) == nullptr && GetDlgItem(surface, 467) == nullptr &&
              GetDlgItem(surface, 468) == nullptr && !visible(settings.model_status_),
          "general settings omit the default permission selector and configuration actions");
    check(SendMessageW(nav, LB_GETCOUNT, 0, 0) == 3, "settings navigation contains three pages");
    const auto check_general_cards = [&] {
        const auto contained = [&](HWND control, const RECT& card) {
            RECT rect{}; GetWindowRect(control, &rect);
            MapWindowPoints(nullptr, surface, reinterpret_cast<POINT*>(&rect), 2);
            return rect.left >= card.left && rect.top >= card.top &&
                rect.right <= card.right && rect.bottom <= card.bottom;
        };
        check(contained(settings.workspace_path_, settings.general_cards_[0]) &&
                  contained(settings.choose_workspace_, settings.general_cards_[0]) &&
                  contained(settings.data_path_, settings.general_cards_[2]) &&
                  contained(settings.clean_cache_, settings.general_cards_[2]),
              "general path controls remain inside their painted cards");
        for (const auto* choice : {&settings.language_combo_, &settings.theme_combo_,
                                  &settings.shell_combo_, &settings.send_key_combo_})
            check(contained(choice->button, settings.general_cards_[1]),
                  "all preference controls remain inside the same painted card");
    };
    check_general_cards();
    Snapshot(surface, L"general-dark.jpg");
    SetWindowPos(owner, nullptr, 0, 0, 720, 640, SWP_NOZORDER | SWP_NOACTIVATE);
    settings.Resize(); check_general_cards();
    RECT small_general{}; GetClientRect(surface, &small_general);
    check(settings.general_cards_[2].bottom <= small_general.bottom,
          "all general settings cards fit the narrow window without hiding the data controls");
    Snapshot(surface, L"general-dark-small.jpg");
    SetWindowPos(owner, nullptr, 0, 0, 1280, 900, SWP_NOZORDER | SWP_NOACTIVATE);
    settings.Resize();
    SendMessageW(nav, LB_SETCURSEL, 1, 0);
    SendMessageW(surface, WM_COMMAND, MAKEWPARAM(450, LBN_SELCHANGE), reinterpret_cast<LPARAM>(nav));
    check(ReadWindowText(GetDlgItem(surface, 451)) == L"供应商配置",
          "the middle navigation item opens provider configuration");
    check(visible(settings.provider_list_) && visible(settings.add_provider_) &&
              !visible(settings.provider_empty_) && !visible(GetDlgItem(surface, 460)) &&
              !visible(GetDlgItem(surface, 481)),
          "only provider controls are visible on the provider page");
    check(SendMessageW(settings.provider_list_, LB_GETCOUNT, 0, 0) == 2, "the list reads all configured providers");
    RECT model_status{}, config_path{}, list_bounds{};
    GetWindowRect(settings.model_status_, &model_status);
    GetWindowRect(settings.config_path_, &config_path);
    GetWindowRect(settings.provider_list_, &list_bounds);
    check(visible(settings.model_status_) && visible(settings.config_path_) && visible(settings.open_config_) &&
              model_status.bottom < config_path.top && config_path.bottom < list_bounds.top &&
              ReadWindowText(settings.config_path_) == Utf8ToWide(config.config_path),
          "model configuration status and the absolute config path appear above providers");
    wchar_t label[128]{};
    SendMessageW(settings.provider_list_, LB_GETTEXT, 1, reinterpret_cast<LPARAM>(label));
    check(std::wstring(label) == L"second", "an unnamed provider displays its configured ID");
    RECT item{};
    SendMessageW(settings.provider_list_, LB_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&item));
    for (int action = 0; action < 5; ++action) {
        const RECT rect = settings.ProviderActionRect(0, action);
        const POINT point{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
        SendMessageW(settings.provider_list_, WM_MOUSEMOVE, 0, MAKELPARAM(point.x, point.y));
        check(settings.provider_hover_row_ == 0 && settings.provider_hover_action_ == action &&
                  settings.ProviderActionAt(point) == action,
              "the drag handle and four action icons have separate hover targets");
        wchar_t tip[256]{};
        TOOLINFOW tool{TTTOOLINFOW_V2_SIZE};
        tool.hwnd = settings.provider_list_; tool.uId = action + 1; tool.lpszText = tip;
        SendMessageW(settings.provider_tooltip_, TTM_GETTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
        check(tip[0] != L'\0', "all five icons have localized hover descriptions");
        if (action == 4) Snapshot(surface, L"providers-dark-hover.jpg");
        if (action != 2) SendMessageW(settings.provider_list_, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
    }
    SendMessageW(settings.add_provider_, BM_CLICK, 0, 0);
    HWND editor = FindWindowExW(surface, nullptr, L"SparkProviderEditor", nullptr);
    check(editor != nullptr && visible(editor), "adding a provider opens an in-page editor");
    if (editor) SendMessageW(GetDlgItem(editor, 510), BM_CLICK, 0, 0);
    SendMessageW(settings.provider_list_, WM_MOUSELEAVE, 0, 0);
    check(settings.provider_hover_row_ == -1 && settings.provider_hover_action_ == -1,
          "leaving a provider hides its action icons");
    check(settings.ProviderRowAt({20, item.bottom - 1}) == -1,
          "the gap between cards is not a provider hover target");
    settings.SwitchPage(SettingsWindow::Page::Archived);
    check(SendMessageW(nav, LB_GETCURSEL, 0, 0) == 2 && !visible(settings.provider_list_) &&
              visible(GetDlgItem(surface, 481)), "archived chats remains the third, independent settings page");
    settings.SwitchPage(SettingsWindow::Page::General);
    check(visible(settings.workspace_path_) && !visible(settings.add_provider_) &&
              !visible(settings.model_status_) && !visible(settings.config_path_),
          "general controls return without provider UI overlapping them");
    settings.SwitchPage(SettingsWindow::Page::Providers);
    check(settings.window_ == surface, "page switches reuse the same settings surface");
    check(saves == 0 && ConfigToJson(settings.config_) == original && !PathExists(config.config_path),
          "hover, unpaired releases, and page navigation do not write configuration");

    settings.config_.ui.theme = "light";
    settings.ApplyTheme();
    settings.config_.ui.language = "de-DE";
    settings.ApplyLocalization();
    check(settings.page_ == SettingsWindow::Page::Providers &&
              ReadWindowText(GetDlgItem(surface, 451)) == i18n::Text(L"Provider configuration"),
          "theme and language refresh preserve the provider page");
    SetWindowPos(owner, nullptr, 0, 0, 720, 640, SWP_NOZORDER | SWP_NOACTIVATE);
    settings.Resize();
    RECT bounds{}, list{};
    GetClientRect(surface, &bounds);
    GetWindowRect(settings.provider_list_, &list);
    MapWindowPoints(nullptr, surface, reinterpret_cast<POINT*>(&list), 2);
    check(list.right <= bounds.right && list.bottom <= bounds.bottom &&
              list.bottom - list.top >= SendMessageW(settings.provider_list_, LB_GETITEMHEIGHT, 0, 0),
          "provider metadata leaves a visible row in a small window");
    Snapshot(surface, L"providers-light-small.jpg");
    settings.SetStatus(L"fixture-model: HTTP 200\nhi");
    RECT probe_status{};
    GetWindowRect(settings.provider_list_, &list);
    GetWindowRect(settings.provider_status_, &probe_status);
    MapWindowPoints(nullptr, surface, reinterpret_cast<POINT*>(&list), 2);
    MapWindowPoints(nullptr, surface, reinterpret_cast<POINT*>(&probe_status), 2);
    check(list.bottom <= probe_status.top && probe_status.bottom <= bounds.bottom &&
              probe_status.bottom - probe_status.top >= settings.Dpi(36),
          "hi results remain visible below the list in a small window");
    settings.SetStatus(L"");
    for (int index = 0; index < 30; ++index) settings.config_.providers.push_back(provider);
    settings.RefreshProviders();
    bool saw_compact = false, saw_regular = false;
    for (int target_width : {settings.Dpi(480) + GetSystemMetrics(SM_CXVSCROLL) - 1,
                             settings.Dpi(480) + GetSystemMetrics(SM_CXVSCROLL) + 1}) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            RECT owner_bounds{}, list_bounds{};
            GetWindowRect(owner, &owner_bounds); GetWindowRect(settings.provider_list_, &list_bounds);
            const int width = owner_bounds.right - owner_bounds.left +
                target_width - (list_bounds.right - list_bounds.left);
            SetWindowPos(owner, nullptr, 0, 0, width, settings.Dpi(700), SWP_NOZORDER | SWP_NOACTIVATE);
            settings.Resize();
        }
        RECT row{};
        SendMessageW(settings.provider_list_, LB_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&row));
        const auto rect = settings.ProviderActionRect(0, 1);
        const bool compact = SendMessageW(settings.provider_list_, LB_GETITEMHEIGHT, 0, 0) == settings.Dpi(112);
        saw_compact = saw_compact || compact;
        saw_regular = saw_regular || !compact;
        check((row.right - row.left >= settings.Dpi(480) || compact) &&
                  (!compact || rect.top >= row.top + settings.Dpi(70)),
              "scrollbar width cannot mix compact icons with short rows or overlap provider details");
    }
    check(saw_compact && saw_regular, "DPI-aware boundary cases exercise both provider row layouts");
    SetWindowPos(owner, HWND_BOTTOM, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetFocus(nav);
    Click(settings.provider_list_, {settings.Dpi(90), settings.Dpi(20)});
    check(GetFocus() == settings.provider_list_, "clicking provider text focuses the list for Win7 mouse-wheel scrolling");
    ShowWindow(owner, SW_HIDE);
    SendMessageW(settings.provider_list_, LB_SETTOPINDEX, 10, 0);
    const int top = static_cast<int>(SendMessageW(settings.provider_list_, LB_GETTOPINDEX, 0, 0));
    const RECT action_rect = settings.ProviderActionRect(top, 4);
    const POINT action_point{(action_rect.left + action_rect.right) / 2, (action_rect.top + action_rect.bottom) / 2};
    check(top > 0 && settings.ProviderRowAt(action_point) == top && settings.ProviderActionAt(action_point) == 4,
          "scrolled provider actions map to the correct row");
    settings.config_.providers.clear();
    settings.RefreshProviders();
    settings.SwitchPage(SettingsWindow::Page::Providers);
    check(visible(settings.provider_empty_) && !visible(settings.provider_list_) && visible(settings.add_provider_),
          "the empty state leaves the add button available");

    settings.config_ = config;
    settings.config_.context.fallback_context_window = 128000;
    settings.config_.context.auto_compact_threshold_percent = 82;
    settings.config_.workspace.path = WideToUtf8(directory.wstring());
    ModelConfig model;
    model.id = model.display_name = "original-model";
    model.capabilities.reasoning_efforts = {"low", "high"};
    model.capabilities.default_reasoning_effort = "high";
    for (auto& entry : settings.config_.providers) entry.models = {model};
    settings.config_.active = {"first", model.id, "high"};
    settings.config_.providers[0].api_key_expression = "${ENV:SPARK_PROVIDER_TEST_KEY_NOT_SET}";
    settings.config_.providers[0].headers["X-Test"] = "keep";
    settings.config_.providers[1].protocol = "anthropic_messages";
    settings.ApplyTheme(); settings.ApplyLocalization();
    settings.RefreshProviders();
    settings.config_.active = {"first", "summary-active-marker", ""};
    settings.RefreshModelStatus();
    check(ReadWindowText(settings.model_status_).find(L"summary-active-marker") == std::wstring::npos,
          "configuration status does not expose an active-model selection");
    settings.config_.active = {"first", model.id, "high"};
    settings.RefreshModelStatus();
    SetWindowPos(owner, nullptr, 0, 0, 1600, 1100, SWP_NOZORDER | SWP_NOACTIVATE);
    settings.Resize();
    const auto untouched = ConfigToJson(settings.config_)["providers"]["first"];
    SendMessageW(settings.add_provider_, BM_CLICK, 0, 0);
    auto& form = settings.provider_editor_;
    check(form.Window() && form.rows_.size() == 1 && !visible(settings.provider_list_) &&
              !visible(settings.model_status_) && !visible(settings.config_path_) && !visible(settings.open_config_),
          "the add button opens a single empty model draft and hides the provider list");
    SetWindowTextW(form.rows_[0].id, L"protocol-model");
    form.RefreshPreview();
    HWND anthropic = FindWindowExW(form.body_, nullptr, L"BUTTON", L"Anthropic Messages");
    check(visible(anthropic) && ProtocolButtonsFit(form),
          "all three upstream protocols are visible without clipped or overlapping buttons");
    SendMessageW(anthropic, BM_CLICK, 0, 0);
    check(form.ReadDraft(false).value.protocol == "anthropic_messages" &&
              ParseConfigJson(WideToUtf8(ReadWindowText(form.preview_)))["providers"][form.draft_.id]
                  ["protocol"] == "anthropic_messages" &&
              ProtocolButtonSelected(form, anthropic) && !ProtocolButtonSelected(form, form.chat_) &&
              !ProtocolButtonSelected(form, form.responses_),
          "selecting Anthropic updates the draft, JSON preview and exclusive selected state");
    Snapshot(surface, L"provider-anthropic-dark.jpg");
    for (const char* protocol : {"openai_chat_completions", "openai_responses", "anthropic_messages"}) {
        auto document = ParseConfigJson(WideToUtf8(ReadWindowText(form.preview_)));
        document["providers"][form.draft_.id]["protocol"] = protocol;
        SetWindowTextW(form.preview_, Utf8ToWide(document.dump(2)).c_str());
        form.DraftChanged(true);
        SendMessageW(form.Window(), WM_TIMER, 2, 0);
        HWND selected = std::string(protocol) == "anthropic_messages" ? anthropic :
            std::string(protocol) == "openai_responses" ? form.responses_ : form.chat_;
        check(form.draft_.protocol == protocol && ProtocolButtonSelected(form, selected) &&
                  ReadWindowText(form.chat_) == L"Chat Completions",
              "loading protocol aliases or Anthropic JSON selects the matching button without relabeling Chat");
    }
    SendMessageW(form.responses_, BM_CLICK, 0, 0);
    SetWindowTextW(form.rows_[0].id, L"");
    form.RefreshPreview();
    {
        auto& row = form.rows_[0];
        check(form.context_.fallback_context_window == 128000 && ContextHintVisible(form) &&
                  ReadWindowText(row.context).empty() &&
                  ReadWindowText(row.auto_compact).find(L"82%") != std::wstring::npos,
              "empty model settings display the effective global defaults without storing overrides");
        Snapshot(surface, L"provider-effective-defaults.jpg");
        SetWindowTextW(row.id, L"deepseek-v4.1-flash");
        form.RefreshPreview();
        check(form.ReadDraft(false).ok &&
                  form.ReadDraft(false).value.models[0].capabilities.auto_compact_threshold_percent == 0,
              "a localized default compaction button is stored as an integer, not parsed as English text");
        row.auto_compact_percent = 75; form.SetAutoCompactButton(row); form.RefreshPreview();
        check(form.ReadDraft(false).value.models[0].capabilities.auto_compact_threshold_percent == 75 &&
                  ParseConfigJson(WideToUtf8(ReadWindowText(form.preview_)))["providers"][form.draft_.id]
                      ["models"][0]["capabilities"]["auto_compact_threshold_percent"] == 75,
              "percentage selection updates the model and JSON preview together");
        row.auto_compact_percent = 0; form.SetAutoCompactButton(row);
        PercentMenuProbe menu_probe;
        SetWindowSubclass(form.Window(), PercentMenuProc, 99, reinterpret_cast<DWORD_PTR>(&menu_probe));
        SetWindowPos(owner, HWND_BOTTOM, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SendMessageW(row.auto_compact, BM_CLICK, 0, 0);
        RemoveWindowSubclass(form.Window(), PercentMenuProc, 99);
        ShowWindow(owner, SW_HIDE);
        check(menu_probe.opened && menu_probe.populated && menu_probe.numeric_default,
              "clicking automatic compaction opens percentages and the effective numeric default");
        SendMessageW(row.reasoning, BM_CLICK, 0, 0);
        check(form.reasoning_popup_ && form.reasoning_popup_efforts_ == std::vector<std::string>{"low", "high", "max"},
              "reasoning opens a model-specific popup using the live model ID");
        const auto click_rect = [&](const RECT& rect) {
            Click(form.reasoning_popup_, {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2});
        };
        if (form.reasoning_popup_) {
            Snapshot(form.reasoning_popup_, L"reasoning-deepseek.jpg");
            click_rect(form.reasoning_popup_chip_rects_[3]);
            check(row.model.capabilities.default_reasoning_effort == "max",
                  "default chips select an enabled model effort without closing the popup");
            click_rect(form.reasoning_popup_effort_rects_[2]);
            check(row.model.capabilities.default_reasoning_effort.empty() &&
                      EffectiveReasoningEfforts(form.ReadDraft(false).value.models[0]) ==
                          std::vector<std::string>{"low", "high"},
                  "deselecting the default effort resets the override and retains the other levels");
            click_rect(form.reasoning_popup_reset_rect_);
            check(row.model.capabilities.reasoning_efforts.empty() && row.model.capabilities.reasoning &&
                      EffectiveDefaultReasoningEffort(form.ReadDraft(false).value.models[0]) == "high",
                  "reset restores the model's built-in levels and automatic default");
            SendMessageW(form.reasoning_popup_, WM_KEYDOWN, VK_ESCAPE, 0);
            check(!form.reasoning_popup_, "Escape dismisses the reasoning popup without leaving a stale handle");
        }
        SendMessageW(form.rows_[0].reasoning, BM_CLICK, 0, 0);
        check(form.reasoning_popup_ && form.ApplyPreview() && !form.reasoning_popup_,
              "reloading the preview closes the popup before replacing model rows");
        SetWindowTextW(form.rows_[0].id, L"");
        form.RefreshPreview();
    }
    SendMessageW(form.save_, BM_CLICK, 0, 0);
    check(form.Window() && saves == 0 && !ReadWindowText(form.status_).empty(),
          "invalid drafts do not save or close");
    SetWindowTextW(form.name_, L"Added Provider");
    SetWindowTextW(form.url_, L"https://example.invalid/v1");
    SetWindowTextW(form.key_, L"dummy-key");
    SetWindowTextW(form.rows_[0].id, L"first-added-model");
    SetWindowTextW(form.rows_[0].context, L"1M");
    SendMessageW(form.chat_, BM_CLICK, 0, 0);
    SendMessageW(form.add_model_, BM_CLICK, 0, 0);
    SetWindowTextW(form.rows_[1].id, L"first-added-model");
    SendMessageW(form.save_, BM_CLICK, 0, 0);
    check(saves == 0 && form.Window(), "duplicate model IDs are rejected before writing");
    SetWindowTextW(form.rows_[1].id, L"second-added-model");
    SetWindowTextW(form.rows_[1].context, L"200K");
    form.SetImages(form.rows_[1], false);
    SetWindowTextW(form.test_model_, L"");
    check(visible(form.test_model_) && SendMessageW(form.key_, EM_GETPASSWORDCHAR, 0, 0) != 0,
          "the test model is always visible while API keys stay masked");
    Snapshot(surface, L"provider-editor-dark.jpg");
    const auto added_id = form.draft_.id;
    SendMessageW(form.save_, BM_CLICK, 0, 0);
    auto loaded = LoadConfig(config.config_path, false);
    const auto* new_provider = loaded.ok ? FindProvider(loaded.value, added_id) : nullptr;
    const auto* added = new_provider ? FindModel(*new_provider, "second-added-model") : nullptr;
    check(saves == 1 && !form.Window() && loaded.ok && added && !added->capabilities.images &&
              added->capabilities.context_window == 200000 &&
              new_provider->models[0].capabilities.context_window == 1000000 &&
              new_provider->test_model == "first-added-model" &&
              new_provider->protocol == "chat_completions" &&
              loaded.value.active.provider == "first" &&
              ConfigToJson(loaded.value)["providers"]["first"] == untouched,
          "saving adds all models without changing the active provider or unrelated credentials and capabilities");
    const RECT edit_rect = settings.ProviderActionRect(0, 2);
    const POINT edit_point{(edit_rect.left + edit_rect.right) / 2, (edit_rect.top + edit_rect.bottom) / 2};
    Click(settings.provider_list_, edit_point);
    check(form.Window() && ReadWindowText(form.name_) == L"Example Provider" &&
              ReadWindowText(form.key_) == L"${ENV:SPARK_PROVIDER_TEST_KEY_NOT_SET}",
          "the pencil opens the same editor with the original environment reference");
    SetWindowTextW(form.name_, L"Renamed Provider");
    SetWindowTextW(form.rows_[0].context, L"K");
    SendMessageW(form.save_, BM_CLICK, 0, 0);
    check(saves == 1 && form.Window(), "a suffix without a context number cannot be saved");
    SetWindowTextW(form.rows_[0].context, L"0");
    SetWindowTextW(form.rows_[0].id, L"replacement-model");
    SendMessageW(form.save_, BM_CLICK, 0, 0);
    loaded = LoadConfig(config.config_path, false);
    const auto* renamed = loaded.ok ? FindProvider(loaded.value, "first") : nullptr;
    check(saves == 2 && renamed && renamed->label == "Renamed Provider" &&
              renamed->headers.at("X-Test") == "keep" &&
              renamed->api_key_expression == "${ENV:SPARK_PROVIDER_TEST_KEY_NOT_SET}" &&
              renamed->models[0].capabilities.reasoning_efforts == model.capabilities.reasoning_efforts &&
              loaded.value.active.model == "replacement-model" && loaded.value.active.reasoning_effort == "high",
          "editing retains provider identity and hidden metadata and repairs a removed active model");
    size_t second_index = 0;
    while (second_index < settings.config_.providers.size() && settings.config_.providers[second_index].id != "second") ++second_index;
    settings.EditProvider(second_index);
    anthropic = FindWindowExW(form.body_, nullptr, L"BUTTON", L"Anthropic Messages");
    check(ProtocolButtonSelected(form, anthropic) && !ProtocolButtonSelected(form, form.chat_),
          "opening an existing Anthropic provider selects its own button");
    SendMessageW(form.chat_, BM_CLICK, 0, 0);
    auto protocol_preview = ParseConfigJson(WideToUtf8(ReadWindowText(form.preview_)));
    protocol_preview["providers"][form.draft_.id]["headers"]["X-Protocol-Pending"] = "retained";
    SetWindowTextW(form.preview_, Utf8ToWide(protocol_preview.dump(2)).c_str());
    form.DraftChanged(true);
    SendMessageW(anthropic, BM_CLICK, 0, 0);
    SendMessageW(form.save_, BM_CLICK, 0, 0);
    loaded = LoadConfig(config.config_path, false);
    const auto* anthropic_saved = loaded.ok ? FindProvider(loaded.value, "second") : nullptr;
    check(saves == 3 && anthropic_saved && anthropic_saved->protocol == "anthropic_messages" &&
              anthropic_saved->headers.at("X-Protocol-Pending") == "retained",
          "switching back to Anthropic flushes pending JSON and saves the protocol and unrelated metadata");
    settings.EditProvider(0);
    const auto before_preview = ConfigToJson(settings.config_);
    const auto disk_before_preview = ReadFileUtf8(config.config_path);
    SetWindowTextW(form.name_, L"Live preview");
    SendMessageW(form.Window(), WM_TIMER, 1, 0);
    auto preview = ParseConfigJson(WideToUtf8(ReadWindowText(form.preview_)));
    const auto preview_id = form.draft_.id;
    check(preview["providers"][preview_id]["label"] == "Live preview",
          "form edits update the current provider JSON preview");
    preview["providers"][preview_id]["base_url"] = "https://loaded.invalid/v1";
    preview["providers"][preview_id]["headers"]["X-Loaded"] = "retained";
    preview["providers"][preview_id]["models"][0]["capabilities"]["streaming"] = false;
    SetWindowTextW(form.preview_, Utf8ToWide(preview.dump(2)).c_str());
    form.DraftChanged(true);
    SendMessageW(form.Window(), WM_TIMER, 2, 0);
    check(ReadWindowText(form.url_) == L"https://loaded.invalid/v1" &&
              form.draft_.headers.at("X-Loaded") == "retained" &&
              !form.rows_[0].model.capabilities.streaming,
          "valid preview edits load into the form, including hidden provider metadata");
    preview["providers"][preview_id]["label"] = "Pending JSON";
    SetWindowTextW(form.preview_, Utf8ToWide(preview.dump(2)).c_str());
    form.DraftChanged(true);
    SendMessageW(form.add_model_, BM_CLICK, 0, 0);
    check(ReadWindowText(form.name_) == L"Pending JSON" && form.rows_.size() == 2,
          "model actions flush pending JSON instead of discarding recent preview edits");
    preview["providers"][preview_id]["headers"]["X-Pending"] = "retained";
    SetWindowTextW(form.preview_, Utf8ToWide(preview.dump(2)).c_str());
    form.DraftChanged(true);
    SetWindowTextW(form.url_, L"https://form-change.invalid/v1");
    SendMessageW(form.Window(), WM_TIMER, 1, 0);
    auto merged_preview = ParseConfigJson(WideToUtf8(ReadWindowText(form.preview_)));
    check(merged_preview["providers"][preview_id]["headers"]["X-Pending"] == "retained" &&
              ReadWindowText(form.url_) == L"https://form-change.invalid/v1",
          "typing in a form field preserves pending JSON edits to other properties");
    SendMessageW(form.Window(), WM_TIMER, 2, 0);
    check(ReadWindowText(form.url_) == L"https://form-change.invalid/v1",
          "a queued obsolete JSON timer cannot rebuild the form");
    SetWindowTextW(form.preview_, L"{invalid");
    form.DraftChanged(true);
    SendMessageW(form.Window(), WM_TIMER, 2, 0);
    SendMessageW(form.save_, BM_CLICK, 0, 0);
    check(saves == 3 && ReadWindowText(form.url_) == L"https://form-change.invalid/v1" &&
              !ReadWindowText(form.status_).empty(),
          "invalid JSON keeps the last valid draft and cannot be silently saved");
    const auto imported_path = (directory / "import.json").wstring();
    preview["providers"][preview_id]["label"] = "Loaded from file";
    WriteFileUtf8Atomic(WideToUtf8(imported_path), preview.dump(2));
    form.LoadConfiguration(imported_path);
    check(ReadWindowText(form.name_) == L"Loaded from file" && form.json_valid_,
          "loading a JSON file refreshes the draft without writing configuration");
    auto invalid_import = preview;
    invalid_import["providers"][preview_id]["models"].push_back(
        {{"id", " " + invalid_import["providers"][preview_id]["models"][0]["id"].get<std::string>() + " "}});
    WriteFileUtf8Atomic(WideToUtf8(imported_path), invalid_import.dump());
    preview["providers"][preview_id]["label"] = "Pending before failed import";
    SetWindowTextW(form.preview_, Utf8ToWide(preview.dump(2)).c_str());
    form.DraftChanged(true);
    form.LoadConfiguration(imported_path);
    check(ReadWindowText(form.name_) == L"Loaded from file" && form.json_pending_,
          "invalid imported IDs leave the last valid draft and pending JSON untouched");
    SendMessageW(form.Window(), WM_TIMER, 2, 0);
    check(ReadWindowText(form.name_) == L"Pending before failed import" && !form.json_pending_,
          "failed import restores automatic synchronization of the previous JSON draft");
    SendMessageW(form.clear_models_, BM_CLICK, 0, 0);
    preview = ParseConfigJson(WideToUtf8(ReadWindowText(form.preview_)));
    check(form.rows_.empty() && preview["providers"][preview_id]["models"].empty(),
          "clear models updates both the form and JSON preview");
    SendMessageW(form.save_, BM_CLICK, 0, 0);
    check(saves == 3 && form.Window() && ConfigToJson(settings.config_) == before_preview &&
              ReadFileUtf8(config.config_path) == disk_before_preview,
          "previewing, importing, and clearing models do not overwrite disk or unrelated settings");
    SendMessageW(form.add_model_, BM_CLICK, 0, 0);
    SetWindowTextW(form.rows_[0].id, L"preview-model");
    SendMessageW(form.Window(), WM_TIMER, 1, 0);
    form.Scroll(form.content_height_);
    Snapshot(surface, L"provider-config-preview.jpg");
    SendMessageW(form.preview_, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
    check(SendMessageW(form.preview_, EM_GETFIRSTVISIBLELINE, 0, 0) > 0,
          "the JSON preview scrolls internally with the mouse wheel");
    settings.SwitchPage(SettingsWindow::Page::Providers);
    settings.EditProvider(0);
    const auto preserved = form.rows_[0].model;
    {
        ModelsServer server(nlohmann::json{{"data", {{{"id", preserved.id}}, {{"id", "fetched-model"}},
            {{"id", "fetched-model"}}}}}.dump());
        SetWindowTextW(form.url_, Utf8ToWide(server.url).c_str());
        SetWindowTextW(form.rows_[0].context, L"1M");
        const auto started = GetTickCount64();
        SendMessageW(form.fetch_models_, BM_CLICK, 0, 0);
        check(GetTickCount64() - started < 150 && !IsWindowEnabled(form.fetch_models_),
              "fetching models starts in the background without blocking the UI");
        check(PumpUntil([&] { return IsWindowEnabled(form.fetch_models_); }) && form.rows_.size() == 2 &&
                  ReadWindowText(form.rows_[0].context) == L"1000000" &&
                  form.rows_[0].model.capabilities.reasoning_efforts == preserved.capabilities.reasoning_efforts &&
                  ReadWindowText(form.rows_[1].id) == L"fetched-model",
              "upstream IDs merge once while preserving existing model metadata");
    }
    {
        ModelsServer server(R"({"data":[{"id":"stale-model"}]})");
        SetWindowTextW(form.url_, Utf8ToWide(server.url).c_str());
        SendMessageW(form.fetch_models_, BM_CLICK, 0, 0);
        Sleep(60);
        settings.SwitchPage(SettingsWindow::Page::Providers);
        settings.EditProvider(0);
        const auto deadline = GetTickCount64() + 400;
        PumpUntil([&] { return GetTickCount64() >= deadline; });
        check(form.rows_.size() == 1 && ReadWindowText(form.url_) != Utf8ToWide(server.url),
              "a request completing after closing the editor cannot modify a new draft");
    }
    check(saves == 3 && ReadFileUtf8(config.config_path) == disk_before_preview,
          "fetching upstream models never writes the configuration before saving");
    settings.SwitchPage(SettingsWindow::Page::Providers);
    SendMessageW(settings.add_provider_, BM_CLICK, 0, 0);
    for (int index = 0; index < 10; ++index) SendMessageW(form.add_model_, BM_CLICK, 0, 0);
    int caption_updates = 0;
    const HWND watched[] = {form.add_model_, form.rows_[0].auto_compact, form.rows_[0].images, form.rows_[0].reasoning};
    for (HWND button : watched)
        SetWindowSubclass(button, CaptionProbe, 98, reinterpret_cast<DWORD_PTR>(&caption_updates));
    SendMessageW(form.body_, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
    check(form.scroll_ > 0 && caption_updates == 0,
          "scrolling does not reset unchanged button captions and trigger redundant painting");
    HDC button_dc = GetDC(form.add_model_);
    check(SendMessageW(form.add_model_, WM_ERASEBKGND, reinterpret_cast<WPARAM>(button_dc), 0) == 1 &&
              SendMessageW(form.rows_[0].auto_compact, WM_ERASEBKGND, reinterpret_cast<WPARAM>(button_dc), 0) == 1,
          "owner-drawn buttons suppress the system background erase while scrolling");
    ReleaseDC(form.add_model_, button_dc);
    for (HWND button : watched) RemoveWindowSubclass(button, CaptionProbe, 98);
    settings.config_.ui.theme = "light";
    settings.ApplyTheme();
    settings.SwitchPage(SettingsWindow::Page::Providers);
    SetWindowPos(owner, nullptr, 0, 0, 720, 640, SWP_NOZORDER | SWP_NOACTIVATE);
    settings.Resize();
    settings.EditProvider(0);
    check(form.scroll_ == 0, "opening a narrow editor starts at the first field");
    check(ProtocolButtonsFit(form), "protocol buttons wrap without clipping in a narrow editor");
    Snapshot(surface, L"provider-editor-light-small.jpg");
    RECT form_bounds{}, button_bounds{};
    GetClientRect(form.Window(), &form_bounds);
    GetWindowRect(form.save_, &button_bounds);
    MapWindowPoints(nullptr, form.Window(), reinterpret_cast<POINT*>(&button_bounds), 2);
    check(button_bounds.right <= form_bounds.right && button_bounds.left >= 0,
          "the localized save button fits a narrow editor");
    SetWindowTextW(form.name_, L"Discard this draft");
    settings.SwitchPage(SettingsWindow::Page::General);
    check(!form.Window() && saves == 3, "leaving the editor discards unsaved changes without saving");
    settings.SwitchPage(SettingsWindow::Page::Providers);
    settings.EditProvider(0);
    preview = ParseConfigJson(WideToUtf8(ReadWindowText(form.preview_)));
    preview["providers"][form.draft_.id]["label"] = "Saved JSON provider";
    preview["providers"][form.draft_.id]["headers"]["X-Preview-Saved"] = "retained";
    SetWindowTextW(form.preview_, Utf8ToWide(preview.dump(2)).c_str());
    form.DraftChanged(true);
    SendMessageW(form.save_, BM_CLICK, 0, 0);
    loaded = LoadConfig(config.config_path, false);
    const auto* json_saved = loaded.ok ? FindProvider(loaded.value, "first") : nullptr;
    check(saves == 4 && !form.Window() && json_saved && json_saved->label == "Saved JSON provider" &&
              json_saved->headers.at("X-Preview-Saved") == "retained" &&
              loaded.value.active.model == "replacement-model",
          "saving flushes pending JSON edits to disk and refreshes the active application config");
    settings.SwitchPage(SettingsWindow::Page::Providers);
    SetWindowPos(owner, nullptr, 0, 0, 1600, 1100, SWP_NOZORDER | SWP_NOACTIVATE);
    settings.Resize();
    const auto source = settings.config_.providers.front();
    const auto count = settings.config_.providers.size();
    Click(settings.provider_list_, Action(settings, 0, 3));
    check(settings.config_.providers.size() == count + 1 && saves == 5,
          "the duplicate icon saves a new independent provider");
    if (settings.config_.providers.size() == count + 1) {
        auto expected = ConfigToJson(settings.config_)["providers"][source.id];
        const auto& copy = settings.config_.providers[1];
        expected["label"] = copy.label;
        check(copy.id != source.id && copy.label != source.label &&
                  ConfigToJson(settings.config_)["providers"][copy.id] == expected &&
                  settings.config_.active.provider == "first",
              "copy preserves credentials, models and metadata without changing the active provider");
    }
    const POINT start = Action(settings, 0, 0);
    RECT destination{};
    SendMessageW(settings.provider_list_, LB_GETITEMRECT, count - 1, reinterpret_cast<LPARAM>(&destination));
    const POINT drop{start.x, destination.bottom - settings.Dpi(10)};
    SendMessageW(settings.provider_list_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(start.x, start.y));
    SendMessageW(settings.provider_list_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(drop.x, drop.y));
    SendMessageW(settings.provider_list_, WM_LBUTTONUP, 0, MAKELPARAM(drop.x, drop.y));
    check(settings.config_.providers[count - 1].id == source.id && saves == 6,
          "dragging the grip reorders providers and saves only on drop");
    loaded = LoadConfig(config.config_path, false);
    check(loaded.ok && loaded.value.providers[count - 1].id == source.id,
          "the saved provider order survives a full configuration reload");
    const auto before_cancelled_drag = ConfigToJson(settings.config_);
    const POINT cancel_start = Action(settings, 0, 0);
    SendMessageW(settings.provider_list_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cancel_start.x, cancel_start.y));
    SendMessageW(settings.provider_list_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(drop.x, drop.y));
    SendMessageW(settings.provider_list_, WM_KEYDOWN, VK_ESCAPE, 0);
    SendMessageW(settings.provider_list_, WM_LBUTTONUP, 0, MAKELPARAM(drop.x, drop.y));
    check(saves == 6 && ConfigToJson(settings.config_) == before_cancelled_drag,
          "Escape cancels an in-progress drag without saving or invoking an icon");
    const auto valid_path = settings.config_.config_path;
    settings.config_.config_path = WideToUtf8(directory.wstring());
    const auto failed_copy = settings.DuplicateProvider(0);
    check(!failed_copy.ok && saves == 6 && ConfigToJson(settings.config_) == before_cancelled_drag,
          "failed disk writes leave providers and the active application configuration unchanged");
    settings.config_.config_path = valid_path;
    {
        ModelsServer server(R"({"choices":[{"message":{"content":"hi from loopback"}}]})");
        auto& entry = settings.config_.providers.front();
        entry.protocol = "chat_completions"; entry.base_url = server.url;
        entry.test_model.clear();
        entry.models.front().capabilities.streaming = false;
        auto second_model = entry.models.front();
        second_model.id = "active-but-not-test-model";
        entry.models.push_back(second_model);
        settings.config_.active = {entry.id, second_model.id, ""};
        const auto expected_model = Utf8ToWide(entry.models.front().id);
        const auto started = GetTickCount64();
        Click(settings.provider_list_, Action(settings, 0, 1));
        check(GetTickCount64() - started < 150 && settings.provider_test_ &&
                  ReadWindowText(settings.provider_status_).find(expected_model) != std::wstring::npos,
              "the hi icon starts a real request in the background and displays progress");
        check(PumpUntil([&] { return !settings.provider_test_; }) &&
                  ReadWindowText(settings.provider_status_).find(L"hi from loopback") != std::wstring::npos &&
                  ReadWindowText(settings.provider_status_).find(L"HTTP 200") != std::wstring::npos &&
                  ReadWindowText(settings.provider_status_).find(expected_model) != std::wstring::npos &&
                  saves == 6,
              "a real hi reply and elapsed time are displayed without saving a test chat or configuration");
        Snapshot(surface, L"provider-hi-result.jpg");
    }
    {
        ModelsServer server(R"({"choices":[{"message":{"content":"stale probe reply"}}]})");
        settings.config_.providers.front().base_url = server.url;
        Click(settings.provider_list_, Action(settings, 0, 1));
        Sleep(60);
        settings.SwitchPage(SettingsWindow::Page::General);
        settings.SwitchPage(SettingsWindow::Page::Providers);
        const auto deadline = GetTickCount64() + 350;
        PumpUntil([&] { return GetTickCount64() >= deadline; });
        check(!settings.provider_test_ && ReadWindowText(settings.provider_status_).empty(),
              "a closed provider page cannot receive a stale background test result");
    }
    {
        ModelsServer server(R"({"error":{"message":"fixture rejected request"}})", 401);
        settings.config_.providers.front().base_url = server.url;
        Click(settings.provider_list_, Action(settings, 0, 1));
        check(PumpUntil([&] { return !settings.provider_test_; }) &&
                  ReadWindowText(settings.provider_status_).find(L"fixture rejected request") != std::wstring::npos &&
                  ReadWindowText(settings.provider_status_).find(L"HTTP 401") != std::wstring::npos &&
                  ReadWindowText(settings.provider_status_).find(
                      Utf8ToWide(settings.config_.providers.front().models.front().id)) != std::wstring::npos,
              "failed hi tests display actual provider error details");
    }
    {
        ModelsServer server(R"({"choices":[{"message":{"content":"cancelled by reorder"}}]})");
        auto& entry = settings.config_.providers.front();
        entry.base_url = server.url;
        const auto id = entry.id;
        settings.StartProviderTest(0);
        Sleep(60);
        check(settings.MoveProvider(id, 1).ok && !settings.provider_test_ &&
                  ReadWindowText(settings.provider_status_).empty(),
              "reordering during a hi test cancels the request and clears its obsolete progress text");
    }
    const auto active_id = settings.config_.active.provider;
    size_t active_index = 0;
    while (settings.config_.providers[active_index].id != active_id) ++active_index;
    const auto deletion = settings.DeleteProvider(active_index);
    check(deletion.ok && !FindProvider(settings.config_, active_id) &&
              FindProvider(settings.config_, settings.config_.active.provider) &&
              ValidateConfig(settings.config_, false).empty(),
          "deleting the active provider repairs the active model without deleting chat data");
    while (settings.config_.providers.size() > 1) {
        if (!settings.DeleteProvider(0).ok) { check(false, "remaining providers can be deleted"); break; }
    }
    const auto last = ConfigToJson(settings.config_);
    check(!settings.DeleteProvider(0).ok && ConfigToJson(settings.config_) == last,
          "the last provider is protected instead of writing an unusable empty configuration");
    settings.Close();
    DestroyWindow(owner);
    std::error_code error;
    spark::fs::remove_all(directory, error);
    if (failures == 0) std::cout << "Provider settings checks passed.\n";
    return failures ? 1 : 0;
}
};
}  // namespace spark

int main() {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
    const int result = spark::SettingsProviderTest::Run();
    Gdiplus::GdiplusShutdown(token);
    return result;
}
