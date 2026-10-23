#include "gui/settings_window.h"
#include "gui/localization.h"
#include "gui/theme.h"
#include "core/session.h"
#include "platform/window_text.h"

#include <commctrl.h>
#include <objidl.h>
#include <algorithm>
#ifdef SPARK_WINDOWS_XP
using std::min;
using std::max;
#endif
using byte = BYTE;
#include <gdiplus.h>
#include "platform/filesystem.h"
#include <iostream>

namespace spark {
struct SettingsArchiveTest {
static void CALLBACK AnswerDialog(HWND, UINT, UINT_PTR answer, DWORD) {
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM choice) -> BOOL {
        wchar_t name[32]{}; GetClassNameW(window, name, 32);
        if (wcscmp(name, L"#32770") == 0) PostMessageW(window, WM_COMMAND, choice, 0);
        return TRUE;
    }, static_cast<LPARAM>(answer));
}

static bool PaintMatches(SettingsWindow& settings, const wchar_t* snapshot) {
    RECT bounds{}; GetClientRect(settings.window_, &bounds);
    HDC device = GetDC(settings.window_), buffer = CreateCompatibleDC(device);
    HBITMAP bitmap = CreateCompatibleBitmap(device, bounds.right, bounds.bottom);
    HGDIOBJ previous = SelectObject(buffer, bitmap);
    SendMessageW(settings.window_, WM_PRINT, reinterpret_cast<WPARAM>(buffer),
                 PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    const auto palette = theme::For(settings.config_.ui.theme);
    RECT list{}; GetWindowRect(settings.archive_list_, &list);
    MapWindowPoints(nullptr, settings.window_, reinterpret_cast<POINT*>(&list), 2);
    const bool unframed = GetPixel(buffer, list.left + settings.Dpi(1), settings.Dpi(97)) == palette.background;
    // Exercise owner drawing directly as well as the composed settings surface.
    RECT row{}; SendMessageW(settings.archive_list_, LB_GETITEMRECT, 1, reinterpret_cast<LPARAM>(&row));
    OffsetRect(&row, list.left, list.top);
    settings.archive_hover_row_ = 1;
    settings.archive_hover_action_ = 1;
    DRAWITEMSTRUCT item{ODT_LISTBOX, 481, 1, ODA_DRAWENTIRE, 0, settings.archive_list_, buffer, row, 0};
    SendMessageW(settings.window_, WM_DRAWITEM, 481, reinterpret_cast<LPARAM>(&item));
    const int x = row.right - settings.Dpi(12) - settings.archive_restore_width_ + settings.Dpi(4);
    const bool hover = GetPixel(buffer, x, (row.top + row.bottom) / 2) == palette.selection;
    const auto* folder = _wgetenv(L"SPARK_ARCHIVE_SNAPSHOT_DIR");
    if (folder) {
        UINT count = 0, bytes = 0; Gdiplus::GetImageEncodersSize(&count, &bytes);
        std::vector<BYTE> storage(bytes);
        auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
        if (Gdiplus::GetImageEncoders(count, bytes, encoders) == Gdiplus::Ok) {
            Gdiplus::Bitmap image(bitmap, nullptr);
            const int width = std::min(800L, bounds.right);
            const int height = bounds.bottom * width / bounds.right;
            Gdiplus::Bitmap resized(width, height);
            Gdiplus::Graphics graphics(&resized); graphics.DrawImage(&image, 0, 0, width, height);
            for (UINT index = 0; index < count; ++index)
                if (wcscmp(encoders[index].MimeType, L"image/jpeg") == 0)
                    resized.Save((fs::path(folder) / snapshot).c_str(), &encoders[index].Clsid);
        }
    }
    settings.archive_hover_row_ = -1;
    settings.archive_hover_action_ = 0;
    SelectObject(buffer, previous); DeleteObject(bitmap); DeleteDC(buffer); ReleaseDC(settings.window_, device);
    return unframed && hover;
}

static int Run() {
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
    };
    const auto directory = fs::temp_directory_path() /
        ("spark-archives-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(directory / "sessions" / "archived");
    AppConfig config;
    config.ui.language = "en";
    config.ui.theme = "light";
    config.config_path = WideToUtf8((directory / "config.json").wstring());
    config.storage.data_dir = WideToUtf8(directory.wstring());
    const auto write = [&](const char* name, const std::string& value) {
        return WriteFileUtf8Atomic(WideToUtf8((directory / name).wstring()), value);
    };
    write("projects.json", R"({"projects":[{"name":"Test","workspace_path":"C:/fixture/Test"},
        {"name":"Other","workspace_path":"C:/fixture/Other"}]})");
    const auto archive = [&](const char* name, const char* title, const char* scope,
                             const char* workspace = "C:/fixture/Test") {
        return write(name, nlohmann::json({{"type", "session.created"}, {"payload", {
            {"session_id", name}, {"title", title}, {"workspace", workspace}}}}).dump() + "\n" +
            nlohmann::json({{"type", "session.scope"}, {"payload", {{"scope", scope}}}}).dump() + "\n");
    };
    archive("sessions/archived/project.jsonl", "Project chat", "project");
    archive("sessions/archived/recent.jsonl", "Recent chat", "recent");
    archive("sessions/archived/other.jsonl", "Other chat", "project", "C:/fixture/Other");
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    HWND owner = CreateWindowW(L"STATIC", L"archive-fixture", WS_OVERLAPPEDWINDOW,
        0, 0, 1100, 750, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    SettingsWindow settings;
    int changes = 0;
    settings.Open(owner, config, {}, [&] { ++changes; });
    settings.SwitchPage(SettingsWindow::Page::Archived);
    const auto rect = [&](HWND control) {
        RECT value{}; GetWindowRect(control, &value);
        MapWindowPoints(nullptr, settings.window_, reinterpret_cast<POINT*>(&value), 2);
        return value;
    };
    check(settings.archive_rows_.size() == 6 && settings.archive_rows_[0].header == L"Test" &&
              settings.archive_rows_[2].kind == SettingsWindow::ArchiveRowKind::Header,
          "archives group project and explicit recent chats separately");
    HDC dc = GetDC(settings.archive_list_);
    const auto brush = reinterpret_cast<HBRUSH>(SendMessageW(settings.window_, WM_CTLCOLORLISTBOX,
        reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(settings.archive_list_)));
    LOGBRUSH color{}; GetObjectW(brush, sizeof(color), &color);
    ReleaseDC(settings.archive_list_, dc);
    check(color.lbColor == theme::For("light").background,
          "empty archive viewport uses the unframed page background");
    check(PaintMatches(settings, L"archives-light.jpg"), "light page is unframed and uses a light restore hover");
    settings.config_.ui.theme = "dark"; settings.ApplyTheme();
    check(PaintMatches(settings, L"archives-dark.jpg"), "dark page is unframed and uses the theme restore hover");
    settings.config_.ui.theme = "light"; settings.ApplyTheme();
    for (int width : {1100, 720, 560}) {
        SetWindowPos(owner, nullptr, 0, 0, width, 750, SWP_NOZORDER | SWP_NOACTIVATE);
        settings.Resize();
        const auto search = rect(settings.archive_search_);
        const auto scope = rect(settings.archive_scope_combo_);
        const auto list = rect(settings.archive_list_);
        const auto remove = rect(settings.delete_all_button_);
        const auto title = rect(settings.page_title_);
        const bool stacked = list.right - list.left < settings.Dpi(360);
        check(search.left == list.left && scope.right <= list.right &&
                  (stacked ? scope.left == list.left && scope.top >= search.bottom &&
                                 scope.bottom < list.top && search.right == list.right
                           : search.right < scope.left && search.top == scope.top && search.bottom == scope.bottom) &&
                  scope.right - scope.left >= settings.Dpi(140),
              "search and scope fit the archive content at wide and compact widths");
        check(remove.left >= list.left && remove.right <= list.right && title.right <= remove.left,
              "heading and delete-all stay within content without overlap");
        check(list.bottom <= rect(settings.archive_status_).top,
              "archive rows do not cover the bottom status");
    }
    SetWindowPos(owner, nullptr, 0, 0, 1100, 750, SWP_NOZORDER | SWP_NOACTIVATE);
    settings.Resize();
    SetWindowTextW(settings.archive_search_, L"Project");
    check(settings.archive_rows_.size() == 2, "search filters group rows");
    settings.archive_scope_ = SettingsWindow::ArchiveScope::Project;
    settings.archive_scope_project_ = 1;
    settings.RefreshArchived();
    check(settings.archive_rows_.empty(), "project scope and search are intersected");
    SetWindowTextW(settings.archive_search_, L"chat");
    check(settings.archive_rows_.size() == 2 && settings.archive_rows_[0].header == L"Other" &&
              fs::path(Utf8ToWide(settings.archive_entries_[settings.archive_rows_[1].entry_index].path)).filename() ==
                  fs::path("other.jsonl"),
          "project scope cannot leak matching chats from other projects or recent");
    settings.archive_scope_ = SettingsWindow::ArchiveScope::AllProjects;
    SetWindowTextW(settings.archive_search_, L"missing");
    check(settings.archive_rows_.empty() && IsWindowEnabled(settings.delete_all_button_),
          "a search with no matches does not disable deleting all actual archives");
    SetWindowTextW(settings.archive_search_, L"");
    settings.archive_scope_ = SettingsWindow::ArchiveScope::Recent;
    settings.RefreshArchived();
    check(settings.archive_rows_.size() == 2 &&
              settings.archive_entries_[settings.archive_rows_[1].entry_index].scope == "recent",
          "recent scope does not include project chats");
    RECT row{}; SendMessageW(settings.archive_list_, LB_GETITEMRECT, 1, reinterpret_cast<LPARAM>(&row));
    const POINT restore{row.right - settings.Dpi(30), (row.top + row.bottom) / 2};
    const LPARAM location = MAKELPARAM(restore.x, restore.y);
    settings.HandleArchiveListMove(restore);
    check(settings.archive_tooltip_text_ == i18n::Text(L"Unarchive"),
          "restore tooltip describes the action");
    SendMessageW(settings.archive_list_, WM_MOUSELEAVE, 0, 0);
    check(settings.archive_hover_row_ == -1 && settings.archive_tooltip_text_.empty(),
          "leaving the list clears hover and tooltip state");
    SendMessageW(settings.archive_list_, WM_LBUTTONUP, 0, location);
    check(fs::exists(directory / "sessions" / "archived" / "recent.jsonl") && changes == 0,
          "mouse-up without a matching press never restores a chat");
    SendMessageW(settings.archive_list_, WM_LBUTTONDOWN, MK_LBUTTON, location);
    check(GetCapture() == settings.archive_list_, "archive actions capture the matching press");
    SendMessageW(settings.archive_list_, WM_LBUTTONUP, 0, MAKELPARAM(20, restore.y));
    check(fs::exists(directory / "sessions" / "archived" / "recent.jsonl") && changes == 0,
          "dragging off an action cancels it");
    SendMessageW(settings.archive_list_, WM_LBUTTONDOWN, MK_LBUTTON, location);
    ReleaseCapture();
    SendMessageW(settings.archive_list_, WM_LBUTTONUP, 0, location);
    check(changes == 0 && settings.archive_pressed_row_ == -1,
          "capture loss cancels a pending action");
    SendMessageW(settings.archive_list_, WM_LBUTTONDBLCLK, 0, MAKELPARAM(20, 10));
    check(changes == 0, "double-clicking a group header never restores a file");
    EnableWindow(settings.archive_list_, FALSE);
    SendMessageW(settings.archive_list_, WM_LBUTTONDOWN, MK_LBUTTON, location);
    SendMessageW(settings.archive_list_, WM_LBUTTONUP, 0, location);
    check(changes == 0, "disabled archives cannot perform actions");
    EnableWindow(settings.archive_list_, TRUE);
    write("sessions/recent.jsonl", "existing session");
    SendMessageW(settings.archive_list_, WM_LBUTTONDOWN, MK_LBUTTON, location);
    SendMessageW(settings.archive_list_, WM_LBUTTONUP, 0, location);
    check(changes == 0 && ReadFileUtf8(WideToUtf8((directory / "sessions" / "recent.jsonl").wstring())) ==
              std::optional<std::string>("existing session") &&
              fs::exists(directory / "sessions" / "archived" / "recent.jsonl"),
          "a restore collision preserves both the active session and archive");
    fs::remove(directory / "sessions" / "recent.jsonl");
    SendMessageW(settings.archive_list_, WM_LBUTTONDOWN, MK_LBUTTON, location);
    SendMessageW(settings.archive_list_, WM_LBUTTONUP, 0, location);
    check(changes == 1 && fs::exists(directory / "sessions" / "recent.jsonl") &&
              !fs::exists(directory / "sessions" / "archived" / "recent.jsonl") &&
              ReadWindowText(settings.archive_status_) == i18n::Text(L"No archived chats."),
          "unarchive moves the selected synthetic file and refreshes the empty state");
    check(!fs::exists(directory / "config.json") &&
              fs::exists(directory / "sessions" / "archived" / "project.jsonl"),
          "archive actions preserve configuration and other archived chats");
    archive("sessions/archived/orphan.jsonl", "Orphan chat", "project", "C:/fixture/Removed");
    settings.RefreshArchived();
    const auto orphan_path = directory / "sessions" / "archived" / "orphan.jsonl";
    const auto original = ReadFileUtf8(WideToUtf8(orphan_path.wstring()));
    HANDLE locked = CreateFileW(orphan_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    settings.RestoreArchiveAt(settings.archive_rows_[1].entry_index);
    CloseHandle(locked);
    check(changes == 1 && ReadFileUtf8(WideToUtf8(orphan_path.wstring())) == original &&
              !fs::exists(directory / "sessions" / "orphan.jsonl"),
          "failed orphan preparation preserves the source and publishes no partial session");
    write("sessions/orphan.jsonl", "collision");
    settings.RestoreArchiveAt(settings.archive_rows_[1].entry_index);
    check(changes == 1 && ReadFileUtf8(WideToUtf8(orphan_path.wstring())) == original &&
              ReadFileUtf8(WideToUtf8((directory / "sessions" / "orphan.jsonl").wstring())) ==
                  std::optional<std::string>("collision"),
          "orphan restore collisions preserve the original history and destination");
    fs::remove(directory / "sessions" / "orphan.jsonl");
    SendMessageW(settings.archive_list_, LB_GETITEMRECT, 1, reinterpret_cast<LPARAM>(&row));
    SendMessageW(settings.archive_list_, WM_LBUTTONDBLCLK, 0, MAKELPARAM(20, (row.top + row.bottom) / 2));
    SessionJournal restored;
    const auto restored_path = WideToUtf8((directory / "sessions" / "orphan.jsonl").wstring());
    const auto restored_bytes = ReadFileUtf8(restored_path);
    check(changes == 2 && restored.Resume(restored_path).ok &&
              restored.State().metadata.value("scope", "") == "recent" && original && restored_bytes &&
              restored_bytes->compare(0, original->size(), *original) == 0 && !fs::exists(orphan_path),
          "body double-click restores an orphan into Recent without rewriting history");
    for (const auto& file : fs::directory_iterator(directory / "sessions"))
        check(file.path().extension() != ".tmp", "failed/successful restores leave no staging files");
    settings.archive_scope_ = SettingsWindow::ArchiveScope::Project;
    settings.archive_scope_project_ = 1;
    settings.RefreshArchived();
    SendMessageW(settings.archive_list_, LB_GETITEMRECT, 1, reinterpret_cast<LPARAM>(&row));
    SendMessageW(settings.archive_list_, WM_LBUTTONDBLCLK, 0,
                 MAKELPARAM(row.right - settings.Dpi(30), (row.top + row.bottom) / 2));
    check(changes == 2, "action-area double-click does not trigger a second restore");
    SendMessageW(settings.archive_list_, WM_LBUTTONDBLCLK, 0, MAKELPARAM(20, (row.top + row.bottom) / 2));
    SessionJournal project;
    check(changes == 3 && project.Resume(WideToUtf8((directory / "sessions" / "other.jsonl").wstring())).ok &&
              project.State().metadata.value("scope", "") == "project",
          "matching-project restores retain the project scope");
    settings.archive_scope_project_ = 0;
    settings.RefreshArchived();
    SetTimer(settings.window_, IDNO, 20, AnswerDialog);
    settings.DeleteArchiveAt(settings.archive_rows_[1].entry_index);
    KillTimer(settings.window_, IDNO);
    check(changes == 3 && fs::exists(directory / "sessions" / "archived" / "project.jsonl"),
          "cancelled deletion keeps the archived file");
    SetTimer(settings.window_, IDYES, 20, AnswerDialog);
    settings.DeleteArchiveAt(settings.archive_rows_[1].entry_index);
    KillTimer(settings.window_, IDYES);
    check(changes == 4 && !fs::exists(directory / "sessions" / "archived" / "project.jsonl") &&
              fs::exists(directory / "sessions" / "other.jsonl") && !fs::exists(directory / "config.json"),
          "confirmed deletion removes only the selected archive");
    const auto long_data = directory / std::string(189 - directory.wstring().size(), 'x');
    fs::create_directories(long_data / "sessions" / "archived");
    const auto long_name = std::string(39, 'a') + ".jsonl";
    const auto long_source = long_data / "sessions" / "archived" / long_name;
    {
        fs_ofstream output(long_source, std::ios::binary);
        output << R"({"type":"session.created","payload":{"session_id":"long-path","workspace":"C:/gone"}})"
               << "\n" << R"({"type":"session.scope","payload":{"scope":"project"}})" << "\n";
    }
    settings.config_.storage.data_dir = WideToUtf8(long_data.wstring());
    settings.archive_scope_ = SettingsWindow::ArchiveScope::Recent;
    settings.RefreshArchived();
    check(settings.archive_rows_.size() == 2, "long-path archive fixture is discoverable");
    if (settings.archive_rows_.size() == 2) {
        settings.RestoreArchiveAt(settings.archive_rows_[1].entry_index);
        check(changes == 5 && !fs::exists(long_source) && fs::exists(long_data / "sessions" / long_name),
              "orphan restores use short staging names within Win7/XP path limits");
    }
    const HWND tooltip = settings.archive_tooltip_, provider_tooltip = settings.provider_tooltip_;
    settings.Close();
    check(!IsWindow(tooltip) && !IsWindow(provider_tooltip), "closing settings destroys both popup tooltips");
    DestroyWindow(owner);
    std::error_code error; fs::remove_all(directory, error);
    if (!failures) std::cout << "Archive settings checks passed.\n";
    return failures ? 1 : 0;
}
};
}  // namespace spark

int main() {
    Gdiplus::GdiplusStartupInput input; ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
    const int result = spark::SettingsArchiveTest::Run();
    Gdiplus::GdiplusShutdown(token);
    return result;
}
