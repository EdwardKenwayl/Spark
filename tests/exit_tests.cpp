#include "gui/app.h"

#include <chrono>
#include "platform/filesystem.h"
#include <iostream>

namespace spark {

struct AppWindowExitTest {
    static constexpr UINT kTrayMessage = WM_APP + 4;
    static constexpr WPARAM kTrayIconId = 1;
    static constexpr UINT kTrayExit = 1801;
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
        if (app != nullptr && (message == WM_COMMAND || message == WM_CLOSE || message == WM_TIMER ||
                               message == kTrayMessage)) {
            return app->HandleMessage(message, wparam, lparam);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    struct Fixture {
        spark::fs::path directory;
        AppWindow app;

        Fixture() {
            directory = spark::fs::temp_directory_path() / Utf8ToWide("spark-exit-" + NewSessionId());
            spark::fs::create_directories(directory);
            app.config_.storage.data_dir = WideToUtf8(directory.wstring());
            app.config_.workspace.path = app.config_.storage.data_dir;
            app.journal_.SetStorageRoot(app.config_.storage.data_dir);
            app.pending_new_conversation_ = true;
            // Exercise native message dispatch without starting the application,
            // loading user configuration, showing a window, or adding a tray icon.
            app.window_ = CreateWindowExW(0, L"SparkExitTest", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                          GetModuleHandleW(nullptr), &app);
            app.composer_ = CreateWindowExW(0, L"EDIT", L"unsent draft", WS_CHILD, 0, 0, 0, 0,
                                            app.window_, nullptr, GetModuleHandleW(nullptr), nullptr);
            Check(app.window_ != nullptr && app.composer_ != nullptr, "hidden test controls were created");
        }

        ~Fixture() {
            if (IsWindow(app.window_)) DestroyWindow(app.window_);
            app.CancelAndJoinAllTurns();
            app.window_ = nullptr;
            app.composer_ = nullptr;
            std::error_code error;
            spark::fs::remove_all(directory, error);
        }
    };

    static void TestIdleExit() {
        Fixture fixture;
        auto& app = fixture.app;
        app.external_running_sessions_.insert("external-mcp-session");
        const std::string draft_path = app.ComposerStatePath();
        SendMessageW(app.window_, WM_COMMAND, kTrayExit, 0);
        Check(!IsWindow(app.window_), "external MCP activity does not block idle tray exit");
        const auto draft = ReadFileUtf8(draft_path);
        Check(draft.has_value() && nlohmann::json::parse(*draft).value("draft", "") == "unsent draft",
              "tray exit immediately saves the pending draft");
    }

    static void TestRunningExit() {
        Fixture fixture;
        auto& app = fixture.app;
        app.external_running_sessions_.insert("external-mcp-session");
        for (int index = 0; index < 2; ++index) {
            auto runtime = std::make_shared<AppWindow::TurnRuntime>();
            runtime->session_key = "local-turn-" + std::to_string(index);
            runtime->worker = std::thread([runtime] {
                while (!runtime->cancellation.IsCancelled()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                runtime->cancelled.store(true);
                runtime->complete.store(true);
            });
            app.active_turns_.emplace(runtime->session_key, runtime);
        }
        SendMessageW(app.window_, WM_COMMAND, kTrayExit, 0);
        Check(app.close_pending_ && app.active_turns_.size() == 2, "exit keeps workers alive until completion");
        for (const auto& entry : app.active_turns_) {
            Check(entry.second->cancellation.IsCancelled(), "exit cancels every local turn");
        }
        const uint64_t deadline = GetTickCount64() + 2500;
        while (IsWindow(app.window_) && GetTickCount64() < deadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Check(!IsWindow(app.window_), "exit finishes local workers even while external MCP activity remains");
        Check(app.active_turns_.empty(), "exit joins and removes every completed local worker");
    }

    static void TestWindowClose() {
        Fixture fixture;
        auto& app = fixture.app;
        app.tray_icon_added_ = true;
        SendMessageW(app.window_, WM_CLOSE, 0, 0);
        Check(IsWindow(app.window_) && !app.exiting_, "window X keeps the application alive in the tray");
    }

    static void TestTraySingleClickRestoresWindow() {
        Fixture fixture;
        auto& app = fixture.app;
        app.tray_icon_added_ = true;
        SendMessageW(app.window_, kTrayMessage, kTrayIconId, WM_LBUTTONUP);
        Check((GetWindowLongPtrW(app.window_, GWL_STYLE) & WS_VISIBLE) != 0,
              "a single left click on the tray icon restores the window");
    }

    static int Run() {
        WNDCLASSW window_class{};
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpfnWndProc = WindowProc;
        window_class.lpszClassName = L"SparkExitTest";
        if (!RegisterClassW(&window_class)) return 1;
        TestIdleExit();
        TestRunningExit();
        TestWindowClose();
        TestTraySingleClickRestoresWindow();
        UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
        if (failures == 0) std::cout << "Tray exit checks passed.\n";
        return failures == 0 ? 0 : 1;
    }
};

}  // namespace spark

int main() { return spark::AppWindowExitTest::Run(); }
