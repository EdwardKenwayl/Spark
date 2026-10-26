#include "gui/app.h"
#include "gui/localization.h"
#include "gui/model_picker.h"
#include "platform/window_text.h"

#include <commctrl.h>
#include <objidl.h>
using byte = BYTE;
#include <gdiplus.h>
#include <algorithm>
#include "platform/filesystem.h"
#include <fstream>
#include <iostream>
#include <utility>

namespace spark {

struct AppWindowComposerTest {
    inline static int failures = 0;

    static void Check(bool condition, const char* message) {
        if (!condition) {
            ++failures;
            std::cerr << "FAIL: " << message << '\n';
        }
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* app = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<AppWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (!app && (message == WM_PRINT || message == WM_PRINTCLIENT)) {
            RECT rect{};
            GetClientRect(window, &rect);
            HBRUSH brush = CreateSolidBrush(RGB(70, 150, 90));
            FillRect(reinterpret_cast<HDC>(wparam), &rect, brush);
            DeleteObject(brush);
            return 0;
        }
        if (!app && message == WM_PAINT) {
            PAINTSTRUCT paint{};
            HDC device = BeginPaint(window, &paint);
            HBRUSH brush = CreateSolidBrush(RGB(70, 150, 90));
            FillRect(device, &paint.rcPaint, brush);
            DeleteObject(brush);
            EndPaint(window, &paint);
            return 0;
        }
        if (app != nullptr && (message == WM_LBUTTONUP || message == kAddActionMessage ||
                               message == WM_MEASUREITEM || message == WM_DRAWITEM)) {
            return app->HandleMessage(message, wparam, lparam);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    struct ModelMenuProbe {
        AppWindow* app;
        bool reasoning;
        bool opened = false;
    };

    static LRESULT CALLBACK ModelMenuProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                          UINT_PTR, DWORD_PTR data) {
        if (message == WM_INITMENUPOPUP) {
            auto& probe = *reinterpret_cast<ModelMenuProbe*>(data);
            auto& app = *probe.app;
            HMENU menu = reinterpret_cast<HMENU>(wparam);
            probe.opened = true;
            if (probe.reasoning) {
                Check(GetMenuItemCount(menu) == 7 && app.reasoning_menu_targets_.size() == 7,
                      "chat reasoning contains only concrete supported levels");
                bool checked_default = false;
                for (const auto& [id, effort] : app.reasoning_menu_targets_) {
                    Check(!effort.empty(), "chat reasoning does not add a separate automatic choice");
                    if (effort == "medium") checked_default = (GetMenuState(menu, id, MF_BYCOMMAND) & MF_CHECKED) != 0;
                }
                Check(checked_default, "an unset effort checks the actual model default");
            } else {
                for (const auto& [id, target] : app.model_menu_targets_) {
                    Check(app.menu_item_texts_[id] == Utf8ToWide(target.model),
                          "model entries use only their ID, not legacy display names");
                    MEASUREITEMSTRUCT item{};
                    item.CtlType = ODT_MENU; item.itemID = id;
                    app.HandleMessage(WM_MEASUREITEM, 0, reinterpret_cast<LPARAM>(&item));
                    Check(item.itemHeight == 36, "model entries reserve only one text line");
                }
            }
            SetTimer(window, 98, 10, [](HWND owner, UINT, UINT_PTR id, DWORD) {
                KillTimer(owner, id); EndMenu();
            });
        }
        return DefSubclassProc(window, message, wparam, lparam);
    }

    static void TestModelMenus() {
        AppWindow app;
        app.instance_ = GetModuleHandleW(nullptr);
        ProviderConfig provider;
        provider.id = "fixture";
        ModelConfig model;
        model.id = "gpt-6-sol";
        model.display_name = "Legacy visible alias";
        provider.models = {model};
        app.config_.providers = {provider};
        app.config_.active = {provider.id, model.id, ""};
        app.ui_font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        app.window_ = CreateWindowW(L"SparkComposerTest", L"", WS_OVERLAPPEDWINDOW,
                                    -32000, -32000, 700, 500, nullptr, nullptr, app.instance_, &app);
        app.model_button_ = CreateWindowW(L"BUTTON", L"", WS_CHILD, 0, 0, 400, 36,
                                          app.window_, nullptr, app.instance_, nullptr);
        app.RefreshModelButton();
        const auto button_label = ReadWindowText(app.model_button_);
        Check(button_label == L"gpt-6-sol · " + std::wstring(i18n::Text(L"Medium")),
              "composer button displays the ID and resolved default, not a legacy alias");
        ShowWindow(app.window_, SW_SHOWNOACTIVATE);
        ModelMenuProbe probe{&app, true};
        SetWindowSubclass(app.window_, ModelMenuProc, 98, reinterpret_cast<DWORD_PTR>(&probe));
        app.ShowReasoningMenu({0, 0}, provider.id, model.id);
        Check(probe.opened, "the reasoning popup was inspected");
        probe.reasoning = false; probe.opened = false;
        app.ShowModelMenu({0, 0});
        Check(probe.opened, "the model popup was inspected");
        RemoveWindowSubclass(app.window_, ModelMenuProc, 98);
        DestroyWindow(app.window_);
        app.window_ = app.model_button_ = nullptr;
        app.ui_font_ = nullptr;
    }

    static void TestScrollableModelPicker() {
        HWND owner = CreateWindowW(L"SparkComposerTest", L"", WS_OVERLAPPEDWINDOW,
                                    -32000, -32000, 700, 500, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        for (const char* preference : {"dark", "light"}) {
            std::vector<ModelPickerRow> rows{{1, L"Fixture provider", ModelPickerKind::Header}};
            for (UINT index = 0; index < 40; ++index)
                rows.push_back({100 + index, L"model-" + std::to_wstring(index), ModelPickerKind::Model});
            rows.push_back({999, L"Reasoning effort", ModelPickerKind::Reasoning});
            SetTimer(owner, 97, 10, [](HWND window, UINT, UINT_PTR id, DWORD) {
                HWND popup = FindWindowW(L"SparkModelPicker", nullptr);
                if (!popup) return;
                KillTimer(window, id);
                auto* picker = reinterpret_cast<ModelPicker*>(GetWindowLongPtrW(popup, GWLP_USERDATA));
                Check(picker->visible_ <= 12 && picker->Thumb().right > 0,
                      "a long model list has a bounded viewport and visible scrollbar");
                SendMessageW(popup, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
                Check(picker->top_ > 0, "the model list scrolls with the mouse wheel");
                RECT thumb = picker->Thumb();
                const int x = (thumb.left + thumb.right) / 2, y = (thumb.top + thumb.bottom) / 2;
                SendMessageW(popup, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
                SendMessageW(popup, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y + picker->Scale(150)));
                SendMessageW(popup, WM_LBUTTONUP, 0, MAKELPARAM(x, y + picker->Scale(150)));
                Check(picker->top_ > 3, "the scrollbar thumb can be dragged");
                PaintAndCapture(popup, picker->palette_.panel == theme::DarkPalette().panel
                    ? L"model-picker-dark.bmp" : L"model-picker-light.bmp");
                SendMessageW(popup, WM_KEYDOWN, VK_END, 0);
                SendMessageW(popup, WM_KEYDOWN, VK_RETURN, 0);
            });
            ModelPicker picker;
            Check(picker.Track(owner, static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)),
                               {0, 600}, preference, std::move(rows), 100) == 139,
                  "keyboard navigation can select a model beyond the initial viewport");
        }
        DestroyWindow(owner);
    }

    static void TestAttachments() {
        const auto directory = spark::fs::temp_directory_path() / Utf8ToWide("spark-composer-" + NewSessionId());
        spark::fs::create_directories(directory);
        {
            AppWindow app;
            app.config_.storage.data_dir = WideToUtf8(directory.wstring());
            app.config_.workspace.path = app.config_.storage.data_dir;
            app.pending_new_conversation_ = true;
            app.window_ = CreateWindowExW(0, L"SparkComposerTest", L"", WS_OVERLAPPEDWINDOW,
                                         0, 0, 1280, 900, nullptr, nullptr, GetModuleHandleW(nullptr), &app);
            app.composer_ = CreateWindowExW(0, L"EDIT", L"keep draft", WS_CHILD, 0, 0, 0, 0,
                                           app.window_, nullptr, GetModuleHandleW(nullptr), nullptr);
            app.ui_font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            const std::string image = JoinPath(app.config_.storage.data_dir, "first.png");
            const std::string other_image = JoinPath(app.config_.storage.data_dir, "second.png");
            Check(WriteFileUtf8Atomic(image, "test fixture", nullptr), "test image file is created");
            app.attachments_ = {image, other_image};
            app.snapshot_contexts_[image] = "window: first";
            app.snapshot_contexts_[other_image] = "window: second";
            app.SaveComposerState();
            app.RestoreComposerState();
            Check(app.SnapshotContext().find("window: first") != std::string::npos,
                  "smart snapshot context survives draft reload");
            app.attachment_bitmaps_[image] = CreateBitmap(1, 1, 1, 32, nullptr);
            HDC device = GetDC(app.window_);
            const auto metrics = app.MeasureLayout();
            app.DrawAttachmentPreview(device, metrics);
            Check(app.attachment_hit_areas_.size() == 2, "both attachments have preview targets");
            const RECT preview = app.attachment_hit_areas_.front().preview;
            const RECT close = app.attachment_hit_areas_.front().remove;
            Check(preview.right - preview.left >= app.Dpi(140), "image preview is large enough to inspect");
            Check(close.right > close.left && close.bottom > close.top, "preview has a remove button");
            app.LayoutControls();
            RECT composer{};
            GetWindowRect(app.composer_, &composer);
            MapWindowPoints(nullptr, app.window_, reinterpret_cast<POINT*>(&composer), 2);
            Check(composer.top >= preview.bottom + app.Dpi(6), "preview does not cover the text input");
            app.DrawAttachmentPreview(device, app.MeasureLayout());
            const int x = (close.left + close.right) / 2;
            const int y = (close.top + close.bottom) / 2;
            SendMessageW(app.window_, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
            Check(app.attachments_ == std::vector<std::string>{other_image}, "X removes only the selected attachment");
            Check(app.SnapshotContext().find("window: first") == std::string::npos &&
                      app.SnapshotContext().find("window: second") != std::string::npos,
                  "removing a smart snapshot also removes its contextual information");
            Check(app.attachment_bitmaps_.count(image) == 0, "removal releases the preview bitmap");
            Check(spark::fs::exists(Utf8ToWide(image)), "removal preserves the original file");
            const auto saved = ReadFileUtf8(app.ComposerStatePath());
            Check(saved.has_value() && nlohmann::json::parse(*saved)["attachments"] == app.attachments_,
                  "removal persists in the conversation draft");
            app.RestoreComposerState();
            Check(app.attachments_ == std::vector<std::string>{other_image}, "removed attachment stays removed on restore");
            app.DrawAttachmentPreview(device, app.MeasureLayout());
            const RECT last_close = app.attachment_hit_areas_.front().remove;
            SendMessageW(app.window_, WM_LBUTTONUP, 0,
                         MAKELPARAM((last_close.left + last_close.right) / 2,
                                    (last_close.top + last_close.bottom) / 2));
            Check(app.attachments_.empty() && app.attachment_hit_areas_.empty(),
                  "removing the last image clears stale hit targets");
            ReleaseDC(app.window_, device);
            DestroyWindow(app.window_);
            app.window_ = nullptr;
            app.composer_ = nullptr;
            app.ui_font_ = nullptr;
        }
        std::error_code error;
        spark::fs::remove_all(directory, error);
    }

    static void TestErrors() {
        ChatView view;
        view.AddUser("first");
        const size_t first = view.AddError("old", 11);
        Check(view.AddError("replacement", 12) == first && view.Count() == 2 &&
                  view.Item(first)->text == "replacement" && view.Item(first)->timestamp == 12,
              "live error replaces the previous failure of the same turn");
        view.AddUser("second");
        view.AddError("second error", 13);
        Check(view.Count() == 4 && view.Item(first)->text == "replacement",
              "another turn keeps its own error");
    }

    static void TestExternalActivity() {
        const auto directory = spark::fs::temp_directory_path() / Utf8ToWide("spark-activity-" + NewSessionId());
        spark::fs::create_directories(directory / L"external_agents");
        {
            AppWindow app;
            app.config_.storage.data_dir = WideToUtf8(directory.wstring());
            Check(app.journal_.Open(app.config_.storage.data_dir, app.config_.storage.data_dir).ok,
                  "activity fixture session opens");
            const auto path = JoinPath(app.config_.storage.data_dir, "external_agents/process-fixture.json");
            nlohmann::json registry = {
                {"process_id", GetCurrentProcessId()}, {"updated_at", UnixTimeSeconds()},
                {"runs", nlohmann::json::array({{
                    {"session_path", app.journal_.Path()}, {"state", "running"}
                }})}
            };
            const auto publish = [&] {
                Check(WriteFileUtf8Atomic(path, registry.dump()), "activity fixture publishes");
                app.RefreshExternalRuns();
            };
            publish();
            Check(app.IsSessionRunning(app.journal_.Path()), "a live MCP heartbeat shows running");
            registry["updated_at"] = UnixTimeSeconds() - 60;
            publish();
            Check(!app.IsSessionRunning(app.journal_.Path()),
                  "stale MCP records cannot borrow an unrelated live process ID");
            registry["updated_at"] = UnixTimeSeconds();
            registry["runs"][0]["state"] = "completed";
            publish();
            Check(!app.IsSessionRunning(app.journal_.Path()), "completed MCP tasks are idle");
            registry["runs"][0]["state"] = "running";
            registry["process_id"] = 0;
            publish();
            Check(!app.IsSessionRunning(app.journal_.Path()), "a missing process ID is never running");
        }
        spark::fs::remove_all(directory);
    }

    static void TestComposerModes() {
        const auto directory = spark::fs::temp_directory_path() / Utf8ToWide("spark-modes-" + NewSessionId());
        spark::fs::create_directories(directory);
        {
            AppWindow app;
            app.config_.storage.data_dir = WideToUtf8(directory.wstring());
            app.config_.workspace.path = app.config_.storage.data_dir;
            app.pending_new_conversation_ = true;
            app.window_ = CreateWindowExW(0, L"SparkComposerTest", L"", WS_OVERLAPPEDWINDOW,
                                         0, 0, 1280, 900, nullptr, nullptr, GetModuleHandleW(nullptr), &app);
            app.composer_ = CreateWindowW(L"EDIT", L"unsent text", WS_CHILD, 0, 0, 0, 0,
                                          app.window_, nullptr, GetModuleHandleW(nullptr), nullptr);
            ProviderConfig provider; provider.id = "fixture";
            ModelConfig model; model.id = "deepseek-v4.1-flash";
            model.capabilities.default_reasoning_effort = "max";
            provider.models = {model}; app.config_.providers = {provider};
            app.config_.active = {provider.id, model.id, "low"};
            app.PreparePendingConversation("recent", "");
            Check(app.config_.active.reasoning_effort == "max",
                  "a new conversation starts with the selected model's configured default effort");
            app.config_.providers[0].models[0].capabilities.default_reasoning_effort.clear();
            app.PreparePendingConversation("project", "fixture project");
            Check(app.config_.active.reasoning_effort == "high",
                  "an automatic default also applies to a new project conversation");
            app.PreparePendingConversation("recent", "");
            SetWindowTextW(app.composer_, L"unsent text");
            app.goal_ = "Keep working toward the user's objective.";
            app.plan_mode_ = true;
            app.attachments_ = {"one-shot.jpg"};
            app.snapshot_contexts_["one-shot.jpg"] = "one-shot title";
            app.SaveComposerState();
            app.ClearComposer();
            app.ClearComposerState();
            app.RestoreComposerState();
            Check(app.goal_ == "Keep working toward the user's objective." && app.plan_mode_,
                  "sending preserves goal and plan mode across conversation reloads");
            Check(app.ComposerText().empty(), "sending still clears the text draft");
            Check(app.attachments_.empty() && app.SnapshotContext().empty(),
                  "sending removes snapshot images and metadata from the next draft");
            app.active_project_name_ = "another project";
            app.RestoreComposerState();
            Check(app.goal_.empty() && !app.plan_mode_, "another pending conversation has independent modes");
            app.active_project_name_.clear();
            app.RestoreComposerState();
            Check(!app.goal_.empty() && app.plan_mode_, "returning restores the original conversation's modes");
            app.goal_.clear();
            app.plan_mode_ = false;
            app.SaveComposerState();
            app.RestoreComposerState();
            Check(app.goal_.empty() && !app.plan_mode_, "clearing the goal and disabling plan mode persist");
            app.goal_ = "Materialized goal";
            app.plan_mode_ = true;
            app.attachments_ = {"one-shot.jpg"};
            app.snapshot_contexts_["one-shot.jpg"] = "Materialized snapshot";
            app.SaveComposerState();
            const std::string pending_draft = app.ComposerStatePath();
            app.active_workspace_path_ = app.config_.workspace.path;
            Check(app.MaterializePendingConversation(), "pending draft becomes a saved conversation");
            Check(!PathExists(pending_draft) && PathExists(app.ComposerStatePath()),
                  "materialization moves the draft out of the old pending scope before its path changes");
            app.ClearComposer();
            app.ClearComposerState();
            app.RestoreComposerState();
            Check(app.goal_ == "Materialized goal" && app.plan_mode_ && app.attachments_.empty(),
                  "saved conversation retains modes but not already-sent snapshots");
            auto runtime = std::make_shared<AppWindow::TurnRuntime>();
            runtime->session_key = NormalizePathForComparison(app.journal_.Path());
            app.active_turns_[runtime->session_key] = runtime;
            app.ToggleSendAction();
            Check(runtime->cancellation.IsCancelled() && runtime->interrupt_requested.load(),
                  "the send button interrupts a running turn even before any model output");
            app.active_turns_.clear();
            app.pending_new_conversation_ = true;
            app.active_workspace_path_ = app.config_.workspace.path;
            app.RestoreComposerState();
            Check(app.goal_.empty() && !app.plan_mode_ && app.attachments_.empty(),
                  "a fresh conversation cannot reload the previous pending draft's attachments or modes");
            DestroyWindow(app.window_);
            app.window_ = nullptr;
            app.composer_ = nullptr;
        }
        std::error_code error;
        spark::fs::remove_all(directory, error);
    }

    static void TestAddMenu() {
        AppWindow app;
        app.instance_ = GetModuleHandleW(nullptr);
        app.window_ = CreateWindowExW(0, L"SparkComposerTest", L"", WS_OVERLAPPEDWINDOW,
                                     0, 0, 1280, 900, nullptr, nullptr, app.instance_, &app);
        app.ui_font_ = CreateFontW(-app.Dpi(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        for (const auto* preference : {"dark", "light"}) {
            app.config_.ui.theme = preference;
            app.goal_ = "A long goal that should remain inside its row without overlapping another entry.";
            app.ShowAddMenu({});
            Check(app.add_menu_ && IsWindow(app.add_menu_->hwnd()), "plus opens the custom menu");
            RECT popup{};
            GetWindowRect(app.add_menu_->hwnd(), &popup);
            Check(popup.right - popup.left > app.Dpi(400) && popup.bottom - popup.top == app.Dpi(208),
                  "menu has a wide panel and exactly four rows");
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromWindow(app.add_menu_->hwnd(), MONITOR_DEFAULTTONEAREST), &monitor);
            Check(popup.left >= monitor.rcWork.left && popup.right <= monitor.rcWork.right &&
                      popup.top >= monitor.rcWork.top && popup.bottom <= monitor.rcWork.bottom,
                  "popup stays inside the active monitor work area");
            app.add_menu_->Hide();
            PaintAndCapture(app.add_menu_->hwnd(), preference == std::string("dark")
                                ? L"spark-add-dark.bmp" : L"spark-add-light.bmp");
            SendMessageW(app.add_menu_->hwnd(), WM_MOUSEMOVE, 0, MAKELPARAM(app.Dpi(70), app.Dpi(179)));
            SendMessageW(app.add_menu_->hwnd(), WM_KEYDOWN, VK_RETURN, 0);
            MSG message{};
            Check(PeekMessageW(&message, app.window_, kAddActionMessage, kAddActionMessage, PM_REMOVE) &&
                      message.wParam == static_cast<WPARAM>(AddAction::Plan),
                  "keyboard activates the currently highlighted plan row");
            SendMessageW(app.add_menu_->hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(-1, -1));
            Check(!PeekMessageW(&message, app.window_, kAddActionMessage, kAddActionMessage, PM_REMOVE),
                  "outside click closes the menu without triggering an action");
        }
        Check(!CaptureSmartSnapshot(nullptr, "").ok, "closed snapshot target is rejected");
        app.add_menu_.reset();
        DestroyWindow(app.window_);
        app.window_ = nullptr;
        DeleteObject(app.ui_font_);
        app.ui_font_ = nullptr;
    }

    static void TestSnapshotCapture() {
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        Check(Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok, "snapshot encoder starts");
        const auto directory = spark::fs::temp_directory_path() / Utf8ToWide("spark-snapshot-" + NewSessionId());
        const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, L"SparkComposerTest", L"Snapshot fixture",
                                            WS_POPUP | WS_VISIBLE, 30, 30, 320, 180, nullptr, nullptr,
                                            GetModuleHandleW(nullptr), nullptr);
        SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        HDC device = GetDC(window);
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH brush = CreateSolidBrush(RGB(70, 150, 90));
        FillRect(device, &client, brush);
        DeleteObject(brush);
        ReleaseDC(window, device);
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm) {
            const auto flush = reinterpret_cast<HRESULT(WINAPI*)()>(GetProcAddress(dwm, "DwmFlush"));
            if (flush) flush();
            FreeLibrary(dwm);
        }
        const auto captured = CaptureSmartSnapshot(window, JoinPath(WideToUtf8(directory.wstring()), "fixture.jpg"));
        Check(captured.ok && captured.value.context.find("Snapshot fixture") != std::string::npos,
              "snapshot saves an image and the real window title");
        if (captured.ok) {
            Gdiplus::Bitmap image(Utf8ToWide(captured.value.image_path).c_str());
            Check(image.GetLastStatus() == Gdiplus::Ok && image.GetWidth() == 320 && image.GetHeight() == 180,
                  "snapshot is a decodable JPEG of the selected window bounds");
            Gdiplus::Color pixel;
            const bool read = image.GetPixel(160, 90, &pixel) == Gdiplus::Ok;
            Check(read && pixel.GetG() > pixel.GetR() + 30 && pixel.GetG() > pixel.GetB() + 30,
                  "snapshot contains the fixture window pixels instead of a blank image");
        }
        DestroyWindow(window);
        Check(!CaptureSmartSnapshot(window, WideToUtf8(directory.wstring())).ok,
              "capture rejects a window that closed before the request");
        std::error_code error;
        spark::fs::remove_all(directory, error);
        if (token) Gdiplus::GdiplusShutdown(token);
        // Exercise the hook's routing without installing a global hook or
        // injecting keyboard input into any application.
        SnapshotShortcut shortcut;
        shortcut.owner_ = CreateWindowW(L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                        nullptr, GetModuleHandleW(nullptr), nullptr);
        shortcut.message_ = kSnapshotMessage;
        SnapshotShortcut::active_ = &shortcut;
        KBDLLHOOKSTRUCT key{};
        key.vkCode = VK_LMENU;
        SnapshotShortcut::KeyboardProc(HC_ACTION, WM_SYSKEYDOWN, reinterpret_cast<LPARAM>(&key));
        MSG message{};
        Check(!PeekMessageW(&message, shortcut.owner_, kSnapshotMessage, kSnapshotMessage, PM_REMOVE),
              "one Alt key does not attach a snapshot");
        key.vkCode = VK_RMENU;
        key.flags = LLKHF_EXTENDED;
        SnapshotShortcut::KeyboardProc(HC_ACTION, WM_SYSKEYDOWN, reinterpret_cast<LPARAM>(&key));
        Check(PeekMessageW(&message, shortcut.owner_, kSnapshotMessage, kSnapshotMessage, PM_REMOVE) != FALSE,
              "pressing both Alt keys queues a snapshot request");
        SnapshotShortcut::KeyboardProc(HC_ACTION, WM_SYSKEYDOWN, reinterpret_cast<LPARAM>(&key));
        Check(!PeekMessageW(&message, shortcut.owner_, kSnapshotMessage, kSnapshotMessage, PM_REMOVE),
              "holding the shortcut does not repeatedly capture");
        SnapshotShortcut::KeyboardProc(HC_ACTION, WM_SYSKEYUP, reinterpret_cast<LPARAM>(&key));
        key.vkCode = VK_LMENU;
        key.flags = 0;
        SnapshotShortcut::KeyboardProc(HC_ACTION, WM_SYSKEYUP, reinterpret_cast<LPARAM>(&key));
        SnapshotShortcut::KeyboardProc(HC_ACTION, WM_SYSKEYDOWN, reinterpret_cast<LPARAM>(&key));
        key.vkCode = VK_RMENU;
        key.flags = LLKHF_EXTENDED;
        SnapshotShortcut::KeyboardProc(HC_ACTION, WM_SYSKEYDOWN, reinterpret_cast<LPARAM>(&key));
        Check(PeekMessageW(&message, shortcut.owner_, kSnapshotMessage, kSnapshotMessage, PM_REMOVE) != FALSE,
              "releasing both keys rearms the shortcut");
        shortcut.Stop();
        DestroyWindow(shortcut.owner_);
    }

    static void TestSnapshotDraftScope() {
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        Gdiplus::GdiplusStartup(&token, &input, nullptr);
        const auto directory = spark::fs::temp_directory_path() / Utf8ToWide("spark-snapshot-draft-" + NewSessionId());
        {
            AppWindow app;
            app.config_.storage.data_dir = WideToUtf8(directory.wstring());
            app.config_.workspace.path = app.config_.storage.data_dir;
            app.pending_new_conversation_ = true;
            app.window_ = CreateWindowExW(0, L"SparkComposerTest", L"", WS_OVERLAPPEDWINDOW,
                                         0, 0, 1280, 900, nullptr, nullptr, GetModuleHandleW(nullptr), &app);
            app.composer_ = CreateWindowW(L"EDIT", L"", WS_CHILD, 0, 0, 0, 0, app.window_,
                                          nullptr, GetModuleHandleW(nullptr), nullptr);
            HWND target = CreateWindowExW(WS_EX_TOOLWINDOW, L"SparkComposerTest", L"Snapshot fixture",
                                          WS_POPUP | WS_VISIBLE, 30, 30, 320, 180, nullptr, nullptr,
                                          GetModuleHandleW(nullptr), nullptr);
            UpdateWindow(target);
            app.snapshot_shortcut_.target_ = target;
            app.snapshot_shortcut_.target_process_ = GetCurrentProcessId();
            const auto finish = [&] {
                const uint64_t deadline = GetTickCount64() + 5000;
                while (app.snapshot_capture_.Pending() && GetTickCount64() < deadline) {
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                    app.PollSmartSnapshot();
                    MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
                }
                Check(!app.snapshot_capture_.Pending(), "draft snapshot finishes without blocking message dispatch");
            };
            app.AttachSmartSnapshot();
            finish();
            Check(app.attachments_.size() == 1 && !app.SnapshotContext().empty(),
                  "completed background snapshot is added to the requesting draft");
            app.ClearComposerState();
            app.AttachSmartSnapshot();
            app.active_project_name_ = "another conversation";
            finish();
            Check(app.attachments_.empty(), "delayed snapshot cannot appear in another conversation");
            app.AttachSmartSnapshot();
            app.ClearComposerState();
            Check(!app.snapshot_capture_.Pending() && app.attachments_.empty(),
                  "sending cancels an unfinished snapshot rather than attaching it to the next turn");
            DestroyWindow(target);
            DestroyWindow(app.window_);
            app.window_ = app.composer_ = nullptr;
        }
        std::error_code error;
        spark::fs::remove_all(directory, error);
        Gdiplus::GdiplusShutdown(token);
    }

    static void TestHeaderLayout() {
        AppWindow app;
        app.config_.storage.data_dir =
            WideToUtf8((spark::fs::temp_directory_path() / Utf8ToWide("spark-header-" + NewSessionId())).wstring());
        app.window_ = CreateWindowExW(0, L"SparkComposerTest", L"", WS_OVERLAPPEDWINDOW,
                                     0, 0, 1280, 900, nullptr, nullptr, GetModuleHandleW(nullptr), &app);
        app.ui_font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        const auto control = [&](const wchar_t* text) {
            return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                                  0, 0, 0, 0, app.window_, nullptr, GetModuleHandleW(nullptr), nullptr);
        };
        app.main_title_label_ = control(L"A conversation with a very long title");
        app.context_label_ = control(L"上下文 258K / 1000K");
        app.summary_button_ = control(L"摘要");
        app.subagent_button_ = control(L"子智能体 4");
        app.permission_button_ = control(L"请求批准");
        app.model_button_ = control(L"configured-model");
        app.send_button_ = control(L"Send");
        const auto rect = [&](HWND window) {
            RECT value{};
            GetWindowRect(window, &value);
            MapWindowPoints(nullptr, app.window_, reinterpret_cast<POINT*>(&value), 2);
            return value;
        };
        for (const int width : {1280, 980}) {
            for (const int font_height : {14, 22}) {
                HFONT font = CreateFontW(-font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
                app.ui_font_ = font;
                SetWindowPos(app.window_, nullptr, 0, 0, width, 900, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                for (const bool children : {false, true}) {
                    app.subagents_ui_.clear();
                    if (children) app.subagents_ui_["child"].info.id = "child";
                    app.side_chat_open_ = children;
                    app.LayoutControls();
                    const RECT title = rect(app.main_title_label_);
                    const RECT context = rect(app.context_label_);
                    const RECT summary = rect(app.summary_button_);
                    Check(title.right <= context.left, "conversation title does not cover context");
                    Check(context.right + app.Dpi(6) <= summary.left,
                          "context has a clear gap before header buttons");
                    Check(summary.right - summary.left == app.Dpi(32),
                          "summary uses one stable square icon");
                    Check(!IsWindowVisible(app.subagent_button_),
                          "child agents are reached through the summary icon");
                    Check(summary.right <= app.MeasureLayout().main_right,
                          "header remains inside the main pane with a child chat open");
                    Check(rect(app.permission_button_).right <= rect(app.model_button_).left &&
                              rect(app.model_button_).right <= rect(app.send_button_).left,
                          "composer selectors remain separate with the side panel open");
                }
                app.ui_font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
                DeleteObject(font);
            }
        }
        DestroyWindow(app.window_);
        app.window_ = nullptr;
        app.ui_font_ = nullptr;
    }

    static void PaintAndCapture(HWND window, const wchar_t* name) {
        RECT client{};
        GetClientRect(window, &client);
        HDC screen = GetDC(window);
        HDC dc = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = client.right;
        info.bmiHeader.biHeight = -client.bottom;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        const auto old = SelectObject(dc, bitmap);
        SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
        const DWORD bytes = client.right * client.bottom * 4;
        if (const wchar_t* directory = _wgetenv(L"SPARK_SUMMARY_SNAPSHOT_DIR")) {
            const auto path = spark::fs::path(directory) / name;
            BITMAPFILEHEADER header{};
            header.bfType = 0x4d42;
            header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
            header.bfSize = header.bfOffBits + bytes;
            spark::fs_ofstream output(path, std::ios::binary);
            output.write(reinterpret_cast<const char*>(&header), sizeof(header));
            output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(BITMAPINFOHEADER));
            output.write(static_cast<const char*>(pixels), bytes);
        }
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
        ReleaseDC(window, screen);
    }

    static void TestSummary() {
        AppWindow app;
        app.instance_ = GetModuleHandleW(nullptr);
        app.window_ = CreateWindowExW(0, L"SparkComposerTest", L"", WS_OVERLAPPEDWINDOW,
                                     0, 0, 1280, 900, nullptr, nullptr, app.instance_, &app);
        app.ui_font_ = CreateFontW(-app.Dpi(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        app.summary_button_ = CreateWindowW(L"BUTTON", L"", WS_CHILD, 0, 0, 32, 32,
                                           app.window_, nullptr, app.instance_, nullptr);
        app.side_chat_view_ = std::make_unique<ChatView>();
        app.side_chat_view_->Create(app.window_, app.instance_, 530, app.ui_font_, app.ui_font_);
        app.CreateSummaryControls();
        const auto directory = spark::fs::temp_directory_path() / Utf8ToWide("spark-summary-" + NewSessionId());
        Check(app.journal_.Open(WideToUtf8(directory.wstring()), WideToUtf8(directory.wstring())).ok,
              "summary uses an isolated session");
        app.config_.storage.data_dir = WideToUtf8(directory.wstring());
        const std::string parent = NormalizePathForComparison(app.journal_.Path());
        auto popup_rows = app.SummaryRows(true);
        Check(std::none_of(popup_rows.begin(), popup_rows.end(), [](const auto& row) {
                  return row.title == L"子智能体" || row.action == SummaryAction::Agents;
              }), "an empty conversation has no subagent section or placeholder avatars");
        app.summary_popup_->SetContent(SummaryTab::Agents, popup_rows);
        MoveWindow(app.summary_popup_->hwnd(), 0, 0, 320, 180, FALSE);
        PaintAndCapture(app.summary_popup_->hwnd(), L"spark-summary-popup-empty.bmp");
        for (int index = 0; index < 13; ++index) {
            auto& info = app.subagents_ui_["agent-" + std::to_string(index)].info;
            info.id = "agent-" + std::to_string(index);
            info.parent_session_key = parent;
            info.name = index == 0 ? "Goodall" : index == 1 ? "Carver" : "Read-only review " + std::to_string(index + 1);
            info.task = "Read the project and report the findings.";
            info.output = "The review is complete.";
            info.state = SubAgentState::Completed;
            info.started_at = info.finished_at = UnixTimeSeconds() - (index + 1) * 86400;
            info.elapsed_ms = 96500;
            info.model = "configured-model";
            info.reasoning_effort = "ultra";
            if (index == 0) {
                popup_rows = app.SummaryRows(true);
                Check(std::count_if(popup_rows.begin(), popup_rows.end(), [](const auto& row) {
                          return row.kind == SummaryRowKind::Agent;
                      }) == 1, "one child has exactly one named avatar");
                app.summary_popup_->SetContent(SummaryTab::Agents, popup_rows);
                MoveWindow(app.summary_popup_->hwnd(), 0, 0, 320, 210, FALSE);
                PaintAndCapture(app.summary_popup_->hwnd(), L"spark-summary-popup-one.bmp");
            }
            if (index == 1) {
                popup_rows = app.SummaryRows(true);
                Check(std::count_if(popup_rows.begin(), popup_rows.end(), [](const auto& row) {
                          return row.kind == SummaryRowKind::Agent;
                      }) == 2, "a small subagent menu shows the real names and child IDs");
                Check(std::any_of(popup_rows.begin(), popup_rows.end(), [](const auto& row) {
                          return row.id == "agent-0" && row.title == L"Goodall" &&
                                 row.action == SummaryAction::OpenAgent;
                      }), "a named menu item opens its own child instead of a generic count");
                app.summary_popup_->SetContent(SummaryTab::Agents, popup_rows);
                MoveWindow(app.summary_popup_->hwnd(), 0, 0, 320, 240, FALSE);
                PaintAndCapture(app.summary_popup_->hwnd(), L"spark-summary-popup-two.bmp");
                app.summary_popup_->SetAppearance("light");
                PaintAndCapture(app.summary_popup_->hwnd(), L"spark-summary-popup-two-light.bmp");
                app.summary_popup_->SetAppearance(app.config_.ui.theme);
            }
            if (index == 3 || index == 4) {
                popup_rows = app.SummaryRows(true);
                const auto count = std::count_if(popup_rows.begin(), popup_rows.end(), [](const auto& row) {
                    return row.kind == SummaryRowKind::Agent;
                });
                Check(count == (index == 3 ? 4 : 0), "four names fit directly; five children use the compact group");
            }
        }
        app.subagents_ui_["foreign"].info.parent_session_key = "another-session";
        popup_rows = app.SummaryRows(true);
        const auto group = std::find_if(popup_rows.begin(), popup_rows.end(), [](const auto& row) {
            return row.kind == SummaryRowKind::AgentGroup;
        });
        Check(group != popup_rows.end() && group->title == L"13 完成" &&
                  group->agent_ids == std::vector<std::string>{"agent-0", "agent-1", "agent-2", "agent-3"},
              "group avatars and completed count come from this conversation's actual children");
        app.summary_popup_->SetContent(SummaryTab::Agents, popup_rows);
        MoveWindow(app.summary_popup_->hwnd(), 0, 0, 320, 230, FALSE);
        PaintAndCapture(app.summary_popup_->hwnd(), L"spark-summary-popup-many.bmp");
        SendMessageW(app.summary_popup_->hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(app.Dpi(80), app.Dpi(60)));
        popup_rows = app.SummaryRows(true);
        Check(app.summary_agents_expanded_ && !app.side_chat_open_ &&
                  std::count_if(popup_rows.begin(), popup_rows.end(), [](const auto& row) {
                      return row.kind == SummaryRowKind::Agent;
                  }) == 13, "clicking the compact group expands real names inside the same menu");
        MoveWindow(app.summary_popup_->hwnd(), 0, 0, 320, 720, FALSE);
        PaintAndCapture(app.summary_popup_->hwnd(), L"spark-summary-popup-expanded.bmp");
        SendMessageW(app.summary_popup_->hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(app.Dpi(80), app.Dpi(25)));
        Check(!app.summary_agents_expanded_, "the subagent heading folds the expanded list again");
        app.subagents_ui_["agent-0"].info.state = SubAgentState::Running;
        app.subagents_ui_["agent-1"].info.state = SubAgentState::Failed;
        app.subagents_ui_["agent-2"].info.state = SubAgentState::Cancelled;
        popup_rows = app.SummaryRows(true);
        Check(std::any_of(popup_rows.begin(), popup_rows.end(), [](const auto& row) {
                  return row.kind == SummaryRowKind::AgentGroup && row.title == L"1 运行中 · 10 完成 · 1 失败 · 1 已停止";
              }), "failed and cancelled children do not inflate the completed count");
        for (int index = 0; index < 3; ++index) app.subagents_ui_["agent-" + std::to_string(index)].info.state = SubAgentState::Completed;
        auto rows = app.SummaryRows(false);
        Check(std::count_if(rows.begin(), rows.end(), [](const auto& row) { return row.kind == SummaryRowKind::Agent; }) == 10,
              "only ten finished children of this parent initially appear");
        Check(rows.back().action == SummaryAction::More && rows.back().title.find(L"3") != std::wstring::npos,
              "remaining agents use show-more");
        app.HandleSummaryAction(SummaryAction::More, {});
        rows = app.SummaryRows(false);
        Check(std::count_if(rows.begin(), rows.end(), [](const auto& row) { return row.kind == SummaryRowKind::Agent; }) == 13,
              "show-more reveals only this parent's children");
        app.pending_new_conversation_ = true;
        rows = app.SummaryRows(false);
        Check(std::none_of(rows.begin(), rows.end(),
                           [](const auto& row) { return row.kind == SummaryRowKind::Agent; }),
              "a pending conversation has no inherited children");
        popup_rows = app.SummaryRows(true);
        Check(std::none_of(popup_rows.begin(), popup_rows.end(), [](const auto& row) {
                  return row.kind == SummaryRowKind::Agent || row.kind == SummaryRowKind::AgentGroup ||
                         row.title == L"子智能体";
              }), "a new conversation hides the entire previous conversation's child section");
        app.pending_new_conversation_ = false;
        ChatMessage message;
        message.image_paths = {"C:\\pictures\\example.png"};
        message.sources = {{"image", "Viewed image", "c:/pictures/example.png"},
                           {"web", "Source title", "https://example.com/article"},
                           {"mcp", "MCP tool", "mcp__reader__read"}};
        app.journal_.MutableState().messages.push_back(message);
        message.image_paths.clear();
        message.sources = {{"web", "Updated title", "https://example.com/article"},
                           {"tool", "File tool", "read_file"}};
        app.journal_.MutableState().messages.push_back(message);
        Check(CollectSummarySources(app.journal_.State()).size() == 4,
              "sources span the full transcript with normalized-path and URL deduplication");
        app.HandleSummaryAction(SummaryAction::Sources, {});
        Check(app.side_chat_open_ && app.summary_tab_ == SummaryTab::Sources && app.active_subagent_id_.empty(),
              "view-all opens the sources tab");
        MoveWindow(app.summary_panel_->hwnd(), 0, 0, 500, 720, FALSE);
        PaintAndCapture(app.summary_panel_->hwnd(), L"spark-summary-sources.bmp");
        SendMessageW(app.summary_panel_->hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(app.Dpi(40), app.Dpi(20)));
        Check(app.summary_tab_ == SummaryTab::Agents, "the subagent tab is clickable");
        MoveWindow(app.summary_panel_->hwnd(), 0, 0, 500, 720, FALSE);
        PaintAndCapture(app.summary_panel_->hwnd(), L"spark-summary-agents.bmp");
        app.summary_agents_expanded_ = true;
        app.summary_popup_->SetContent(SummaryTab::Agents, app.SummaryRows(true));
        MoveWindow(app.summary_popup_->hwnd(), 0, 0, 320, 720, FALSE);
        PaintAndCapture(app.summary_popup_->hwnd(), L"spark-summary-popup-expanded.bmp");
        SendMessageW(app.summary_popup_->hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(app.Dpi(80), app.Dpi(60)));
        Check(app.active_subagent_id_ == "agent-0" && app.side_chat_open_,
              "clicking a child name opens that child's side chat");
        app.OpenSubAgentChat("agent-0");
        Check(app.active_subagent_id_ == "agent-0" && app.side_chat_view_->Count() >= 3 &&
                  app.side_chat_view_->Item(1)->elapsed_ms == 96500,
              "child detail retains messages and recorded elapsed time");
        auto& live = app.subagents_ui_["agent-0"];
        live.info.state = SubAgentState::Running;
        live.info.started_tick_ms = GetTickCount64() - 3000;
        app.RefreshSubAgentChat();
        const auto emit = [&](AppWindow::EventKind kind, std::string first = {},
                              std::string second = {}, std::string third = {}, bool success = false) {
            auto* event = new AppWindow::UiEvent;
            event->kind = kind;
            event->subagent_id = "agent-0";
            event->session_key = parent;
            event->first = std::move(first);
            event->second = std::move(second);
            event->third = std::move(third);
            event->success = success;
            app.HandleUiEvent(event);
        };
        emit(AppWindow::EventKind::SubAgentReasoning, "Inspecting the project.");
        const size_t thought = app.side_reasoning_entry_;
        Check(thought < app.side_chat_view_->Count() &&
                  app.side_chat_view_->Item(thought)->kind == ChatItemKind::Reasoning,
              "reasoning streams into its own one-line row");
        emit(AppWindow::EventKind::SubAgentText, "I will read the root directory.");
        Check(!app.side_chat_view_->Item(thought)->visible,
              "answer output removes the temporary reasoning row");
        emit(AppWindow::EventKind::SubAgentToolStart, "read-1", "list_directory", "{\"path\":\".\"}");
        const size_t command = app.side_tool_entries_["read-1"];
        emit(AppWindow::EventKind::SubAgentToolFinish, "read-1", "list_directory", "Source\nContent", true);
        Check(app.side_chat_view_->Item(command)->finished && app.side_chat_view_->Item(command)->result == "Source\nContent",
              "child commands use the normal expandable tool records");
        app.side_chat_view_->ToggleTool(command);
        Check(app.side_chat_view_->Item(command)->expanded, "tool details can be expanded in the side chat");
        emit(AppWindow::EventKind::SubAgentPhase, "Inspecting the results.");
        emit(AppWindow::EventKind::SubAgentReasoning, "The directory contains two folders.");
        const size_t next_thought = app.side_reasoning_entry_;
        auto* reasoning = new AppWindow::UiEvent;
        reasoning->kind = AppWindow::EventKind::SubAgentReasoning;
        reasoning->subagent_id = "foreign";
        reasoning->session_key = "another-session";
        reasoning->first = "background thinking";
        app.HandleUiEvent(reasoning);
        Check(app.side_reasoning_entry_ == next_thought,
              "another child's event does not alter the open detail");
        auto* completed = new AppWindow::UiEvent;
        completed->kind = AppWindow::EventKind::SubAgentFinished;
        completed->session_key = parent;
        completed->subagent_id = "agent-0";
        completed->child_info = live.info;
        completed->child_info.state = SubAgentState::Completed;
        completed->child_info.output = "The root contains **Source** and **Content**.";
        completed->child_info.elapsed_ms = 96500;
        app.HandleUiEvent(completed);
        Check(app.side_reasoning_entry_ == static_cast<size_t>(-1) &&
                  app.side_chat_view_->Item(1)->elapsed_ms == 96500,
              "completion clears live reasoning and records the true elapsed time");
        PaintAndCapture(app.summary_panel_->hwnd(), L"spark-summary-detail-header.bmp");
        SendMessageW(app.summary_panel_->hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(app.Dpi(24), app.Dpi(64)));
        Check(app.active_subagent_id_.empty() && app.side_chat_open_, "back returns to the child list");
        app.summary_popup_->SetContent(SummaryTab::Agents, app.SummaryRows(true));
        MoveWindow(app.summary_popup_->hwnd(), 0, 0, 320, 300, FALSE);
        PaintAndCapture(app.summary_popup_->hwnd(), L"spark-summary-popup.bmp");
        app.HandleSummaryAction(SummaryAction::Close, {});
        Check(!app.side_chat_open_, "close releases the side panel width");
        app.summary_popup_.reset();
        app.side_chat_view_.reset();
        app.summary_panel_.reset();
        DestroyWindow(app.window_);
        app.window_ = nullptr;
        DeleteObject(app.ui_font_);
        app.ui_font_ = nullptr;
        std::error_code error;
        spark::fs::remove_all(directory, error);
    }

    static int Run(bool external_only = false) {
        if (external_only) {
            TestExternalActivity();
            return failures == 0 ? 0 : 1;
        }
        WNDCLASSW window_class{};
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpfnWndProc = WindowProc;
        window_class.lpszClassName = L"SparkComposerTest";
        if (!RegisterClassW(&window_class)) return 1;
        // Hidden windows and temporary drafts only; never start the real GUI or
        // load the user's configuration, clipboard, model, or conversation data.
        const std::pair<const char*, void (*)()> checks[] = {
            {"attachments", TestAttachments}, {"errors", TestErrors},
            {"external activity", TestExternalActivity}, {"composer modes", TestComposerModes},
            {"add menu", TestAddMenu}, {"snapshot capture", TestSnapshotCapture},
            {"snapshot draft scope", TestSnapshotDraftScope}, {"header layout", TestHeaderLayout},
            {"model menus", TestModelMenus}, {"scrollable model picker", TestScrollableModelPicker},
            {"summary", TestSummary},
        };
        for (const auto& [name, check] : checks) {
            std::cout << "START: " << name << std::endl;
            const auto started = GetTickCount64();
            check();
            std::cout << "DONE: " << name << " (" << GetTickCount64() - started << " ms)" << std::endl;
        }
        UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
        if (failures == 0) std::cout << "Composer checks passed.\n";
        return failures == 0 ? 0 : 1;
    }
};

}  // namespace spark

int main(int argc, char** argv) {
    if (const auto helper = spark::RunSnapshotHelper()) return *helper;
    return spark::AppWindowComposerTest::Run(argc > 1 && std::string(argv[1]) == "--external-state");
}
