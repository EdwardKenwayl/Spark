#include "gui/app.h"
#include "gui/session_loader.h"
#include "gui/theme.h"

#include <commctrl.h>
#include <objidl.h>
using byte = BYTE;
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include "platform/filesystem.h"
#include <functional>
#include <iostream>
#include <vector>

namespace spark {
namespace {

constexpr wchar_t kTestClass[] = L"SparkSessionLoadingTest";
constexpr UINT kProbeMessage = WM_APP + 90;
constexpr size_t kLongTurns = 2000;
constexpr size_t kMaxFixtureBytes = 1024 * 1024;
int failures = 0;
uint64_t deadline = 0;
size_t probes = 0;
bool probe_pending = false;
uint64_t last_probe_sent_tick = 0;
size_t app_timers = 0;
size_t chat_timers = 0;
uint64_t max_probe_delay = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void PumpOnce() {
    MSG message{};
    if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message != WM_QUIT) {
            if (message.message == WM_TIMER && GetClassLongPtrW(message.hwnd, GCW_ATOM) !=
                    GetClassLongPtrW(GetAncestor(message.hwnd, GA_ROOT), GCW_ATOM)) ++chat_timers;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    } else {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 2, QS_ALLINPUT);
    }
}

void PostProbe(HWND window) {
    const uint64_t now = GetTickCount64();
    if (probe_pending || now - last_probe_sent_tick < 20) return;
    if (PostMessageW(window, kProbeMessage, static_cast<WPARAM>(now), 0)) {
        probe_pending = true;
        last_probe_sent_tick = now;
    }
}

bool WaitUntil(const std::function<bool()>& done, const char* message,
               const std::function<void()>& step = {}) {
    while (GetTickCount64() < deadline) {
        if (step) step();
        if (done()) return true;
        PumpOnce();
    }
    Check(false, message);
    return false;
}

void PumpFor(uint64_t milliseconds, const std::function<void()>& step = {}) {
    const uint64_t until = std::min(deadline, GetTickCount64() + milliseconds);
    while (GetTickCount64() < until) {
        if (step) step();
        PumpOnce();
    }
}

struct TempDirectory {
    spark::fs::path path = spark::fs::temp_directory_path() /
        Utf8ToWide("spark-session-load-" + NewSessionId());

    TempDirectory() { spark::fs::create_directories(path); }
    ~TempDirectory() { std::error_code error; spark::fs::remove_all(path, error); }
    std::string Utf8() const { return WideToUtf8(path.wstring()); }
};

struct JournalFixture {
    std::string path;
    std::string id;
    std::string workspace;
    std::vector<ChatMessage> messages;
};

JournalFixture MakeJournal(const std::string& root, const std::string& marker, size_t turns,
                           bool windows_lines = false) {
    JournalFixture fixture;
    fixture.id = NewSessionId();
    fixture.workspace = JoinPath(root, marker);
    fixture.path = JoinPath(JoinPath(root, "sessions"), fixture.id + ".jsonl");
    spark::fs::create_directories(Utf8ToWide(fixture.workspace));
    spark::fs::create_directories(Utf8ToWide(ParentPath(fixture.path)));
    std::string contents = windows_lines ? "\xef\xbb\xbf" : "";
    const auto event = [&](const char* type, const nlohmann::json& payload) {
        contents += nlohmann::json({{"type", type}, {"timestamp", uint64_t{1700000001}},
                                   {"payload", payload}}).dump();
        contents += windows_lines ? "\r\n" : "\n";
    };
    event("session.created", {{"session_id", fixture.id}, {"workspace", fixture.workspace},
                             {"created_at", uint64_t{1700000000}}, {"title", marker}});
    event("session.scope", {{"scope", "project"}});
    for (size_t turn = 0; turn < turns; ++turn) {
        ChatMessage user;
        user.role = MessageRole::User;
        user.content = marker + " question " + std::to_string(turn);
        user.timestamp = 1700000001 + turn;
        ChatMessage assistant;
        assistant.role = MessageRole::Assistant;
        assistant.content = marker + " answer " + std::to_string(turn) + "\n**Fixture** `code`.";
        assistant.timestamp = user.timestamp + 1;
        for (const auto& message : {user, assistant}) {
            fixture.messages.push_back(message);
            event("message", message);
        }
    }
    if (windows_lines) contents += "\r\n{\"type\":\"message\",\"payload\":";
    Check(contents.size() <= kMaxFixtureBytes, "journal fixture stays within one MiB");
    std::string error;
    Check(WriteFileUtf8Atomic(fixture.path, contents, &error), "temporary journal fixture is written");
    return fixture;
}

void TestJournalAndLoader(const JournalFixture& long_session, const JournalFixture& small_session) {
    SessionJournal journal;
    const auto resumed = journal.Resume(small_session.path);
    Check(resumed.ok && journal.State().id == small_session.id &&
              nlohmann::json(journal.State().messages) == nlohmann::json(small_session.messages) &&
              journal.State().workspace == small_session.workspace &&
              journal.State().metadata.value("scope", "") == "project",
          "BOM, CRLF, blank lines and damaged tail preserve valid messages and project scope");
    CancellationToken cancellation;
    cancellation.Cancel();
    SessionJournal cancelled;
    const auto cancelled_result = cancelled.Resume(long_session.path, &cancellation);
    Check(!cancelled_result.ok && cancelled_result.error == "cancelled" && cancelled.State().messages.empty(),
          "an already cancelled token prevents journal restoration");
    SessionLoader loader;
    const uint64_t first = loader.Request(long_session.path);
    const uint64_t latest = loader.Request(small_session.path);
    std::unique_ptr<SessionLoader::Loaded> loaded;
    if (WaitUntil([&] { loaded = loader.Poll(); return loaded != nullptr; }, "latest loader generation completes")) {
        Check(latest > first && loaded->generation == latest && loaded->path == small_session.path &&
                  loaded->error.empty() && loaded->journal.State().id == small_session.id &&
                  loaded->journal.State().metadata.value("scope", "") == "project",
              "rapid requests adopt only the latest generation and retain scope");
    }
    loader.Request(long_session.path);
    loader.Cancel();
    PumpFor(60, [&] { Check(loader.Poll() == nullptr, "cancelled loader generation never becomes adoptable"); });
    const uint64_t failure_generation = loader.Request(small_session.path + ".missing");
    if (WaitUntil([&] { loaded = loader.Poll(); return loaded != nullptr; }, "missing journal returns a result")) {
        Check(loaded->generation == failure_generation && !loaded->error.empty(),
              "loader reports a missing path rather than remaining pending");
    }
    loader.Request(long_session.path);
    loader.Stop();
    Check(loader.Poll() == nullptr, "stopping discards pending loader output");
}

struct PrintedClient {
    HDC screen = nullptr;
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    DWORD* pixels = nullptr;
    int width = 0;
    int height = 0;

    explicit PrintedClient(HWND window, UINT print_message = WM_PRINTCLIENT) {
        RECT client{};
        GetClientRect(window, &client);
        width = client.right;
        height = client.bottom;
        screen = GetDC(nullptr);
        dc = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* data = nullptr;
        bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &data, nullptr, 0);
        pixels = static_cast<DWORD*>(data);
        if (dc != nullptr && bitmap != nullptr) {
            previous = SelectObject(dc, bitmap);
            PatBlt(dc, 0, 0, width, height, BLACKNESS);
            SendMessageW(window, print_message, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_ERASEBKGND);
            GdiFlush();
        }
    }

    ~PrintedClient() {
        if (previous != nullptr) SelectObject(dc, previous);
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (dc != nullptr) DeleteDC(dc);
        if (screen != nullptr) ReleaseDC(nullptr, screen);
    }

    COLORREF Pixel(int x, int y) const {
        const DWORD value = pixels[y * width + x];
        return RGB((value >> 16) & 255, (value >> 8) & 255, value & 255);
    }

    void SaveOptional(const wchar_t* name) const {
        const wchar_t* output = _wgetenv(L"SPARK_SUMMARY_SNAPSHOT_DIR");
        if (output == nullptr || *output == 0 || bitmap == nullptr) return;
        spark::fs::create_directories(output);
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
            Check(false, "optional JPEG encoder starts");
            return;
        }
        {
            UINT count = 0;
            UINT bytes = 0;
            Gdiplus::GetImageEncodersSize(&count, &bytes);
            std::vector<BYTE> buffer(bytes);
            auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
            bool saved = false;
            if (bytes != 0 && Gdiplus::GetImageEncoders(count, bytes, encoders) == Gdiplus::Ok) {
                for (UINT index = 0; index < count; ++index) {
                    if (std::wstring(encoders[index].MimeType) != L"image/jpeg") continue;
                    Gdiplus::Bitmap image(bitmap, nullptr);
                    const auto path = spark::fs::path(output) / name;
                    saved = image.Save(path.c_str(), &encoders[index].Clsid, nullptr) == Gdiplus::Ok;
                    break;
                }
            }
            Check(saved, "optional loading snapshot saves as JPEG");
        }
        Gdiplus::GdiplusShutdown(token);
    }
};

}  // namespace

struct ChatViewLoadTest {
    static void CheckRestoreOrigin(const ChatView& view, uint64_t started) {
        Check(view.restore_started_tick_ == started,
              "file reading and transcript replay share one spinner delay origin");
    }

    static size_t Measured(const ChatView& view) {
        return static_cast<size_t>(std::count_if(view.item_heights_.begin(), view.item_heights_.end(),
                                                [](int height) { return height >= 0; }));
    }

    static void CheckCircle(ChatView& view, bool dark) {
        PrintedClient image(view.hwnd());
        Check(image.pixels != nullptr && image.dc != nullptr, "loading paint has a valid DIB target");
        if (image.pixels == nullptr || image.dc == nullptr) return;
        const COLORREF background = dark ? theme::DarkPalette().background : theme::LightPalette().background;
        const int center_x = image.width / 2;
        const int center_y = image.height / 2;
        RECT bounds{image.width, image.height, -1, -1};
        size_t changed = 0;
        for (int y = 0; y < image.height; ++y) {
            for (int x = 0; x < image.width; ++x) {
                if (image.Pixel(x, y) == background) continue;
                ++changed;
                bounds.left = std::min<LONG>(bounds.left, x);
                bounds.right = std::max<LONG>(bounds.right, x);
                bounds.top = std::min<LONG>(bounds.top, y);
                bounds.bottom = std::max<LONG>(bounds.bottom, y);
            }
        }
        const int span_x = bounds.right - bounds.left;
        const int span_y = bounds.bottom - bounds.top;
        Check(changed >= 16 && span_x >= view.Scale(8) && span_y >= view.Scale(8) &&
                  span_x <= view.Scale(80) && span_y <= view.Scale(80) &&
                  std::abs((bounds.left + bounds.right) / 2 - center_x) <= view.Scale(16) &&
                  std::abs((bounds.top + bounds.bottom) / 2 - center_y) <= view.Scale(16) &&
                  image.Pixel(center_x, center_y) == background && image.Pixel(0, 0) == background,
              "WM_PRINTCLIENT paints a centered hollow loading circle in the selected theme");
        image.SaveOptional(dark ? L"session-loading-dark.jpg" : L"session-loading-light.jpg");
    }

    static void CheckQuietLoading(ChatView& view, bool dark) {
        PrintedClient image(view.hwnd());
        const COLORREF background = dark ? theme::DarkPalette().background : theme::LightPalette().background;
        size_t changed = 0;
        if (image.pixels != nullptr) {
            for (int y = 0; y < image.height; ++y) {
                for (int x = 0; x < image.width; ++x) {
                    if (image.Pixel(x, y) != background) ++changed;
                }
            }
        }
        Check(image.pixels != nullptr && changed == 0,
              "short loading paints no circle or partially restored messages");
    }

    static void Run(HWND parent) {
        ChatView view;
        const auto font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        Check(view.Create(parent, GetModuleHandleW(nullptr), 900, font, font), "hidden ChatView is created");
        if (!IsWindow(view.hwnd())) return;
        MoveWindow(view.hwnd(), 0, 0, 640, 400, FALSE);
        for (const bool dark : {true, false}) {
            view.Clear();
            view.SetTheme(dark);
            view.BeginRestore();
            Check(view.Loading(), "BeginRestore enters loading without production model work");
            CheckQuietLoading(view, dark);
            const int frame = view.restore_frame_;
            SendMessageW(view.hwnd(), WM_TIMER, view.restore_spinner_timer_, 0);
            Check(view.restore_frame_ == frame, "loading animation stays idle before the delay");
            view.restore_started_tick_ = GetTickCount64() - 1000;
            SendMessageW(view.hwnd(), WM_TIMER, view.restore_spinner_timer_, 0);
            Check(view.restore_frame_ != frame, "slow loading advances the loading circle");
            CheckCircle(view, dark);
            view.Clear();
            Check(!view.Loading() && view.Count() == 0, "Clear stops an empty restoration");
            view.BeginRestore();
            view.AddUser("quick question");
            const size_t reply = view.AddAssistant("quick answer");
            view.FinishAssistant(reply);
            view.EndRestore();
            for (int tick = 0; tick < 4 && view.Loading(); ++tick) {
                SendMessageW(view.hwnd(), WM_TIMER, view.restore_prepare_timer_, 0);
            }
            Check(!view.Loading() && view.restore_frame_ == 0 && view.Item(reply)->text == "quick answer",
                  "fast restoration completes without ever animating a circle");
        }

        view.BeginRestore();
        for (size_t turn = 0; turn < kLongTurns && GetTickCount64() < deadline; ++turn) {
            view.AddUser("restored question " + std::to_string(turn));
            const size_t assistant = view.AddAssistant("**Restored answer** with `code` and a short paragraph.");
            view.FinishAssistant(assistant);
        }
        Check(view.Count() == kLongTurns * 2 && Measured(view) == 0,
              "long replay adds rows without synchronously measuring the whole transcript");
        view.EndRestore();
        Check(view.Loading() && Measured(view) < view.Count(), "EndRestore schedules incremental layout");
        bool partial = false;
        const size_t start_probes = probes;
        const size_t start_timers = chat_timers;
        const auto step = [&] {
            partial = partial || (Measured(view) > 0 && Measured(view) < view.Count());
            PostProbe(parent);
        };
        if (WaitUntil([&] { return !view.Loading(); }, "long ChatView layout completes within the suite deadline", step)) {
            Check(partial && Measured(view) == view.Count() && probes > start_probes && chat_timers > start_timers,
                  "layout is incremental and the main message pump processes probes and ChatView timers");
        }
        view.Clear();
        view.BeginRestore();
        for (int index = 0; index < 128; ++index) view.AddAssistant("obsolete layout");
        view.EndRestore();
        const UINT_PTR old_timer = view.restore_prepare_timer_;
        view.Clear();
        view.AddUser("fresh after clear");
        SendMessageW(view.hwnd(), WM_TIMER, old_timer, 0);
        PumpFor(30);
        Check(!view.Loading() && view.Count() == 1 && view.Item(0)->text == "fresh after clear",
              "Clear invalidates the old layout generation and ignores its queued timer");
    }
};

struct AppWindowSessionLoadTest {
    inline static std::vector<bool> composer_enabled;
    inline static size_t composer_moves = 0;
    inline static size_t chat_moves = 0;

    static LRESULT CALLBACK PaintProbe(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                      UINT_PTR subclass_id, DWORD_PTR) {
        if (message == WM_ENABLE && subclass_id == 91) composer_enabled.push_back(wparam != FALSE);
        if (message == WM_WINDOWPOSCHANGING) {
            if (subclass_id == 91) ++composer_moves;
            else ++chat_moves;
        }
        return DefSubclassProc(window, message, wparam, lparam);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* app = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<AppWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (message == kProbeMessage) {
            probe_pending = false;
            ++probes;
            max_probe_delay = std::max(max_probe_delay, GetTickCount64() - static_cast<uint64_t>(wparam));
            return 0;
        }
        // Never forward WM_CREATE: it starts normal application initialization.
        if (app != nullptr && message == WM_TIMER) {
            ++app_timers;
            return app->HandleMessage(message, wparam, lparam);
        }
        if (app != nullptr && (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT ||
                               message == WM_DRAWITEM || message == WM_PAINT || message == WM_PRINTCLIENT)) {
            return app->HandleMessage(message, wparam, lparam);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    struct Fixture {
        TempDirectory directory;
        AppWindow app;

        Fixture() {
            auto document = ParseConfigJson(DefaultConfigExampleJson());
            document["workspace"]["path"] = directory.Utf8();
            document["storage"]["data_dir"] = directory.Utf8();
            document["skills"]["enabled"] = false;
            document["ui"]["theme"] = "dark";
            document["providers"]["provider-a"]["base_url"] = "http://127.0.0.1:1";
            document["providers"]["provider-a"]["api_key"] = "";
            app.config_ = ConfigFromJson(document, JoinPath(directory.Utf8(), "fixture-config.json"));
            app.config_.skills.enabled = false;
            app.config_.mcp_servers.clear();
            app.instance_ = GetModuleHandleW(nullptr);
            app.configuration_initialized_ = true;
            app.active_workspace_path_ = app.config_.workspace.path;
            app.pending_new_conversation_ = true;
            app.journal_.SetStorageRoot(app.config_.storage.data_dir);
            app.window_ = CreateWindowExW(0, kTestClass, L"", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                         0, 0, 1100, 760, nullptr, nullptr, app.instance_, &app);
            Check(app.window_ != nullptr, "hidden application fixture window is created");
            if (app.window_ == nullptr) return;
            app.CreateControls();
            app.LayoutControls();
            Check(app.chat_view_ != nullptr && IsWindow(app.composer_), "hidden chat and composer controls exist");
        }

        ~Fixture() {
            app.ClearChatView();
            app.session_loader_.Stop();
            app.chat_view_.reset();
            app.side_chat_view_.reset();
            app.summary_popup_.reset();
            app.summary_panel_.reset();
            if (IsWindow(app.window_)) DestroyWindow(app.window_);
            app.window_ = nullptr;
            app.composer_ = nullptr;
            for (HFONT font : {app.ui_font_, app.chat_font_, app.mono_font_, app.brand_font_,
                               app.empty_mark_font_, app.empty_title_font_}) {
                if (font != nullptr) DeleteObject(font);
            }
            app.ui_font_ = app.chat_font_ = app.mono_font_ = app.brand_font_ = nullptr;
            app.empty_mark_font_ = app.empty_title_font_ = nullptr;
        }
    };

    static bool Loaded(AppWindow& app, const JournalFixture& fixture) {
        return !app.SessionLoading() && !app.chat_view_->Loading() && app.journal_.State().id == fixture.id;
    }

    static void CheckTranscript(AppWindow& app, const JournalFixture& fixture) {
        std::vector<ChatMessage> visible;
        for (size_t index = 0; index < app.chat_view_->Count(); ++index) {
            const auto* item = app.chat_view_->Item(index);
            if (item->kind != ChatItemKind::User && item->kind != ChatItemKind::Assistant) continue;
            ChatMessage message;
            message.role = item->kind == ChatItemKind::User ? MessageRole::User : MessageRole::Assistant;
            message.content = item->text;
            message.timestamp = item->timestamp;
            visible.push_back(std::move(message));
        }
        Check(nlohmann::json(visible) == nlohmann::json(fixture.messages),
              "restored transcript contains every selected message in order, without stale-session rows");
    }

    static void CheckComposerPaint(AppWindow& app, bool enabled) {
        const auto palette = theme::For(app.config_.ui.theme);
        HDC device = GetDC(app.composer_);
        const auto brush = reinterpret_cast<HBRUSH>(SendMessageW(app.window_,
            enabled ? WM_CTLCOLOREDIT : WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(device),
            reinterpret_cast<LPARAM>(app.composer_)));
        LOGBRUSH color{};
        Check(GetObjectW(brush, sizeof(color), &color) != 0 && color.lbColor == palette.panel &&
                  GetBkColor(device) == palette.panel,
              "enabled and loading composer keep the same panel background in either theme");
        ReleaseDC(app.composer_, device);
        PrintedClient image(app.composer_, WM_PRINT);
        Check(image.pixels != nullptr && image.width > 16 && image.height > 16 &&
                  image.Pixel(image.width - 8, image.height - 8) == palette.panel,
              "native edit paint never exposes the chat background while loading");
    }

    static void TestSwitchPainting(AppWindow& app) {
        SetWindowSubclass(app.composer_, PaintProbe, 91, 0);
        SetWindowSubclass(app.chat_view_->hwnd(), PaintProbe, 92, 0);
        composer_moves = chat_moves = 0;
        app.LayoutControls();
        app.LayoutControls();
        Check(composer_moves == 0 && chat_moves == 0,
              "unchanged layout does not move or repaint the composer and transcript");
        for (const char* preference : {"dark", "light"}) {
            app.config_.ui.theme = preference;
            app.ApplyTheme();
            CheckComposerPaint(app, true);
            EnableWindow(app.composer_, FALSE);
            CheckComposerPaint(app, false);
            EnableWindow(app.composer_, TRUE);
        }
        app.config_.ui.theme = "dark";
        app.ApplyTheme();
        app.attachments_ = {JoinPath(app.config_.storage.data_dir, "preview.txt")};
        app.LayoutControls();
        {
            PrintedClient image(app.window_);
            const auto metrics = app.MeasureLayout();
            Check(image.pixels != nullptr &&
                      image.Pixel(metrics.panel_left + 8, metrics.panel_bottom - 8) == theme::DarkPalette().panel &&
                      image.Pixel(metrics.main_x + 4, metrics.topbar_height + 4) == theme::DarkPalette().background,
                  "buffered window paint preserves the composer panel and chat background");
        }
        const size_t hit_count = app.attachment_hit_areas_.size();
        app.LayoutControls();
        Check(hit_count == 1 && app.attachment_hit_areas_.size() == hit_count,
              "unchanged layout retains the attachment preview click target");
        // Warm the shared GDI caches before checking for growth across repeated paints.
        for (int frame = 0; frame < 2; ++frame) { PrintedClient image(app.window_); }
        GdiFlush();
        const DWORD resources_before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        for (int frame = 0; frame < 6; ++frame) { PrintedClient image(app.window_); }
        GdiFlush();
        const DWORD resources_after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        if (resources_after > resources_before) {
            std::cerr << "GDI resources: " << resources_before << " -> " << resources_after << '\n';
        }
        Check(resources_after <= resources_before,
              "buffered switching paint releases its temporary GDI resources");
        app.attachments_.clear();
        app.LayoutControls();
        composer_enabled.clear();
    }

    static void RunIntegration(Fixture& fixture, const JournalFixture& a, const JournalFixture& b) {
        auto& app = fixture.app;
        app.projects_.push_back({"Project A", a.workspace});
        app.projects_.push_back({"Project B", b.workspace});
        AppWindow::SidebarRow a_row;
        a_row.kind = AppWindow::SidebarRowKind::Chat;
        a_row.session_path = a.path;
        a_row.label = L"A";
        AppWindow::SidebarRow b_row = a_row;
        b_row.session_path = b.path;
        b_row.label = L"B";
        app.project_rows_ = {a_row, b_row};
        SetWindowTextW(app.composer_, L"pending no-project draft");
        app.SaveComposerState();
        const std::string pending_draft = app.ComposerStatePath();
        const std::string b_draft = JoinPath(JoinPath(app.config_.storage.data_dir, "drafts"), b.id + ".json");
        Check(WriteFileUtf8Atomic(b_draft, R"({"draft":"B draft","goal":"B goal","plan_mode":true})", nullptr),
              "selected conversation has its own draft fixture");
        app.ResumeSessionPath(a.path);
        wchar_t title[256]{};
        GetWindowTextW(app.main_title_label_, title, static_cast<int>(std::size(title)));
        Check(std::wstring(title) == L"A", "loading retains the selected sidebar conversation title");
        Check(app.SessionLoading() && app.session_load_path_ == a.path && app.chat_view_->Loading() &&
                  !IsWindowEnabled(app.composer_), "selection starts asynchronous read with a disabled composer");
        app.ResumeSessionPath(b.path);
        GetWindowTextW(app.main_title_label_, title, static_cast<int>(std::size(title)));
        Check(std::wstring(title) == L"B", "rapid reselection uses the latest conversation title immediately");
        if (!WaitUntil([&] { return Loaded(app, b); }, "A then B selects only B")) return;
        Check(composer_enabled == std::vector<bool>{false, true},
              "rapid conversation switching disables once and reenables only after the latest load completes");
        CheckTranscript(app, b);
        Check(app.active_session_scope_ == "project" && app.active_project_name_ == "Project B" &&
                  app.active_workspace_path_ == b.workspace && app.ComposerText() == "B draft" &&
                  app.goal_ == "B goal" && app.plan_mode_, "latest selection retains registered project scope and B draft");
        SetWindowTextW(app.composer_, L"edited B draft");
        app.ResumeSessionPath(a.path);
        app.ResumeSessionPath(b.path);
        if (!WaitUntil([&] { return Loaded(app, b); }, "rapid reselection restores the saved B draft")) return;
        Check(app.ComposerText() == "edited B draft", "loading cannot overwrite the previous conversation draft");

        app.ResumeSessionPath(b.path + ".missing");
        if (!WaitUntil([&] { return !app.SessionLoading(); }, "failed selection leaves loading state")) return;
        Check(!app.chat_view_->Loading() && IsWindowEnabled(app.composer_) &&
                  app.journal_.State().id == b.id && app.ComposerText() == "edited B draft",
              "failed path restores the selected transcript and draft without a stuck spinner");
        CheckTranscript(app, b);

        app.ResumeSessionPath(a.path);
        app.BeginNewSession();
        PumpFor(80, [&] { app.PollSessionLoad(); });
        Check(app.pending_new_conversation_ && app.active_session_scope_ == "recent" &&
                  app.active_project_name_.empty() && app.active_workspace_path_ == app.config_.workspace.path &&
                  app.session_load_path_.empty() && !app.SessionLoading() && !app.chat_view_->Loading() &&
                  app.chat_view_->Count() == 0 && app.ComposerStatePath() == pending_draft &&
                  app.ComposerText() == "pending no-project draft",
              "new chat cancels the read and retains an independent no-project pending draft");

        app.ResumeSessionPath(a.path);
        app.ClearChatView();
        PumpFor(60, [&] { app.PollSessionLoad(); });
        Check(!app.SessionLoading() && app.session_load_path_.empty() && app.chat_view_->Count() == 0,
              "ClearChatView prevents a cancelled read from restoring old messages");
        app.ResumeSessionPath(a.path);
        const uint64_t load_started = app.session_load_started_tick_;
        const size_t timers_before = app_timers;
        if (!WaitUntil([&] { return app.journal_.State().id == a.id && app.chat_view_->Count() > 0; },
                       "long session is adopted for incremental replay", [&] { PostProbe(app.window_); })) return;
        Check(app.SessionLoading() && app.chat_view_->Count() < a.messages.size(),
              "adoption yields after a bounded replay step");
        ChatViewLoadTest::CheckRestoreOrigin(*app.chat_view_, load_started);
        auto runtime = std::make_shared<AppWindow::TurnRuntime>();
        runtime->path = a.path;
        runtime->session_key = NormalizePathForComparison(a.path);
        runtime->workspace = a.workspace;
        runtime->config = app.config_;
        Check(runtime->journal.Resume(a.path).ok, "threadless runtime uses only the fixture journal");
        app.active_turns_[runtime->session_key] = runtime;
        ChatMessage final;
        final.role = MessageRole::Assistant;
        final.content = "final persisted completion";
        final.timestamp = 1700009999;
        runtime->journal.MutableState().messages.push_back(final);
        Check(runtime->journal.Append("message", final).ok, "completion is persisted while replay is active");
        runtime->complete.store(true);
        runtime->success.store(true);
        app.FinishTurn(runtime, true, "");
        if (!WaitUntil([&] { return Loaded(app, a); }, "completion during replay refreshes the persisted journal",
                       [&] { PostProbe(app.window_); })) return;
        const auto* last = app.chat_view_->Count() == 0 ? nullptr :
            app.chat_view_->Item(app.chat_view_->Count() - 1);
        Check(app.journal_.State().messages.back().content == final.content &&
                  last != nullptr && last->text == final.content &&
                  app_timers > timers_before && max_probe_delay < 500,
              "completion rereads final output, removes the spinner and keeps the pump responsive");
        app.QueueSessionLoad(a.path);
        if (!WaitUntil([&] { return app.journal_.State().id == a.id &&
                                    app.session_load_path_.empty() && app.chat_view_->Count() > 0; },
                       "forced refresh begins a new replay")) return;
        app.ClearChatView();
        app.chat_view_->AddUser("fresh view");
        PumpFor(60, [&] { app.PollSessionLoad(); });
        Check(!app.SessionLoading() && !app.chat_view_->Loading() && app.chat_view_->Count() == 1 &&
                  app.chat_view_->Item(0)->text == "fresh view", "Clear cancels adopted replay and old layout timers");

        auto live = std::make_shared<AppWindow::TurnRuntime>();
        live->path = b.path;
        live->session_key = NormalizePathForComparison(b.path);
        live->started_tick = GetTickCount64();
        app.active_turns_[live->session_key] = live;
        app.QueueSessionLoad(b.path);
        auto* delta = new AppWindow::UiEvent(AppWindow::EventKind::TextDelta, "live output during file read");
        delta->session_key = live->session_key;
        app.HandleUiEvent(delta);
        Check(app.session_deferred_events_.size() == 1 && app.chat_view_->Count() == 0,
              "live output is buffered during file reading without entering the loading view");
        if (WaitUntil([&] { return Loaded(app, b); }, "live session restores and drains buffered output")) {
            const auto* reply = app.chat_view_->Item(app.chat_view_->Count() - 1);
            Check(live->ui_attached && app.session_deferred_events_.empty() && reply != nullptr &&
                      reply->text == "live output during file read",
                  "buffered streaming output survives journal adoption and renders after layout");
        }
        app.active_turns_.erase(live->session_key);
        app.ClearChatView();
    }

    static int Run() {
        deadline = GetTickCount64() + 12000;
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        WNDCLASSW window_class{};
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpfnWndProc = WindowProc;
        window_class.lpszClassName = kTestClass;
        if (!RegisterClassW(&window_class)) return 1;
        try {
            Fixture fixture;
            if (fixture.app.chat_view_ != nullptr && IsWindow(fixture.app.composer_)) {
                const auto a = MakeJournal(fixture.directory.Utf8(), "A", kLongTurns);
                const auto b = MakeJournal(fixture.directory.Utf8(), "B", 2, true);
                TestJournalAndLoader(a, b);
                ChatViewLoadTest::Run(fixture.app.window_);
                TestSwitchPainting(fixture.app);
                RunIntegration(fixture, a, b);
            }
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "FAIL: session-loading fixture exception: " << exception.what() << '\n';
        }
        Check(GetTickCount64() <= deadline, "session-loading suite finishes within twelve seconds");
        UnregisterClassW(kTestClass, window_class.hInstance);
        if (failures == 0) std::cout << "Session loading checks passed.\n";
        return failures == 0 ? 0 : 1;
    }
};

}  // namespace spark

int main() { return spark::AppWindowSessionLoadTest::Run(); }
