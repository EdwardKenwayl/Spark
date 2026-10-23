#include "gui/settings_window.h"

#include "gui/localization.h"
#include "gui/theme.h"
#include "platform/window_text.h"

#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <ctime>
#include "platform/filesystem.h"
#include <sstream>

namespace spark {
namespace {

constexpr wchar_t kSettingsClass[] = L"SparkSettingsWindow";
constexpr UINT kSettingsClosedMessage = WM_APP + 5;
constexpr int kSettingsNavWidth = 238;

constexpr int kBack = 449;
constexpr int kNav = 450;
constexpr int kPageTitle = 451;
constexpr int kStatus = 453;

constexpr int kWorkspacePath = 462;
constexpr int kChooseWorkspace = 463;
constexpr int kModelStatus = 464;
constexpr int kConfigPath = 465;
constexpr int kOpenConfig = 466;
constexpr int kLanguageCombo = 469;
constexpr int kThemeCombo = 470;
constexpr int kShellCombo = 471;
constexpr int kSendKeyCombo = 472;
constexpr int kDataPath = 475;
constexpr int kCleanCache = 476;
constexpr int kDeleteAllChats = 477;

constexpr int kArchiveSearch = 480;
constexpr int kArchiveList = 481;
constexpr int kRestoreArchive = 482;
constexpr int kDeleteArchive = 483;
constexpr int kArchiveStatus = 484;
constexpr int kArchiveScopeCombo = 485;
constexpr int kArchiveListSubclass = 1;

enum class ArchiveAction {
    None,
    Restore,
    Delete,
};

constexpr int kLabelWorkspace = 491;
constexpr int kLabelModel = 492;
constexpr int kLabelConfig = 493;
constexpr int kLabelPreferences = 494;
constexpr int kLabelData = 496;

COLORREF kBackground = RGB(28, 29, 31);
COLORREF kSidebar = RGB(37, 38, 40);
COLORREF kPanel = RGB(43, 44, 47);
COLORREF kInput = RGB(48, 49, 52);
COLORREF kText = RGB(236, 237, 240);
COLORREF kMuted = RGB(150, 153, 160);
COLORREF kBorder = RGB(67, 68, 72);
COLORREF kAccent = RGB(91, 99, 112);
COLORREF kDanger = RGB(244, 112, 112);

HMENU ControlId(int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); }

HFONT MakeFont(int points, bool title = false) {
    HDC screen = GetDC(nullptr);
    const int height = -MulDiv(points, GetDeviceCaps(screen, LOGPIXELSY), 72);
    ReleaseDC(nullptr, screen);
    return CreateFontW(height, 0, 0, 0, title ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Microsoft YaHei UI");
}

void ShowControl(HWND control, bool show) {
    if (control != nullptr) ShowWindow(control, show ? SW_SHOW : SW_HIDE);
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
    }
    return lines;
}

uint64_t FileTimestamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    ULARGE_INTEGER time{};
    time.LowPart = data.ftLastWriteTime.dwLowDateTime;
    time.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return time.QuadPart / 10000000ull - 11644473600ull;
}

std::wstring ArchiveTimeLabel(uint64_t timestamp) {
    if (timestamp == 0) return L"";
    const std::time_t value = static_cast<std::time_t>(timestamp);
    std::tm local{};
    if (localtime_s(&local, &value) != 0) return L"";
    SYSTEMTIME time{};
    time.wYear = static_cast<WORD>(local.tm_year + 1900);
    time.wMonth = static_cast<WORD>(local.tm_mon + 1);
    time.wDay = static_cast<WORD>(local.tm_mday);
    time.wDayOfWeek = static_cast<WORD>(local.tm_wday);
    time.wHour = static_cast<WORD>(local.tm_hour);
    time.wMinute = static_cast<WORD>(local.tm_min);
    using LocaleNameToLcid = LCID(WINAPI*)(LPCWSTR, DWORD);
    static const auto resolve_locale = reinterpret_cast<LocaleNameToLcid>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LocaleNameToLCID"));
    LCID locale = resolve_locale ? resolve_locale(Utf8ToWide(i18n::CurrentLocale()).c_str(), 0) :
                                  LOCALE_USER_DEFAULT;
    if (locale == 0) locale = LOCALE_USER_DEFAULT;
    wchar_t date[128]{};
    wchar_t clock[128]{};
    if (GetDateFormatW(locale, DATE_SHORTDATE, &time, nullptr, date, static_cast<int>(std::size(date))) == 0 ||
        GetTimeFormatW(locale, TIME_NOSECONDS, &time, nullptr, clock, static_cast<int>(std::size(clock))) == 0) {
        return {};
    }
    return i18n::Format(L"{date}, {time}", {{L"date", date}, {L"time", clock}});
}

void DrawFolderGlyph(HDC device, const RECT& rect, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_pen = SelectObject(device, pen);
    HGDIOBJ old_brush = SelectObject(device, GetStockObject(NULL_BRUSH));
    const int top = rect.top + 2;
    const int bottom = rect.bottom - 2;
    const int left = rect.left + 1;
    const int right = rect.right - 1;
    MoveToEx(device, left, top + 3, nullptr);
    LineTo(device, left + 5, top + 3);
    LineTo(device, left + 7, top);
    LineTo(device, right - 5, top);
    LineTo(device, right - 3, top + 3);
    LineTo(device, right, top + 3);
    LineTo(device, right, bottom);
    LineTo(device, left, bottom);
    LineTo(device, left, top + 3);
    SelectObject(device, old_brush);
    SelectObject(device, old_pen);
    DeleteObject(pen);
}

void DrawTrashGlyph(HDC device, const RECT& rect, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_pen = SelectObject(device, pen);
    const int left = rect.left + 3;
    const int right = rect.right - 3;
    const int top = rect.top + 4;
    MoveToEx(device, left - 2, top, nullptr);
    LineTo(device, right + 2, top);
    MoveToEx(device, left + 2, top - 2, nullptr);
    LineTo(device, right - 2, top - 2);
    MoveToEx(device, left, top + 1, nullptr);
    LineTo(device, left + 1, rect.bottom - 2);
    LineTo(device, right - 1, rect.bottom - 2);
    LineTo(device, right, top + 1);
    MoveToEx(device, (left + right) / 2, top + 2, nullptr);
    LineTo(device, (left + right) / 2, rect.bottom - 4);
    SelectObject(device, old_pen);
    DeleteObject(pen);
}

void DrawChevronDown(HDC device, const RECT& rect, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_pen = SelectObject(device, pen);
    const int center_x = (rect.left + rect.right) / 2;
    const int center_y = (rect.top + rect.bottom) / 2;
    MoveToEx(device, center_x - 4, center_y - 2, nullptr);
    LineTo(device, center_x, center_y + 2);
    LineTo(device, center_x + 4, center_y - 2);
    SelectObject(device, old_pen);
    DeleteObject(pen);
}

std::wstring WorkspaceName(const std::string& workspace) {
    if (workspace.empty()) return i18n::Text(L"No recorded project");
    const spark::fs::path path(Utf8ToWide(workspace));
    const std::wstring name = path.filename().wstring();
    return name.empty() ? Utf8ToWide(workspace) : name;
}

std::wstring ConversationTitle(const std::string& title) {
    const std::wstring wide = Utf8ToWide(title);
    if (wide.empty() || wide == L"\u65b0\u5bf9\u8bdd" || wide == L"New conversation" || wide == L"New chat") {
        return i18n::Text(L"New conversation");
    }
    return wide;
}

std::string ResolvedDisplayPath(const AppConfig& config, const std::string& value) {
    const std::string base =
        config.config_path.empty() ? GetExecutableDirectory() : ParentPath(config.config_path);
    return AbsolutePath(ResolveConfigPath(value, config.config_path), base);
}

std::wstring SelectFolder(HWND owner) {
    wchar_t path[MAX_PATH]{};
    BROWSEINFOW browse{};
    browse.hwndOwner = owner;
    browse.lpszTitle = i18n::Text(L"Choose the default task folder");
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
    if (item == nullptr) return {};
    const bool ok = SHGetPathFromIDListW(item, path) != FALSE;
    CoTaskMemFree(item);
    return ok ? std::wstring(path) : std::wstring{};
}

}  // namespace

SettingsWindow::~SettingsWindow() { Close(); }

int SettingsWindow::Dpi(int pixels) const {
    HDC screen = GetDC(nullptr);
    const int dpi = screen != nullptr ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen != nullptr) ReleaseDC(nullptr, screen);
    return MulDiv(pixels, dpi > 0 ? dpi : 96, 96);
}

bool SettingsWindow::IsChoiceId(int id) const {
    return id == kLanguageCombo || id == kThemeCombo || id == kShellCombo ||
           id == kSendKeyCombo || id == kArchiveScopeCombo;
}

HWND SettingsWindow::CreateChoice(ChoiceControl& control, int id, std::initializer_list<const wchar_t*> options) {
    control.options.assign(options.begin(), options.end());
    control.values.clear();
    control.index = 0;
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window_, GWLP_HINSTANCE));
    control.button = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0,
                                   window_, ControlId(id), instance, nullptr);
    if (control.button != nullptr && !control.options.empty()) {
        SetWindowTextW(control.button, control.options.front().c_str());
    }
    return control.button;
}

void SettingsWindow::SetChoiceIndex(ChoiceControl& control, int index) {
    if (control.options.empty()) return;
    const int next_index = std::clamp(index, 0, static_cast<int>(control.options.size()) - 1);
    const std::wstring& next_text = control.options[static_cast<size_t>(next_index)];
    wchar_t current_text[256]{};
    if (control.button != nullptr) {
        GetWindowTextW(control.button, current_text, static_cast<int>(std::size(current_text)));
    }
    const bool label_changed = control.button != nullptr && next_text != current_text;
    control.index = next_index;
    if (label_changed) {
        SetWindowTextW(control.button, next_text.c_str());
        InvalidateRect(control.button, nullptr, FALSE);
    }
}

std::string SettingsWindow::ChoiceValue(const ChoiceControl& control) const {
    if (control.options.empty()) return {};
    const size_t index = static_cast<size_t>(std::clamp(control.index, 0, static_cast<int>(control.options.size()) - 1));
    if (control.values.size() == control.options.size()) return control.values[index];
    return WideToUtf8(control.options[index]);
}

void SettingsWindow::SelectChoiceByValue(ChoiceControl& control, const std::string& value, int fallback) {
    for (size_t index = 0; index < control.options.size(); ++index) {
        const std::string candidate = control.values.size() == control.options.size()
                                          ? control.values[index]
                                          : WideToUtf8(control.options[index]);
        if (_stricmp(candidate.c_str(), value.c_str()) == 0) {
            SetChoiceIndex(control, static_cast<int>(index));
            return;
        }
    }
    SetChoiceIndex(control, fallback);
}

void SettingsWindow::ShowChoiceMenu(ChoiceControl& control) {
    if (control.button == nullptr || control.options.empty()) return;
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    for (size_t index = 0; index < control.options.size(); ++index) {
        const UINT flags = MF_STRING | (static_cast<int>(index) == control.index ? MF_CHECKED : 0);
        AppendMenuW(menu, flags, static_cast<UINT_PTR>(index + 1), control.options[index].c_str());
    }
    RECT rect{};
    GetWindowRect(control.button, &rect);
    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, rect.left, rect.bottom, 0,
                                       window_, nullptr);
    DestroyMenu(menu);
    if (command <= 0) return;
    SetChoiceIndex(control, command - 1);
    SaveGeneral();
}

void SettingsWindow::ShowArchiveScopeMenu() {
    if (archive_scope_combo_.button == nullptr) return;
    constexpr UINT kAllProjectsCommand = 1;
    constexpr UINT kRecentCommand = 2;
    constexpr UINT kProjectCommandBase = 100;

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    AppendMenuW(menu, MF_STRING | (archive_scope_ == ArchiveScope::AllProjects ? MF_CHECKED : 0),
                kAllProjectsCommand, Text(L"All projects"));
    if (!archive_projects_.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    for (size_t index = 0; index < archive_projects_.size(); ++index) {
        const bool checked = archive_scope_ == ArchiveScope::Project && archive_scope_project_ == index;
        AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0),
                    static_cast<UINT_PTR>(kProjectCommandBase + index),
                    Utf8ToWide(archive_projects_[index].name).c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (archive_scope_ == ArchiveScope::Recent ? MF_CHECKED : 0), kRecentCommand,
                Text(L"Recent"));

    RECT rect{};
    GetWindowRect(archive_scope_combo_.button, &rect);
    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, rect.left, rect.bottom, 0,
                                       window_, nullptr);
    DestroyMenu(menu);
    if (command <= 0) return;
    if (command == static_cast<int>(kAllProjectsCommand)) {
        archive_scope_ = ArchiveScope::AllProjects;
        archive_scope_project_ = static_cast<size_t>(-1);
    } else if (command == static_cast<int>(kRecentCommand)) {
        archive_scope_ = ArchiveScope::Recent;
        archive_scope_project_ = static_cast<size_t>(-1);
    } else if (command >= static_cast<int>(kProjectCommandBase) &&
               static_cast<size_t>(command - static_cast<int>(kProjectCommandBase)) < archive_projects_.size()) {
        archive_scope_ = ArchiveScope::Project;
        archive_scope_project_ = static_cast<size_t>(command - static_cast<int>(kProjectCommandBase));
    }
    RefreshArchived();
}

void SettingsWindow::Open(HWND owner, AppConfig config, SavedCallback on_saved, ChangedCallback on_changed) {
    if (window_ != nullptr) {
        SetForegroundWindow(window_);
        return;
    }
    owner_ = owner;
    config_ = std::move(config);
    i18n::SetLocale(config_.ui.language);
    on_saved_ = std::move(on_saved);
    on_changed_ = std::move(on_changed);
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner_, GWLP_HINSTANCE));
    if (background_brush_ == nullptr) background_brush_ = CreateSolidBrush(kBackground);
    if (sidebar_brush_ == nullptr) sidebar_brush_ = CreateSolidBrush(kSidebar);
    if (panel_brush_ == nullptr) panel_brush_ = CreateSolidBrush(kPanel);
    if (input_brush_ == nullptr) input_brush_ = CreateSolidBrush(kInput);
    static bool settings_class_registered = false;
    if (!settings_class_registered) {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = WindowProc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // The settings surface paints its own client area; leaving the class brush unset
        // keeps a class-level GDI handle from outliving the instance that deleted it.
        window_class.hbrBackground = nullptr;
        window_class.lpszClassName = kSettingsClass;
        if (RegisterClassExW(&window_class) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            settings_class_registered = true;
        }
    }
    RECT owner_client{};
    GetClientRect(owner_, &owner_client);
    window_ = CreateWindowExW(0, kSettingsClass, L"",
                              WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, owner_client.right,
                              owner_client.bottom,
                              owner_, nullptr, instance, this);
    if (window_ == nullptr) return;
    CreateControls();
    ApplyTheme();
    ApplyLocalization();
    RefreshGeneral();
    RefreshArchived();
    SwitchPage(Page::General);
    RECT client{};
    GetClientRect(owner_, &client);
    SetWindowPos(window_, HWND_TOP, 0, 0, client.right, client.bottom,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);
    RaiseSurface();
    SetFocus(back_button_);
}

void SettingsWindow::Close() {
    CancelProviderDrag();
    CancelProviderTest();
    provider_editor_.Close();
    if (window_ != nullptr) DestroyWindow(window_);
}

const wchar_t* SettingsWindow::Text(std::wstring_view english_msgid) const {
    return i18n::Text(english_msgid);
}

void SettingsWindow::ApplyTheme() {
    if (window_ != nullptr) SendMessageW(window_, WM_SETREDRAW, FALSE, 0);
    const theme::Palette palette = theme::For(config_.ui.theme);
    kBackground = palette.background;
    kSidebar = palette.sidebar;
    kPanel = palette.panel;
    kInput = palette.input;
    kText = palette.text;
    kMuted = palette.muted;
    kBorder = palette.border;
    kAccent = palette.accent;
    kDanger = palette.error;
    if (background_brush_ != nullptr) DeleteObject(background_brush_);
    if (sidebar_brush_ != nullptr) DeleteObject(sidebar_brush_);
    if (panel_brush_ != nullptr) DeleteObject(panel_brush_);
    if (input_brush_ != nullptr) DeleteObject(input_brush_);
    background_brush_ = CreateSolidBrush(kBackground);
    sidebar_brush_ = CreateSolidBrush(kSidebar);
    panel_brush_ = CreateSolidBrush(kPanel);
    input_brush_ = CreateSolidBrush(kInput);
    if (window_ != nullptr) {
        SendMessageW(window_, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
        RaiseSurface();
    }
}

void SettingsWindow::ApplyLocalization() {
    i18n::SetLocale(config_.ui.language);
    if (window_ == nullptr) return;
    SendMessageW(window_, WM_SETREDRAW, FALSE, 0);
    const std::string theme = ChoiceValue(theme_combo_);
    const std::string shell = ChoiceValue(shell_combo_);
    const std::string send_key = ChoiceValue(send_key_combo_);

    SetWindowTextW(back_button_, Text(L"Back to app"));
    SendMessageW(nav_, LB_RESETCONTENT, 0, 0);
    SendMessageW(nav_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Text(L"General")));
    SendMessageW(nav_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Text(L"Provider configuration")));
    SendMessageW(nav_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Text(L"Archived chats")));
    SendMessageW(nav_, LB_SETCURSEL, static_cast<int>(page_), 0);

    SetWindowTextW(GetDlgItem(window_, kLabelWorkspace), Text(L"Default task folder"));
    SetWindowTextW(choose_workspace_, Text(L"Choose"));
    SetWindowTextW(GetDlgItem(window_, kLabelModel), Text(L"Model configuration status"));
    SetWindowTextW(GetDlgItem(window_, kLabelConfig), Text(L"Configuration file"));
    SetWindowTextW(open_config_, Text(L"Open config"));
    SetWindowTextW(GetDlgItem(window_, kLabelPreferences), Text(L"Preferences"));

    language_combo_.options = {Text(L"Automatic")};
    language_combo_.values = {"system"};
    for (const i18n::Locale& locale : i18n::SupportedLocales()) {
        language_combo_.options.emplace_back(locale.name);
        language_combo_.values.emplace_back(locale.code);
    }
    SelectChoiceByValue(language_combo_, config_.ui.language, 0);
    theme_combo_.options = {Text(L"Dark"), Text(L"Light"), Text(L"System")};
    theme_combo_.values = {"dark", "light", "system"};
    SelectChoiceByValue(theme_combo_, theme, 0);
    shell_combo_.options = {L"cmd.exe", L"powershell.exe"};
    shell_combo_.values = {"cmd.exe", "powershell.exe"};
    SelectChoiceByValue(shell_combo_, shell, 0);
    send_key_combo_.options = {L"enter", L"ctrl+enter"};
    send_key_combo_.values = {"enter", "ctrl+enter"};
    SelectChoiceByValue(send_key_combo_, send_key, 0);

    SetWindowTextW(GetDlgItem(window_, kLabelData), Text(L"Data folder"));
    SetWindowTextW(clean_cache_, Text(L"Clear cache"));
    SetWindowTextW(delete_all_button_, Text(L"Delete all"));
    SetWindowTextW(restore_button_, Text(L"Restore to project"));
    SetWindowTextW(delete_archive_button_, Text(L"Delete permanently"));
    SendMessageW(archive_search_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(Text(L"Search archived chats")));

    const wchar_t* titles[] = {Text(L"General"), Text(L"Provider configuration"), Text(L"Archived chats")};
    SetWindowTextW(page_title_, titles[static_cast<int>(page_)]);
    SetWindowTextW(add_provider_, Text(L"Add provider"));
    SetWindowTextW(provider_empty_, Text(L"No providers configured."));
    RefreshProviders();
    RefreshModelStatus();
    RefreshArchiveLabels();
    SetWindowTextW(status_, Text(L"Model context settings, API keys, and API addresses are read from the config file."));
    Layout();
    SendMessageW(window_, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
    RaiseSurface();
}

void SettingsWindow::RaiseSurface() {
    if (window_ == nullptr || !IsWindow(window_)) return;
    SetWindowPos(window_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void SettingsWindow::Resize() {
    if (window_ != nullptr) {
        RECT client{};
        GetClientRect(owner_, &client);
        SetWindowPos(window_, nullptr, 0, 0, client.right, client.bottom,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void SettingsWindow::CreateControls() {
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window_, GWLP_HINSTANCE));
    ui_font_ = MakeFont(10);
    sidebar_font_ = MakeFont(12);
    title_font_ = MakeFont(15, true);

    auto button = [&](const wchar_t* text, int id) {
        return CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0,
                             window_, ControlId(id), instance, nullptr);
    };
    auto label = [&](const wchar_t* text, int id, DWORD style) {
        return CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window_, ControlId(id),
                             instance, nullptr);
    };
    back_button_ = button(Text(L"Back to app"), kBack);
    nav_ = CreateWindowExW(0, L"LISTBOX", L"",
                           WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS | LBS_OWNERDRAWFIXED,
                           0, 0, 0, 0, window_, ControlId(kNav), instance, nullptr);
    page_title_ = label(Text(L"General"), kPageTitle, SS_LEFT | SS_ENDELLIPSIS);
    status_ = label(L"", kStatus, SS_LEFT | SS_ENDELLIPSIS);

    label(Text(L"Default task folder"), kLabelWorkspace, SS_LEFT);
    workspace_path_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      0, 0, 0, 0, window_, ControlId(kWorkspacePath), instance, nullptr);
    choose_workspace_ = button(Text(L"Choose"), kChooseWorkspace);
    label(Text(L"Model configuration status"), kLabelModel, SS_LEFT);
    model_status_ = label(L"", kModelStatus, SS_LEFT | SS_ENDELLIPSIS);
    label(Text(L"Configuration file"), kLabelConfig, SS_LEFT | SS_ENDELLIPSIS);
    config_path_ = label(L"", kConfigPath, SS_LEFT | SS_PATHELLIPSIS);
    open_config_ = button(Text(L"Open config"), kOpenConfig);
    label(Text(L"Preferences"), kLabelPreferences, SS_LEFT);
    CreateChoice(language_combo_, kLanguageCombo, {});
    CreateChoice(theme_combo_, kThemeCombo, {});
    theme_combo_.values = {"dark", "light", "system"};
    CreateChoice(shell_combo_, kShellCombo, {L"cmd.exe", L"powershell.exe"});
    shell_combo_.values = {"cmd.exe", "powershell.exe"};
    CreateChoice(send_key_combo_, kSendKeyCombo, {L"enter", L"ctrl+enter"});
    send_key_combo_.values = {"enter", "ctrl+enter"};
    label(Text(L"Data folder"), kLabelData, SS_LEFT);
    data_path_ = label(L"", kDataPath, SS_LEFT | SS_PATHELLIPSIS);
    clean_cache_ = button(Text(L"Clear cache"), kCleanCache);
    delete_all_button_ = CreateWindowW(L"BUTTON", Text(L"Delete all"),
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, window_,
                                       ControlId(kDeleteAllChats), instance, nullptr);

    archive_search_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      0, 0, 0, 0, window_, ControlId(kArchiveSearch), instance, nullptr);
    archive_list_ = CreateWindowExW(0, L"LISTBOX", L"",
                                    WS_CHILD | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS | LBS_OWNERDRAWVARIABLE |
                                        LBS_NOSEL,
                                    0, 0, 0, 0, window_, ControlId(kArchiveList), instance, nullptr);
    CreateChoice(archive_scope_combo_, kArchiveScopeCombo, {Text(L"All projects")});
    archive_scope_combo_.values = {"all"};
    restore_button_ = button(Text(L"Restore to project"), kRestoreArchive);
    delete_archive_button_ = button(Text(L"Delete permanently"), kDeleteArchive);
    archive_status_ = label(L"", kArchiveStatus, SS_LEFT);
    CreateProviderControls();

    for (HWND control : {status_, workspace_path_,
                         choose_workspace_,
                         model_status_, config_path_, open_config_, language_combo_.button,
                         theme_combo_.button, shell_combo_.button, send_key_combo_.button, data_path_,
                         clean_cache_, delete_all_button_, archive_search_, archive_list_,
                         archive_scope_combo_.button, restore_button_, delete_archive_button_, archive_status_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
    }
    for (int id : {kLabelWorkspace, kLabelModel, kLabelConfig, kLabelPreferences, kLabelData}) {
        SendMessageW(GetDlgItem(window_, id), WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
    }
    SendMessageW(back_button_, WM_SETFONT, reinterpret_cast<WPARAM>(sidebar_font_), TRUE);
    SendMessageW(nav_, WM_SETFONT, reinterpret_cast<WPARAM>(sidebar_font_), TRUE);
    SendMessageW(nav_, LB_SETITEMHEIGHT, 0, Dpi(48));
    SendMessageW(page_title_, WM_SETFONT, reinterpret_cast<WPARAM>(title_font_), TRUE);
    SetWindowSubclass(archive_list_, ArchiveListProc, kArchiveListSubclass, reinterpret_cast<DWORD_PTR>(this));
    ShowWindow(restore_button_, SW_HIDE);
    ShowWindow(delete_archive_button_, SW_HIDE);
}

void SettingsWindow::Layout() {
    if (window_ == nullptr || ui_font_ == nullptr) return;
    RECT client{};
    GetClientRect(window_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int nav_width = page_ != Page::Archived
        ? std::min(Dpi(kSettingsNavWidth), std::max(Dpi(140), width / 3)) : Dpi(kSettingsNavWidth);
    SetWindowPos(back_button_, nullptr, Dpi(18), Dpi(14), nav_width - Dpi(36), Dpi(40), SWP_NOZORDER);
    SetWindowPos(nav_, nullptr, 0, Dpi(70), nav_width, std::max(Dpi(100), height - Dpi(70)), SWP_NOZORDER);
    const int margin = Dpi(page_ != Page::Archived && width < Dpi(800) ? 24 : 42);
    const int x = nav_width + margin;
    const int content_width = page_ == Page::Providers
        ? std::max(1, std::min(Dpi(980), width - x - Dpi(24)))
        : std::max(1, std::min(Dpi(980), width - x - (width < Dpi(800) ? margin : Dpi(64))));
    HDC device = GetDC(window_);
    auto measure = [&](HWND control, std::wstring_view text, int minimum, int padding) {
        SIZE size{};
        if (device != nullptr) {
            HFONT font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
            HGDIOBJ old_font = SelectObject(device, font != nullptr ? font : ui_font_);
            GetTextExtentPoint32W(device, text.data(), static_cast<int>(text.size()), &size);
            SelectObject(device, old_font);
        }
        return std::max<int>(Dpi(minimum), size.cx + Dpi(padding));
    };
    const int choose_width = measure(choose_workspace_, ReadWindowText(choose_workspace_), 80, 24);
    const int open_width = measure(open_config_, ReadWindowText(open_config_), 116, 24);
    const int cache_width = measure(clean_cache_, ReadWindowText(clean_cache_), 86, 24);
    const int delete_all_width = measure(delete_all_button_, ReadWindowText(delete_all_button_), 118, 44);
    const int restore_width = measure(restore_button_, ReadWindowText(restore_button_), 126, 24);
    const int delete_archive_width = measure(delete_archive_button_, ReadWindowText(delete_archive_button_), 110, 24);
    const int add_width = measure(add_provider_, ReadWindowText(add_provider_), 130, 54);
    ChoiceControl* choices[] = {&language_combo_, &theme_combo_, &shell_combo_, &send_key_combo_};
    int choice_widths[4]{};
    const int gap = Dpi(8);
    for (size_t index = 0; index < std::size(choices); ++index) {
        choice_widths[index] = Dpi(100);
        for (const std::wstring& option : choices[index]->options) {
            choice_widths[index] = std::max(choice_widths[index], measure(choices[index]->button, option, 100, 42));
        }
    }
    if (device != nullptr) ReleaseDC(window_, device);

    const bool compact_providers = content_width - GetSystemMetrics(SM_CXVSCROLL) < Dpi(480);
    const int title_width = page_ == Page::Archived ? content_width - delete_all_width - Dpi(16)
        : page_ == Page::Providers && !compact_providers ? content_width - add_width - Dpi(16) : content_width;
    SetWindowPos(page_title_, nullptr, x, Dpi(28), title_width, Dpi(30), SWP_NOZORDER);
    const int label_width = std::min(Dpi(168), std::max(Dpi(96), content_width / 3));
    const int inner_x = x + Dpi(12);
    const int inner_width = std::max(1, content_width - Dpi(24));
    const bool compact_general = content_width < Dpi(650);
    const int field_x = x + label_width;
    const int field_width = std::max(1, content_width - label_width - Dpi(12));
    auto label_pos = [&](int id, int y, int width) {
        SetWindowPos(GetDlgItem(window_, id), nullptr, inner_x, y, std::max(1, width), Dpi(30), SWP_NOZORDER);
    };
    const int card_gap = Dpi(10);
    const int card_padding = Dpi(16);
    const int workspace_card_top = Dpi(82);
    const int workspace_y = workspace_card_top + card_padding;
    label_pos(kLabelWorkspace, workspace_y, label_width - Dpi(20));
    const int workspace_controls_y = workspace_y;
    const int workspace_control_x = field_x;
    const int workspace_control_width = field_width;
    const int workspace_button_width =
        std::min(choose_width, std::max(Dpi(52), workspace_control_width / 2));
    SetWindowPos(workspace_path_, nullptr, workspace_control_x, workspace_controls_y,
                 std::max(1, workspace_control_width - workspace_button_width - Dpi(10)), Dpi(30), SWP_NOZORDER);
    SetWindowPos(choose_workspace_, nullptr,
                 workspace_control_x + workspace_control_width - workspace_button_width, workspace_controls_y,
                 workspace_button_width, Dpi(30), SWP_NOZORDER);
    const int workspace_card_bottom = workspace_controls_y + Dpi(30) + card_padding;
    general_cards_[0] = {x, workspace_card_top, x + content_width, workspace_card_bottom};

    const int preferences_card_top = workspace_card_bottom + card_gap;
    const int preferences_y = preferences_card_top + card_padding;
    label_pos(kLabelPreferences, preferences_y, compact_general ? inner_width : label_width - Dpi(20));
    int choice_y = preferences_y + (compact_general ? Dpi(34) : 0);
    int preference_rows = 1;
    const int choice_area_x = compact_general ? inner_x : field_x;
    const int choice_area_width = compact_general ? inner_width : field_width;
    if (compact_general && choice_area_width >= Dpi(220))
        for (int& choice_width : choice_widths) choice_width = std::min(choice_width, (choice_area_width - gap) / 2);
    for (size_t first = 0; first < std::size(choices);) {
        size_t end = first + 1;
        int used_width = std::min(choice_widths[first], choice_area_width);
        while (end < std::size(choices) &&
               used_width + gap + std::min(choice_widths[end], choice_area_width) <= choice_area_width) {
            used_width += gap + std::min(choice_widths[end], choice_area_width);
            ++end;
        }
        int extra_width = std::max(0, choice_area_width - used_width);
        int choice_x = choice_area_x;
        for (size_t index = first; index < end; ++index) {
            const int base_width = std::min(choice_widths[index], choice_area_width);
            const int share = extra_width / static_cast<int>(end - index);
            const int choice_width = std::min(choice_area_width - (choice_x - choice_area_x),
                                              base_width + share);
            SetWindowPos(choices[index]->button, nullptr, choice_x, choice_y, std::max(1, choice_width), Dpi(30),
                         SWP_NOZORDER);
            choice_x += choice_width + gap;
            extra_width -= share;
        }
        first = end;
        if (first < std::size(choices)) {
            choice_y += Dpi(34);
            ++preference_rows;
        }
    }
    preferences_extra_height_ = (preference_rows - 1) * Dpi(34);
    const int preferences_card_bottom = choice_y + Dpi(30) + card_padding;
    general_cards_[1] = {x, preferences_card_top, x + content_width, preferences_card_bottom};

    const int data_card_top = preferences_card_bottom + card_gap;
    const int data_y = data_card_top + card_padding;
    label_pos(kLabelData, data_y, label_width - Dpi(20));
    const int data_controls_y = data_y;
    const int data_control_x = field_x;
    const int data_control_width = field_width;
    const int cache_button_width = std::min(cache_width, std::max(Dpi(52), data_control_width / 2));
    SetWindowPos(data_path_, nullptr, data_control_x, data_controls_y,
                 std::max(1, data_control_width - cache_button_width - Dpi(10)), Dpi(30), SWP_NOZORDER);
    SetWindowPos(clean_cache_, nullptr, data_control_x + data_control_width - cache_button_width, data_controls_y,
                 cache_button_width, Dpi(30), SWP_NOZORDER);
    const int data_card_bottom = data_controls_y + Dpi(30) + card_padding;
    general_cards_[2] = {x, data_card_top, x + content_width, data_card_bottom};
    SetWindowPos(status_, nullptr, x + Dpi(12), std::max(data_card_bottom + Dpi(12), height - Dpi(48)),
                 std::max(1, content_width - Dpi(24)), Dpi(26), SWP_NOZORDER);

    const int archive_scope_width = Dpi(210);
    const int archive_search_width = std::max(Dpi(180), content_width - archive_scope_width - Dpi(12));
    SetWindowPos(archive_search_, nullptr, x, Dpi(104), archive_search_width, Dpi(36), SWP_NOZORDER);
    SetWindowPos(archive_scope_combo_, nullptr, x + archive_search_width + Dpi(12), Dpi(104),
                 archive_scope_width, Dpi(36), SWP_NOZORDER);
    SetWindowPos(delete_all_button_, nullptr, x + content_width - delete_all_width, Dpi(24), delete_all_width, Dpi(34),
                 SWP_NOZORDER);
    SetWindowPos(archive_list_, nullptr, x, Dpi(152), content_width,
                 std::max(Dpi(140), height - Dpi(204)), SWP_NOZORDER);
    SetWindowPos(restore_button_, nullptr, x, height - Dpi(56), restore_width, Dpi(30), SWP_NOZORDER);
    SetWindowPos(delete_archive_button_, nullptr, x + restore_width + Dpi(10), height - Dpi(56), delete_archive_width,
                 Dpi(30), SWP_NOZORDER);
    SetWindowPos(archive_status_, nullptr, x, height - Dpi(46), content_width, Dpi(24), SWP_NOZORDER);
    SetWindowPos(add_provider_, nullptr, compact_providers ? x : x + content_width - add_width,
                 Dpi(compact_providers ? 72 : 24), std::min(add_width, content_width), Dpi(36), SWP_NOZORDER);
    const int metadata_top = Dpi(compact_providers ? 116 : 84);
    SetWindowPos(GetDlgItem(window_, kLabelModel), nullptr, x, metadata_top, content_width, Dpi(24), SWP_NOZORDER);
    SetWindowPos(model_status_, nullptr, x, metadata_top + Dpi(30), content_width,
                 Dpi(compact_providers ? 24 : 42), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(window_, kLabelConfig), nullptr, x, metadata_top + Dpi(compact_providers ? 66 : 78),
                 compact_providers ? std::max(1, content_width - open_width - Dpi(16)) : content_width,
                 Dpi(24), SWP_NOZORDER);
    SetWindowPos(config_path_, nullptr, x, metadata_top + Dpi(compact_providers ? 100 : 110),
                 compact_providers ? content_width : std::max(1, content_width - open_width - Dpi(16)),
                 Dpi(24), SWP_NOZORDER);
    SetWindowPos(open_config_, nullptr, x + std::max(0, content_width - open_width),
                 metadata_top + Dpi(compact_providers ? 60 : 104), std::min(open_width, content_width),
                 Dpi(30), SWP_NOZORDER);
    const int provider_top = metadata_top + Dpi(compact_providers ? 138 : 152);
    const int provider_available = std::max(1, height - provider_top - Dpi(16));
    const int provider_status_height = ReadWindowText(provider_status_).empty() ? 0
        : std::min(Dpi(76), std::max(Dpi(36), provider_available / 3));
    const int provider_height = std::max(1, provider_available -
        (provider_status_height > 0 ? provider_status_height + Dpi(12) : 0));
    SendMessageW(provider_list_, LB_SETITEMHEIGHT, 0, Dpi(compact_providers ? 112 : 88));
    SetWindowPos(provider_list_, nullptr, x, provider_top, content_width, provider_height, SWP_NOZORDER);
    SetWindowPos(provider_empty_, nullptr, x, provider_top + Dpi(32), content_width, Dpi(30), SWP_NOZORDER);
    SetWindowPos(provider_status_, nullptr, x, provider_top + provider_height + Dpi(12), content_width,
                 provider_status_height, SWP_NOZORDER);
    provider_editor_.Resize(x, Dpi(24), content_width, std::max(1, height - Dpi(48)));
    HoverProvider({-1, -1});
}

void SettingsWindow::SwitchPage(Page page) {
    CancelProviderDrag();
    CancelProviderTest();
    SetWindowTextW(provider_status_, L"");
    provider_editor_.Close();
    page_ = page;
    const bool general = page == Page::General, archived = page == Page::Archived;
    SendMessageW(nav_, LB_SETCURSEL, static_cast<int>(page), 0);
    const wchar_t* titles[] = {Text(L"General"), Text(L"Provider configuration"), Text(L"Archived chats")};
    SetWindowTextW(page_title_, titles[static_cast<int>(page)]);
    ShowWindow(page_title_, SW_SHOW);
    for (HWND control : {workspace_path_, choose_workspace_, language_combo_.button, theme_combo_.button,
                         shell_combo_.button, send_key_combo_.button, data_path_, clean_cache_, status_}) {
        ShowControl(control, general);
    }
    for (int id : {kLabelWorkspace, kLabelPreferences, kLabelData}) {
        ShowControl(GetDlgItem(window_, id), general);
    }
    for (HWND control : {model_status_, config_path_, open_config_,
                         GetDlgItem(window_, kLabelModel), GetDlgItem(window_, kLabelConfig)}) {
        ShowControl(control, page == Page::Providers);
    }
    for (HWND control : {archive_search_, archive_scope_combo_.button, archive_list_, archive_status_,
                          delete_all_button_}) {
        ShowControl(control, archived);
    }
    ShowControl(restore_button_, false);
    ShowControl(delete_archive_button_, false);
    ShowControl(add_provider_, page == Page::Providers);
    ShowControl(provider_list_, page == Page::Providers && !config_.providers.empty());
    ShowControl(provider_empty_, page == Page::Providers && config_.providers.empty());
    ShowControl(provider_status_, page == Page::Providers);
    if (archived) RefreshArchived();
    Layout();
    InvalidateRect(window_, nullptr, FALSE);
}

void SettingsWindow::RefreshGeneral() {
    SetWindowTextW(workspace_path_,
                   Utf8ToWide(ResolvedDisplayPath(config_, config_.workspace.path)).c_str());
    RefreshModelStatus();
    SetWindowTextW(config_path_, Utf8ToWide(config_.config_path).c_str());
    SelectChoiceByValue(language_combo_, config_.ui.language, 0);
    SelectChoiceByValue(theme_combo_, config_.ui.theme, 0);
    SelectChoiceByValue(shell_combo_, config_.ui.shell, 0);
    SelectChoiceByValue(send_key_combo_, config_.ui.send_key, 0);
    SetWindowTextW(data_path_, Utf8ToWide(ResolvedDisplayPath(config_, config_.storage.data_dir)).c_str());
    SetStatus(Text(L"Model context settings, API keys, and API addresses are read from the config file."));
}

void SettingsWindow::RefreshModelStatus() {
    const auto errors = ValidateConfig(config_, false);
    int models = 0;
    for (const ProviderConfig& provider : config_.providers) models += static_cast<int>(provider.models.size());
    const std::initializer_list<std::pair<std::wstring_view, std::wstring>> arguments =
        {{L"providers", std::to_wstring(config_.providers.size())}, {L"models", std::to_wstring(models)},
         {L"count", std::to_wstring(errors.size())}};
    const std::wstring model_text = errors.empty()
        ? i18n::Format(L"Loaded providers: {providers}. Models: {models}.", arguments)
        : i18n::Format(L"Loaded providers: {providers}. Models: {models}. Configuration issues: {count}.", arguments);
    SetWindowTextW(model_status_, model_text.c_str());
}

void SettingsWindow::RefreshArchived() {
    if (archive_list_ == nullptr) return;
    SendMessageW(archive_list_, LB_RESETCONTENT, 0, 0);
    archive_entries_.clear();
    archive_projects_.clear();
    archive_rows_.clear();
    archive_hover_row_ = -1;
    archive_hover_action_ = 0;

    const std::string projects_path = JoinPath(config_.storage.data_dir, "projects.json");
    std::string projects_error;
    const auto projects_contents = ReadFileUtf8(projects_path, &projects_error);
    if (projects_contents.has_value()) {
        try {
            const nlohmann::json document = nlohmann::json::parse(*projects_contents);
            if (document.contains("projects") && document["projects"].is_array()) {
                for (const auto& item : document["projects"]) {
                    if (!item.is_object()) continue;
                    ArchiveProject project;
                    project.name = item.value("name", std::string{});
                    project.workspace = item.value("workspace_path", std::string{});
                    if (item.contains("folders") && item["folders"].is_array()) {
                        for (const auto& folder : item["folders"]) {
                            if (folder.is_string()) project.folders.push_back(folder.get<std::string>());
                        }
                    }
                    if (project.folders.empty() && !project.workspace.empty()) project.folders.push_back(project.workspace);
                    if (project.workspace.empty()) continue;
                    if (project.name.empty()) project.name = WideToUtf8(WorkspaceName(project.workspace));
                    archive_projects_.push_back(std::move(project));
                }
            }
        } catch (const nlohmann::json::exception&) {
        }
    }
    if (archive_scope_ == ArchiveScope::Project && archive_scope_project_ >= archive_projects_.size()) {
        archive_scope_ = ArchiveScope::AllProjects;
        archive_scope_project_ = static_cast<size_t>(-1);
    }

    const std::string directory = ArchiveDirectory();
    const std::wstring pattern = Utf8ToWide(JoinPath(directory, "*.jsonl"));
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        RefreshArchiveLabels();
        return;
    }
    wchar_t search_buffer[256]{};
    GetWindowTextW(archive_search_, search_buffer, static_cast<int>(std::size(search_buffer)));
    const std::wstring search = LowerAscii(WideToUtf8(search_buffer)).empty()
                                    ? std::wstring{}
                                    : Utf8ToWide(LowerAscii(WideToUtf8(search_buffer)));
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        ArchiveEntry entry;
        entry.path = JoinPath(directory, WideToUtf8(data.cFileName));
        entry.archived_at = FileTimestamp(entry.path.empty() ? std::wstring{} : Utf8ToWide(entry.path));
        std::string error;
        const auto contents = ReadFileUtf8(entry.path, &error);
        if (contents.has_value()) {
            for (const std::string& line : SplitLines(*contents)) {
                try {
                    const nlohmann::json event = nlohmann::json::parse(line);
                    const std::string type = event.value("type", std::string{});
                    const nlohmann::json payload = event.value("payload", nlohmann::json::object());
                    if (type == "session.created") {
                        entry.title = payload.value("title", entry.title);
                        entry.workspace = payload.value("workspace", entry.workspace);
                    } else if (type == "session.title") {
                        entry.title = payload.value("title", entry.title);
                    } else if (type == "session.scope") {
                        entry.scope = payload.value("scope", entry.scope);
                    }
                } catch (const nlohmann::json::exception&) {
                }
            }
        }
        const std::wstring haystack = Utf8ToWide(LowerAscii(
            entry.title + " " + WideToUtf8(ConversationTitle(entry.title)) + " " + entry.workspace));
        if (!search.empty() && haystack.find(search) == std::wstring::npos) continue;
        if (entry.scope == "recent") {
            entry.project_index = static_cast<size_t>(-1);
        } else if (!entry.workspace.empty()) {
            const std::string normalized_workspace = NormalizePathForComparison(entry.workspace);
            for (size_t project_index = 0; project_index < archive_projects_.size(); ++project_index) {
                const ArchiveProject& project = archive_projects_[project_index];
                bool match = NormalizePathForComparison(project.workspace) == normalized_workspace;
                if (!match) {
                    for (const std::string& folder : project.folders) {
                        if (NormalizePathForComparison(folder) == normalized_workspace) {
                            match = true;
                            break;
                        }
                    }
                }
                if (match) {
                    entry.project_index = project_index;
                    break;
                }
            }
        }
        archive_entries_.push_back(entry);
    } while (FindNextFileW(find, &data) != FALSE);
    FindClose(find);
    std::sort(archive_entries_.begin(), archive_entries_.end(), [](const ArchiveEntry& left, const ArchiveEntry& right) {
        return left.archived_at > right.archived_at;
    });

    std::vector<size_t> visible_entries;
    for (size_t index = 0; index < archive_entries_.size(); ++index) {
        const ArchiveEntry& entry = archive_entries_[index];
        const bool project_scope = entry.project_index != static_cast<size_t>(-1);
        if (archive_scope_ == ArchiveScope::Project) {
            if (entry.project_index != archive_scope_project_) continue;
        } else if (archive_scope_ == ArchiveScope::Recent) {
            if (project_scope) continue;
        }
        visible_entries.push_back(index);
    }

    auto append_group = [&](const std::wstring& header, const std::vector<size_t>& indexes) {
        if (indexes.empty()) return;
        ArchiveRow row;
        row.kind = ArchiveRowKind::Header;
        row.header = header;
        row.count = indexes.size();
        archive_rows_.push_back(row);
        SendMessageW(archive_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.header.c_str()));
        for (size_t entry_index : indexes) {
            ArchiveRow entry_row;
            entry_row.kind = ArchiveRowKind::Entry;
            entry_row.entry_index = entry_index;
            archive_rows_.push_back(entry_row);
            SendMessageW(archive_list_, LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(ConversationTitle(archive_entries_[entry_index].title).c_str()));
        }
    };

    auto collect_project = [&](size_t project_index) {
        std::vector<size_t> indexes;
        for (size_t index : visible_entries) {
            if (archive_entries_[index].project_index == project_index) indexes.push_back(index);
        }
        return indexes;
    };

    if (archive_scope_ != ArchiveScope::Recent) {
        if (archive_scope_ == ArchiveScope::Project && archive_scope_project_ < archive_projects_.size()) {
            const ArchiveProject& project = archive_projects_[archive_scope_project_];
            append_group(Utf8ToWide(project.name), collect_project(archive_scope_project_));
        } else {
            for (size_t project_index = 0; project_index < archive_projects_.size(); ++project_index) {
                const ArchiveProject& project = archive_projects_[project_index];
                append_group(Utf8ToWide(project.name), collect_project(project_index));
            }
        }
    }
    std::vector<size_t> recent;
    for (size_t index : visible_entries) {
        if (archive_entries_[index].project_index == static_cast<size_t>(-1)) recent.push_back(index);
    }
    if (archive_scope_ != ArchiveScope::Project) append_group(L"", recent);
    RefreshArchiveLabels();
}

void SettingsWindow::RefreshArchiveLabels() {
    HDC device = GetDC(archive_list_);
    SIZE action_size{};
    if (device != nullptr) {
        HGDIOBJ old_font = SelectObject(device, ui_font_);
        const wchar_t* action = Text(L"Unarchive");
        GetTextExtentPoint32W(device, action, static_cast<int>(std::wcslen(action)), &action_size);
        SelectObject(device, old_font);
        ReleaseDC(archive_list_, device);
    }
    archive_restore_width_ = std::max<int>(Dpi(92), action_size.cx + Dpi(20));
    if (archive_scope_ == ArchiveScope::Project && archive_scope_project_ < archive_projects_.size()) {
        SetWindowTextW(archive_scope_combo_, Utf8ToWide(archive_projects_[archive_scope_project_].name).c_str());
    } else if (archive_scope_ == ArchiveScope::Recent) {
        SetWindowTextW(archive_scope_combo_, Text(L"Recent"));
    } else {
        SetWindowTextW(archive_scope_combo_, Text(L"All projects"));
    }

    if (archive_rows_.empty()) {
        SetWindowTextW(archive_status_, Text(L"No archived chats."));
    } else {
        const size_t count = static_cast<size_t>(std::count_if(archive_rows_.begin(), archive_rows_.end(),
            [](const ArchiveRow& row) { return row.kind == ArchiveRowKind::Entry; }));
        SetWindowTextW(archive_status_, i18n::Format(L"Archived chats: {count}.",
            {{L"count", std::to_wstring(count)}}).c_str());
    }
    InvalidateRect(archive_list_, nullptr, TRUE);
}

void SettingsWindow::SaveGeneral() {
    AppConfig updated = config_;
    updated.workspace.path = WideToUtf8(ReadWindowText(workspace_path_));
    updated.ui.language = ChoiceValue(language_combo_);
    updated.ui.theme = ChoiceValue(theme_combo_);
    updated.ui.shell = ChoiceValue(shell_combo_);
    updated.ui.send_key = ChoiceValue(send_key_combo_);
    updated.context.auto_compact = true;
    if (ConfigToJson(updated) == ConfigToJson(config_)) return;
    const auto errors = ValidateConfig(updated, false);
    if (!errors.empty()) {
        SetStatus(i18n::Format(L"Settings were not saved: {error}", {{L"error", Utf8ToWide(errors.front())}}));
        return;
    }
    const auto saved = SaveConfigAtomically(updated);
    if (!saved.ok) {
        SetStatus(i18n::Format(L"Save failed: {error}", {{L"error", Utf8ToWide(saved.error)}}));
        return;
    }
    const bool theme_changed = config_.ui.theme != updated.ui.theme;
    const bool language_changed = config_.ui.language != updated.ui.language;
    config_ = std::move(updated);
    if (on_saved_) on_saved_(config_);
    if (theme_changed) ApplyTheme();
    if (language_changed) ApplyLocalization();
    RaiseSurface();
    SetStatus(Text(L"Settings saved and applied."));
}

void SettingsWindow::OpenConfigFile() {
    ShellExecuteW(window_, L"open", L"notepad.exe", Utf8ToWide(config_.config_path).c_str(), nullptr, SW_SHOWNORMAL);
}

void SettingsWindow::ChooseWorkspace() {
    const std::wstring selected = SelectFolder(window_);
    if (!selected.empty()) {
        SetWindowTextW(workspace_path_, selected.c_str());
        SaveGeneral();
    }
}

void SettingsWindow::CleanCache() {
    const std::string cache = JoinPath(config_.storage.data_dir, "cache");
    std::error_code error;
    spark::fs::remove_all(Utf8ToWide(cache), error);
    if (error) SetStatus(i18n::Format(L"Cache cleanup failed: {error}", {{L"error", Utf8ToWide(error.message())}}));
    else SetStatus(Text(L"Cleared data/cache. Sessions and archives were kept."));
}

void SettingsWindow::DeleteAllChats() {
    if (MessageBoxW(window_, Text(L"Permanently delete all archived chats? Current chats, projects, configuration files, and workspace files will be kept."),
                    Text(L"Delete all"), MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    const std::string archive_directory = ArchiveDirectory();
    std::error_code error;
    spark::fs::remove_all(Utf8ToWide(archive_directory), error);
    if (error) {
        SetStatus(i18n::Format(L"Deleting all archived chats failed: {error}", {{L"error", Utf8ToWide(error.message())}}));
        return;
    }
    spark::fs::create_directories(Utf8ToWide(archive_directory), error);
    if (error) {
        SetStatus(i18n::Format(L"Recreating the archive folder failed: {error}", {{L"error", Utf8ToWide(error.message())}}));
        return;
    }
    RefreshArchived();
    if (on_changed_) on_changed_();
    SetStatus(Text(L"All archived chats were deleted."));
    Resize();
}

void SettingsWindow::RestoreSelectedArchive() {
    const std::string selected = SelectedArchivePath();
    if (selected.empty()) return;
    for (size_t index = 0; index < archive_entries_.size(); ++index) {
        if (NormalizePathForComparison(archive_entries_[index].path) == NormalizePathForComparison(selected)) {
            RestoreArchiveAt(index);
            return;
        }
    }
}

void SettingsWindow::RestoreArchiveAt(size_t index) {
    if (index >= archive_entries_.size()) return;
    const std::string selected = archive_entries_[index].path;
    const std::string sessions = JoinPath(config_.storage.data_dir, "sessions");
    const std::string destination = JoinPath(sessions, spark::fs::path(Utf8ToWide(selected)).filename().wstring().empty()
                                                       ? std::string("session.jsonl")
                                                       : WideToUtf8(spark::fs::path(Utf8ToWide(selected)).filename().wstring()));
    std::error_code error;
    spark::fs::create_directories(Utf8ToWide(sessions), error);
    if (!MoveFileExW(Utf8ToWide(selected).c_str(), Utf8ToWide(destination).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        SetWindowTextW(archive_status_, Text(L"Restore failed. A session with the same name may still exist at the destination."));
        return;
    }
    RefreshArchived();
    if (on_changed_) on_changed_();
    Resize();
}

void SettingsWindow::DeleteSelectedArchive() {
    const std::string selected = SelectedArchivePath();
    if (selected.empty()) return;
    for (size_t index = 0; index < archive_entries_.size(); ++index) {
        if (NormalizePathForComparison(archive_entries_[index].path) == NormalizePathForComparison(selected)) {
            DeleteArchiveAt(index);
            return;
        }
    }
}

void SettingsWindow::DeleteArchiveAt(size_t index) {
    if (index >= archive_entries_.size()) return;
    const std::string selected = archive_entries_[index].path;
    if (MessageBoxW(window_, Text(L"Permanently delete this archived chat? The session JSONL will be deleted immediately. This cannot be undone."),
                    Text(L"Delete permanently"),
                    MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    if (!DeleteFileW(Utf8ToWide(selected).c_str())) {
        SetWindowTextW(archive_status_, Text(L"Delete failed."));
        return;
    }
    RefreshArchived();
    if (on_changed_) on_changed_();
    Resize();
}

void SettingsWindow::SetStatus(const std::wstring& text) {
    if (page_ == Page::Archived) SetWindowTextW(archive_status_, text.c_str());
    else if (page_ == Page::Providers) {
        const bool was_empty = ReadWindowText(provider_status_).empty();
        SetWindowTextW(provider_status_, text.c_str());
        if (was_empty != text.empty()) Layout();
    }
    else SetWindowTextW(status_, text.c_str());
}

std::string SettingsWindow::ArchiveDirectory() const {
    return JoinPath(config_.storage.data_dir, "sessions\\archived");
}

std::string SettingsWindow::SelectedArchivePath() const {
    int row = static_cast<int>(SendMessageW(archive_list_, LB_GETCURSEL, 0, 0));
    if (row < 0 || row >= static_cast<int>(archive_rows_.size())) row = archive_hover_row_;
    if (row < 0 || row >= static_cast<int>(archive_rows_.size())) return {};
    const ArchiveRow& archive_row = archive_rows_[row];
    if (archive_row.kind != ArchiveRowKind::Entry || archive_row.entry_index >= archive_entries_.size()) return {};
    return archive_entries_[archive_row.entry_index].path;
}

int SettingsWindow::ArchiveRowAt(POINT point) const {
    if (archive_list_ == nullptr) return -1;
    const LRESULT hit = SendMessageW(archive_list_, LB_ITEMFROMPOINT, 0, MAKELPARAM(point.x, point.y));
    if (HIWORD(hit) != 0) return -1;
    const int row = LOWORD(hit);
    return row < static_cast<int>(archive_rows_.size()) ? row : -1;
}

int SettingsWindow::ArchiveActionAt(int row, int x) const {
    if (row < 0 || row >= static_cast<int>(archive_rows_.size())) return 0;
    if (archive_rows_[row].kind != ArchiveRowKind::Entry) return 0;
    RECT client{};
    GetClientRect(archive_list_, &client);
    const int restore_right = client.right - Dpi(12);
    const int restore_left = restore_right - std::max(Dpi(92), archive_restore_width_);
    const int delete_left = restore_left - Dpi(36);
    if (x >= restore_left && x <= restore_right) return 1;
    if (x >= delete_left && x < restore_left) return 2;
    return 0;
}

void SettingsWindow::HandleArchiveListMove(POINT point) {
    const int row = ArchiveRowAt(point);
    const int action = ArchiveActionAt(row, point.x);
    if (row == archive_hover_row_ && action == archive_hover_action_) return;
    archive_hover_row_ = row;
    archive_hover_action_ = action;
    InvalidateRect(archive_list_, nullptr, TRUE);
}

void SettingsWindow::HandleArchiveListClick(POINT point) {
    const int row = ArchiveRowAt(point);
    if (row < 0 || row >= static_cast<int>(archive_rows_.size())) return;
    const ArchiveRow& archive_row = archive_rows_[row];
    if (archive_row.kind != ArchiveRowKind::Entry || archive_row.entry_index >= archive_entries_.size()) return;
    const int action = ArchiveActionAt(row, point.x);
    if (action == 1) RestoreArchiveAt(archive_row.entry_index);
    else if (action == 2) DeleteArchiveAt(archive_row.entry_index);
}

LRESULT CALLBACK SettingsWindow::ArchiveListProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                                 UINT_PTR subclass_id, DWORD_PTR ref_data) {
    SettingsWindow* self = reinterpret_cast<SettingsWindow*>(ref_data);
    if (self == nullptr) return DefSubclassProc(window, message, wparam, lparam);
    switch (message) {
        case WM_MOUSEMOVE: {
            POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            self->HandleArchiveListMove(point);
            TRACKMOUSEEVENT tracking{};
            tracking.cbSize = sizeof(tracking);
            tracking.dwFlags = TME_LEAVE;
            tracking.hwndTrack = window;
            TrackMouseEvent(&tracking);
            break;
        }
        case WM_MOUSELEAVE:
            if (self->archive_hover_row_ != -1 || self->archive_hover_action_ != 0) {
                self->archive_hover_row_ = -1;
                self->archive_hover_action_ = 0;
                InvalidateRect(window, nullptr, TRUE);
            }
            break;
        case WM_LBUTTONUP: {
            POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            self->HandleArchiveListClick(point);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT) {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(window, &point);
                const int action = self->ArchiveActionAt(self->ArchiveRowAt(point), point.x);
                SetCursor(LoadCursorW(nullptr, action == 0 ? IDC_ARROW : IDC_HAND));
                return TRUE;
            }
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(window, ArchiveListProc, subclass_id);
            break;
        default:
            break;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK SettingsWindow::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    SettingsWindow* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<SettingsWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = window;
    }
    return self != nullptr ? self->HandleMessage(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT SettingsWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_SIZE:
            Layout();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_COMMAND:
            if (LOWORD(wparam) == kNav && HIWORD(wparam) == LBN_SELCHANGE) {
                const LRESULT selected = SendMessageW(nav_, LB_GETCURSEL, 0, 0);
                if (selected >= 0 && selected <= static_cast<int>(Page::Archived)) SwitchPage(static_cast<Page>(selected));
                return 0;
            }
            if (LOWORD(wparam) == kAddProvider) { EditProvider(config_.providers.size()); return 0; }
            if (LOWORD(wparam) == kBack && HIWORD(wparam) == BN_CLICKED) {
                Close();
                return 0;
            }
            if (LOWORD(wparam) == kChooseWorkspace) { ChooseWorkspace(); return 0; }
            if (LOWORD(wparam) == kOpenConfig) { OpenConfigFile(); return 0; }
            if (LOWORD(wparam) == kCleanCache) { CleanCache(); return 0; }
            if (LOWORD(wparam) == kDeleteAllChats) { DeleteAllChats(); return 0; }
            if (LOWORD(wparam) == kRestoreArchive) { RestoreSelectedArchive(); return 0; }
            if (LOWORD(wparam) == kDeleteArchive) { DeleteSelectedArchive(); return 0; }
            if (LOWORD(wparam) == kArchiveScopeCombo && HIWORD(wparam) == BN_CLICKED) {
                ShowArchiveScopeMenu();
                return 0;
            }
            if (HIWORD(wparam) == BN_CLICKED && IsChoiceId(LOWORD(wparam))) {
                const int choice_id = LOWORD(wparam);
                if (choice_id == kLanguageCombo) ShowChoiceMenu(language_combo_);
                else if (choice_id == kThemeCombo) ShowChoiceMenu(theme_combo_);
                else if (choice_id == kShellCombo) ShowChoiceMenu(shell_combo_);
                else if (choice_id == kSendKeyCombo) ShowChoiceMenu(send_key_combo_);
                return 0;
            }
            if (LOWORD(wparam) == kWorkspacePath && HIWORD(wparam) == EN_KILLFOCUS) {
                SaveGeneral();
                return 0;
            }
            if (LOWORD(wparam) == kArchiveSearch && HIWORD(wparam) == EN_CHANGE) { RefreshArchived(); return 0; }
            if (LOWORD(wparam) == kArchiveList && HIWORD(wparam) == LBN_DBLCLK) { RestoreSelectedArchive(); return 0; }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC device = reinterpret_cast<HDC>(wparam);
            const HWND control = reinterpret_cast<HWND>(lparam);
            SetBkMode(device, TRANSPARENT);
            SetTextColor(device, kText);
            const bool page_header = control == page_title_ || control == status_ ||
                                     control == provider_empty_ || control == provider_status_ ||
                                     page_ == Page::Providers;
            SetBkColor(device, page_header ? kBackground : kPanel);
            return reinterpret_cast<LRESULT>(page_header ? background_brush_ : panel_brush_);
        }
        case WM_CTLCOLOREDIT: {
            HDC device = reinterpret_cast<HDC>(wparam);
            SetTextColor(device, kText);
            SetBkColor(device, kInput);
            return reinterpret_cast<LRESULT>(input_brush_);
        }
        case WM_PRINTCLIENT:
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC device = message == WM_PAINT ? BeginPaint(window_, &paint) : reinterpret_cast<HDC>(wparam);
            RECT client{};
            GetClientRect(window_, &client);
            FillRect(device, &client, background_brush_);
            const int nav_width = page_ != Page::Archived
                ? std::min(Dpi(kSettingsNavWidth), std::max(Dpi(140), static_cast<int>(client.right) / 3))
                : Dpi(kSettingsNavWidth);
            RECT sidebar{0, 0, nav_width, client.bottom};
            FillRect(device, &sidebar, sidebar_brush_);
            HPEN divider = CreatePen(PS_SOLID, 1, kBorder);
            HGDIOBJ old_divider = SelectObject(device, divider);
            MoveToEx(device, nav_width, 0, nullptr);
            LineTo(device, nav_width, client.bottom);
            SelectObject(device, old_divider);
            DeleteObject(divider);
            const int x = nav_width + Dpi(42);
            const int card_right = x + std::min(Dpi(980), std::max(Dpi(420), static_cast<int>(client.right) - x - Dpi(64)));
            auto draw_card = [&](int top, int bottom) {
                RECT card{x, top, card_right, bottom};
                HBRUSH fill = CreateSolidBrush(kPanel);
                HPEN border = CreatePen(PS_SOLID, 1, kBorder);
                HGDIOBJ old_brush = SelectObject(device, fill);
                HGDIOBJ old_pen = SelectObject(device, border);
                RoundRect(device, card.left, card.top, card.right, card.bottom, Dpi(8), Dpi(8));
                SelectObject(device, old_pen);
                SelectObject(device, old_brush);
                DeleteObject(border);
                DeleteObject(fill);
            };
            if (page_ == Page::General) {
                for (const RECT& card : general_cards_) {
                    if (card.bottom <= card.top) continue;
                    HBRUSH fill = CreateSolidBrush(kPanel);
                    HPEN border = CreatePen(PS_SOLID, 1, kBorder);
                    HGDIOBJ old_brush = SelectObject(device, fill);
                    HGDIOBJ old_pen = SelectObject(device, border);
                    RoundRect(device, card.left, card.top, card.right, card.bottom, Dpi(8), Dpi(8));
                    SelectObject(device, old_pen);
                    SelectObject(device, old_brush);
                    DeleteObject(border);
                    DeleteObject(fill);
                }
            } else if (page_ == Page::Archived) {
                draw_card(Dpi(96), client.bottom - Dpi(84));
            }
            if (message == WM_PAINT) EndPaint(window_, &paint);
            return 0;
        }
        case WM_CTLCOLORLISTBOX: {
            HDC device = reinterpret_cast<HDC>(wparam);
            HWND control = reinterpret_cast<HWND>(lparam);
            SetTextColor(device, kText);
            const COLORREF color = control == nav_ ? kSidebar : control == provider_list_ ? kBackground : kPanel;
            SetBkColor(device, color);
            return reinterpret_cast<LRESULT>(control == nav_ ? sidebar_brush_
                : control == provider_list_ ? background_brush_ : panel_brush_);
        }
        case WM_MEASUREITEM: {
            auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if (item == nullptr) break;
            if (item->CtlType == ODT_MENU) {
                item->itemHeight = Dpi(30);
                return TRUE;
            }
            if (item->CtlType == ODT_LISTBOX && item->CtlID == kNav) {
                item->itemHeight = Dpi(48);
                return TRUE;
            }
            if (item->CtlType == ODT_LISTBOX && item->CtlID == kProviderList) {
                item->itemHeight = Dpi(88);
                return TRUE;
            }
            if (item->CtlType == ODT_LISTBOX && item->CtlID == kArchiveList) {
                const size_t row = static_cast<size_t>(item->itemID);
                item->itemHeight =
                    row < archive_rows_.size() && archive_rows_[row].kind == ArchiveRowKind::Header ? Dpi(42) : Dpi(64);
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (item == nullptr) break;
            if ((item->CtlType == ODT_LISTBOX && item->CtlID == kProviderList) ||
                (item->CtlType == ODT_BUTTON && item->CtlID == kAddProvider)) {
                DrawProviderControl(*item);
                return TRUE;
            }
            if (item->CtlType == ODT_MENU) {
                const bool selected = (item->itemState & ODS_SELECTED) != 0;
                const bool checked = (item->itemState & ODS_CHECKED) != 0;
                FillRect(item->hDC, &item->rcItem, selected ? input_brush_ : panel_brush_);
                wchar_t text[128]{};
                MENUITEMINFOW info{};
                info.cbSize = sizeof(info);
                info.fMask = MIIM_STRING;
                info.dwTypeData = text;
                info.cch = static_cast<UINT>(std::size(text));
                GetMenuItemInfoW(reinterpret_cast<HMENU>(item->hwndItem), item->itemID, FALSE, &info);
                SetBkMode(item->hDC, TRANSPARENT);
                SetTextColor(item->hDC, selected ? kText : kMuted);
                HFONT old_font = static_cast<HFONT>(SelectObject(item->hDC, ui_font_));
                RECT text_rect = item->rcItem;
                text_rect.left += Dpi(28);
                DrawTextW(item->hDC, text, -1, &text_rect,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                SelectObject(item->hDC, old_font);
                if (checked) {
                    HPEN check = CreatePen(PS_SOLID, 2, kAccent);
                    HGDIOBJ old_check_pen = SelectObject(item->hDC, check);
                    const int mark_x = item->rcItem.left + Dpi(10);
                    const int mark_y = (item->rcItem.top + item->rcItem.bottom) / 2;
                    MoveToEx(item->hDC, mark_x, mark_y, nullptr);
                    LineTo(item->hDC, mark_x + Dpi(4), mark_y + Dpi(4));
                    LineTo(item->hDC, mark_x + Dpi(11), mark_y - Dpi(5));
                    SelectObject(item->hDC, old_check_pen);
                    DeleteObject(check);
                }
                return TRUE;
            }
            if (item->CtlType == ODT_LISTBOX && item->CtlID == kArchiveList) {
                const int row_index = static_cast<int>(item->itemID);
                if (row_index < 0 || row_index >= static_cast<int>(archive_rows_.size())) return TRUE;
                const ArchiveRow& row = archive_rows_[row_index];
                FillRect(item->hDC, &item->rcItem, background_brush_);
                SetBkMode(item->hDC, TRANSPARENT);
                HFONT old_font = static_cast<HFONT>(SelectObject(item->hDC, ui_font_));
                if (row.kind == ArchiveRowKind::Header) {
                    RECT icon{ item->rcItem.left + Dpi(10), item->rcItem.top + Dpi(12), item->rcItem.left + Dpi(28),
                               item->rcItem.top + Dpi(30) };
                    DrawFolderGlyph(item->hDC, icon, kMuted);
                    RECT title = item->rcItem;
                    title.left += Dpi(36);
                    const std::wstring count = i18n::Format(L"Chats: {count}", {{L"count", std::to_wstring(row.count)}});
                    SIZE count_size{};
                    GetTextExtentPoint32W(item->hDC, count.c_str(), static_cast<int>(count.size()), &count_size);
                    title.right -= count_size.cx + Dpi(24);
                    SetTextColor(item->hDC, kText);
                    DrawTextW(item->hDC, row.header.empty() ? Text(L"No project") : row.header.c_str(), -1, &title,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                    RECT count_rect = item->rcItem;
                    count_rect.right -= Dpi(8);
                    SetTextColor(item->hDC, kMuted);
                    DrawTextW(item->hDC, count.c_str(), -1, &count_rect,
                              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                } else if (row.entry_index < archive_entries_.size()) {
                    const ArchiveEntry& entry = archive_entries_[row.entry_index];
                    if (archive_hover_row_ == row_index) FillRect(item->hDC, &item->rcItem, input_brush_);
                    const int restore_right = item->rcItem.right - Dpi(12);
                    const int restore_left = restore_right - std::max(Dpi(92), archive_restore_width_);
                    const int delete_left = restore_left - Dpi(36);

                    RECT title = item->rcItem;
                    title.left += Dpi(16);
                    title.right = delete_left - Dpi(16);
                    title.top += Dpi(8);
                    title.bottom = title.top + Dpi(24);
                    SetTextColor(item->hDC, kText);
                    DrawTextW(item->hDC, ConversationTitle(entry.title).c_str(), -1, &title,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

                    RECT time_rect = title;
                    time_rect.top += Dpi(25);
                    time_rect.bottom += Dpi(25);
                    SetTextColor(item->hDC, kMuted);
                    DrawTextW(item->hDC, ArchiveTimeLabel(entry.archived_at).c_str(), -1, &time_rect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

                    RECT delete_rect{ delete_left + Dpi(8), item->rcItem.top + Dpi(20),
                                      delete_left + Dpi(28), item->rcItem.top + Dpi(40) };
                    DrawTrashGlyph(item->hDC, delete_rect,
                                   archive_hover_action_ == 2 && archive_hover_row_ == row_index ? kDanger : kMuted);

                    RECT restore_rect{ restore_left, item->rcItem.top + Dpi(11), restore_right,
                                       item->rcItem.bottom - Dpi(11) };
                    HBRUSH restore_fill = CreateSolidBrush(archive_hover_action_ == 1 && archive_hover_row_ == row_index
                                                               ? RGB(58, 59, 64)
                                                               : kInput);
                    HPEN restore_border = CreatePen(PS_SOLID, 1, kBorder);
                    HGDIOBJ old_brush = SelectObject(item->hDC, restore_fill);
                    HGDIOBJ old_pen = SelectObject(item->hDC, restore_border);
                    RoundRect(item->hDC, restore_rect.left, restore_rect.top, restore_rect.right,
                              restore_rect.bottom, Dpi(10), Dpi(10));
                    SelectObject(item->hDC, old_pen);
                    SelectObject(item->hDC, old_brush);
                    DeleteObject(restore_border);
                    DeleteObject(restore_fill);
                    SetTextColor(item->hDC, kText);
                    DrawTextW(item->hDC, Text(L"Unarchive"), -1, &restore_rect,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                    HPEN separator = CreatePen(PS_SOLID, 1, kBorder);
                    HGDIOBJ old_separator = SelectObject(item->hDC, separator);
                    MoveToEx(item->hDC, item->rcItem.left + Dpi(12), item->rcItem.bottom - 1, nullptr);
                    LineTo(item->hDC, item->rcItem.right - Dpi(12), item->rcItem.bottom - 1);
                    SelectObject(item->hDC, old_separator);
                    DeleteObject(separator);
                }
                SelectObject(item->hDC, old_font);
                return TRUE;
            }
            if (item->CtlType == ODT_BUTTON && IsChoiceId(item->CtlID)) {
                const bool pressed = (item->itemState & ODS_SELECTED) != 0;
                FillRect(item->hDC, &item->rcItem, input_brush_);
                wchar_t text[256]{};
                GetWindowTextW(item->hwndItem, text, static_cast<int>(std::size(text)));
                SetBkMode(item->hDC, TRANSPARENT);
                SetTextColor(item->hDC, kText);
                HFONT old_font = static_cast<HFONT>(SelectObject(item->hDC, ui_font_));
                RECT text_rect = item->rcItem;
                text_rect.left += Dpi(item->CtlID == kArchiveScopeCombo ? 34 : 12);
                text_rect.right -= Dpi(30);
                DrawTextW(item->hDC, text, -1, &text_rect,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                SelectObject(item->hDC, old_font);
                if (item->CtlID == kArchiveScopeCombo) {
                    RECT folder{ item->rcItem.left + Dpi(11), item->rcItem.top + Dpi(10),
                                 item->rcItem.left + Dpi(29), item->rcItem.top + Dpi(28) };
                    DrawFolderGlyph(item->hDC, folder, kMuted);
                }
                HPEN border = CreatePen(PS_SOLID, 1, pressed ? kAccent : kBorder);
                HGDIOBJ old_pen = SelectObject(item->hDC, border);
                HGDIOBJ old_brush = SelectObject(item->hDC, GetStockObject(NULL_BRUSH));
                Rectangle(item->hDC, item->rcItem.left, item->rcItem.top, item->rcItem.right - 1,
                          item->rcItem.bottom - 1);
                SelectObject(item->hDC, old_brush);
                const int center_y = (item->rcItem.top + item->rcItem.bottom) / 2;
                const int center_x = item->rcItem.right - Dpi(16);
                MoveToEx(item->hDC, center_x - Dpi(4), center_y - Dpi(2), nullptr);
                LineTo(item->hDC, center_x, center_y + Dpi(2));
                LineTo(item->hDC, center_x + Dpi(4), center_y - Dpi(2));
                SelectObject(item->hDC, old_pen);
                DeleteObject(border);
                return TRUE;
            }
            if (item->CtlType == ODT_BUTTON && item->CtlID == kDeleteAllChats) {
                const bool pressed = (item->itemState & ODS_SELECTED) != 0;
                FillRect(item->hDC, &item->rcItem, background_brush_);
                HBRUSH fill = CreateSolidBrush(pressed ? RGB(68, 31, 34) : RGB(49, 31, 34));
                HPEN border = CreatePen(PS_SOLID, 1, kDanger);
                HGDIOBJ old_brush = SelectObject(item->hDC, fill);
                HGDIOBJ old_pen = SelectObject(item->hDC, border);
                RoundRect(item->hDC, item->rcItem.left + 1, item->rcItem.top + 1, item->rcItem.right - 1,
                          item->rcItem.bottom - 1, Dpi(8), Dpi(8));
                SelectObject(item->hDC, old_pen);
                SelectObject(item->hDC, old_brush);
                DeleteObject(border);
                DeleteObject(fill);
                RECT trash{ item->rcItem.left + Dpi(10), item->rcItem.top + Dpi(8), item->rcItem.left + Dpi(28),
                            item->rcItem.top + Dpi(26) };
                DrawTrashGlyph(item->hDC, trash, kDanger);
                SetBkMode(item->hDC, TRANSPARENT);
                SetTextColor(item->hDC, kDanger);
                HFONT old_font = static_cast<HFONT>(SelectObject(item->hDC, ui_font_));
                RECT text_rect = item->rcItem;
                text_rect.left += Dpi(35);
                text_rect.right -= Dpi(8);
                DrawTextW(item->hDC, Text(L"Delete all"), -1, &text_rect,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                SelectObject(item->hDC, old_font);
                return TRUE;
            }
            if (item->CtlType == ODT_BUTTON && item->CtlID == kBack) {
                const bool pressed = (item->itemState & ODS_SELECTED) != 0;
                FillRect(item->hDC, &item->rcItem, sidebar_brush_);
                SetBkMode(item->hDC, TRANSPARENT);
                SetTextColor(item->hDC, pressed ? kText : kMuted);
                HFONT old_font = static_cast<HFONT>(SelectObject(item->hDC, sidebar_font_));
                RECT text_rect = item->rcItem;
                const int arrow_x = text_rect.left + Dpi(8);
                const int arrow_y = (text_rect.top + text_rect.bottom) / 2;
                HPEN arrow = CreatePen(PS_SOLID, 1, pressed ? kText : kMuted);
                HGDIOBJ old_pen = SelectObject(item->hDC, arrow);
                MoveToEx(item->hDC, arrow_x + Dpi(14), arrow_y, nullptr);
                LineTo(item->hDC, arrow_x, arrow_y);
                LineTo(item->hDC, arrow_x + Dpi(5), arrow_y - Dpi(5));
                MoveToEx(item->hDC, arrow_x, arrow_y, nullptr);
                LineTo(item->hDC, arrow_x + Dpi(5), arrow_y + Dpi(5));
                SelectObject(item->hDC, old_pen);
                DeleteObject(arrow);
                text_rect.left += Dpi(32);
                DrawTextW(item->hDC, Text(L"Back to app"), -1, &text_rect,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                SelectObject(item->hDC, old_font);
                return TRUE;
            }
            if (item != nullptr && item->CtlType == ODT_LISTBOX && item->CtlID == kNav) {
                const bool selected = (item->itemState & ODS_SELECTED) != 0;
                FillRect(item->hDC, &item->rcItem, sidebar_brush_);
                if (selected) {
                    RECT highlight = item->rcItem;
                    highlight.left += Dpi(8);
                    highlight.right -= Dpi(8);
                    FillRect(item->hDC, &highlight, input_brush_);
                }
                wchar_t text[64]{};
                SendMessageW(item->hwndItem, LB_GETTEXT, item->itemID, reinterpret_cast<LPARAM>(text));
                SetBkMode(item->hDC, TRANSPARENT);
                SetTextColor(item->hDC, selected ? kText : kMuted);
                HFONT old_font = static_cast<HFONT>(SelectObject(item->hDC, sidebar_font_));
                RECT text_rect = item->rcItem;
                text_rect.left += Dpi(24);
                DrawTextW(item->hDC, text, -1, &text_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                SelectObject(item->hDC, old_font);
                return TRUE;
            }
            if (item->CtlType != ODT_BUTTON) break;
            const bool pressed = (item->itemState & ODS_SELECTED) != 0;
            const bool disabled = (item->itemState & ODS_DISABLED) != 0;
            FillRect(item->hDC, &item->rcItem, background_brush_);
            const bool danger = item->CtlID == kDeleteAllChats || item->CtlID == kDeleteArchive;
            const COLORREF fill_color = danger ? (pressed ? RGB(68, 31, 34) : RGB(49, 31, 34))
                                               : (pressed ? RGB(61, 62, 66) : kPanel);
            const COLORREF text_color = danger ? (disabled ? kMuted : kDanger) : (disabled ? kMuted : kText);
            HBRUSH fill = CreateSolidBrush(fill_color);
            HPEN border = CreatePen(PS_SOLID, 1, disabled ? kMuted : (danger ? kDanger : kBorder));
            HGDIOBJ old_brush = SelectObject(item->hDC, fill);
            HGDIOBJ old_pen = SelectObject(item->hDC, border);
            RoundRect(item->hDC, item->rcItem.left, item->rcItem.top, item->rcItem.right, item->rcItem.bottom, 8, 8);
            SelectObject(item->hDC, old_brush);
            SelectObject(item->hDC, old_pen);
            DeleteObject(fill);
            DeleteObject(border);
            SetBkMode(item->hDC, TRANSPARENT);
            SetTextColor(item->hDC, text_color);
            HFONT old_font = static_cast<HFONT>(SelectObject(item->hDC, ui_font_));
            wchar_t text[256]{};
            GetWindowTextW(item->hwndItem, text, static_cast<int>(std::size(text)));
            RECT text_rect = item->rcItem;
            text_rect.left += Dpi(6);
            text_rect.right -= Dpi(6);
            if (pressed) ++text_rect.top;
            DrawTextW(item->hDC, text, -1, &text_rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(item->hDC, old_font);
            return TRUE;
        }
        case WM_CLOSE:
            Close();
            return 0;
        case WM_DESTROY:
            window_ = nullptr;
            if (ui_font_ != nullptr) DeleteObject(ui_font_);
            if (sidebar_font_ != nullptr) DeleteObject(sidebar_font_);
            if (title_font_ != nullptr) DeleteObject(title_font_);
            if (background_brush_ != nullptr) DeleteObject(background_brush_);
            if (sidebar_brush_ != nullptr) DeleteObject(sidebar_brush_);
            if (panel_brush_ != nullptr) DeleteObject(panel_brush_);
            if (input_brush_ != nullptr) DeleteObject(input_brush_);
            ui_font_ = nullptr;
            sidebar_font_ = nullptr;
            title_font_ = nullptr;
            background_brush_ = nullptr;
            sidebar_brush_ = nullptr;
            panel_brush_ = nullptr;
            input_brush_ = nullptr;
            if (owner_ != nullptr && IsWindow(owner_)) {
                PostMessageW(owner_, kSettingsClosedMessage, 0, 0);
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

}  // namespace spark
