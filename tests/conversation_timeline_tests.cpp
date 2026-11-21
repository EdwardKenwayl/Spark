#include "gui/chat_view.h"
#include "gui/transcript.h"
#include "core/common.h"

#include "platform/gdiplus_headers.h"

#include <iostream>
#include <type_traits>

namespace spark {
namespace {
int failures = 0;
void Check(bool value, const char* label) {
    std::cout << label << ": " << (value ? "PASS" : "FAIL") << '\n';
    if (!value) ++failures;
}
template <class T, class = void> struct TimelineFields {
    static bool Images(const T&) { return false; }
    static bool Separator(const T&) { return false; }
};
template <class T> struct TimelineFields<T, std::void_t<decltype(T::image_paths), decltype(T::timeline_separator)>> {
    static bool Images(const T& item) { return item.image_paths == std::vector<std::string>{"fixture.png"}; }
    static bool Separator(const T& item) { return item.timeline_separator; }
};
template <class T>
auto AddImageUser(T& view, const std::string& path, int)
    -> decltype(view.AddUser("image above this text", 1700000000, std::vector<std::string>{path})) {
    return view.AddUser("image above this text", 1700000000, {path});
}
template <class T> size_t AddImageUser(T& view, const std::string&, long) {
    return view.AddUser("image above this text", 1700000000);
}
}

struct ChatViewLoadTest {
    template <class T>
    static auto PollingSettled(const T& view, int) -> decltype(view.image_cache_.Pending(), bool{}) {
        return !view.image_cache_.Pending();
    }
    template <class T> static bool PollingSettled(const T&, long) { return false; }

    static void CheckSentImage() {
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        Check(Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok, "image fixture starts decoder");
        wchar_t temporary[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temporary);
        const std::wstring path = std::wstring(temporary) + L"spark-timeline-" + Utf8ToWide(NewSessionId()) + L".bmp";
        {
            const CLSID bmp{0x557cf400, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
            Gdiplus::Bitmap image(32, 16, PixelFormat32bppARGB);
            Gdiplus::Graphics graphics(&image);
            graphics.Clear(Gdiplus::Color(255, 250, 0, 200));
            Check(image.Save(path.c_str(), &bmp) == Gdiplus::Ok, "image fixture creates local bitmap");
        }
        HWND parent = CreateWindowExW(0, L"STATIC", L"fixture", WS_POPUP, 0, 0, 640, 480,
                                      nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        {
            ChatView view;
            auto font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            Check(view.Create(parent, GetModuleHandleW(nullptr), 99, font, font), "image fixture creates real chat view");
            MoveWindow(view.hwnd(), 0, 0, 640, 480, TRUE);
            AddImageUser(view, WideToUtf8(path), 0);
            HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
            HBITMAP bitmap = CreateCompatibleBitmap(screen, 640, 480);
            auto old = SelectObject(dc, bitmap);
            // Streaming can repaint faster than the thumbnail polling interval.
            // Paint must not reset that timer's deadline on each frame.
            const DWORD repaint_start = GetTickCount();
            while (GetTickCount() - repaint_start < 400) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                SendMessageW(view.hwnd(), WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
                Sleep(5);
            }
            Check(PollingSettled(view, 0), "continuous repaint does not starve thumbnail polling");
            bool painted = false;
            const DWORD start = GetTickCount();
            while (GetTickCount() - start < 2000 && !painted) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                SendMessageW(view.hwnd(), WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
                for (int y = 0; y < 480 && !painted; y += 4)
                    for (int x = 0; x < 640; x += 4)
                        if (GetPixel(dc, x, y) == RGB(250, 0, 200)) { painted = true; break; }
                if (!painted) Sleep(10);
            }
            Check(painted, "sent image decoded and painted above user text");
            wchar_t capture[MAX_PATH]{};
            if (GetEnvironmentVariableW(L"SPARK_UI_CAPTURE", capture, MAX_PATH)) {
                const CLSID jpeg{0x557cf401, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
                Gdiplus::Bitmap screenshot(bitmap, nullptr);
                Check(screenshot.Save(capture, &jpeg) == Gdiplus::Ok, "fixture screenshot saved");
            }
            SelectObject(dc, old);
            DeleteObject(bitmap);
            DeleteDC(dc);
            ReleaseDC(nullptr, screen);
        }
        DestroyWindow(parent);
        DeleteFileW(path.c_str());
        Gdiplus::GdiplusShutdown(token);
    }
    static int Run() {
        // Paint the real rail, not a duplicate of its visibility calculation.
        ChatView view;
        view.rail_dashes_ = {{0, 80, 0}, {1, 100, 280}};
        view.content_height_ = 340;
        RECT client{0, 0, 400, 200};
        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, 400, 200);
        auto old = SelectObject(dc, bitmap);
        const auto active = [&](int y) { return GetPixel(dc, 17, y) != RGB(0, 0, 0); };
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        view.scroll_y_ = 140;
        view.DrawScrollRail(dc, client);
        Check(active(80) && active(100), "two visible turns highlighted at bottom");
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        view.scroll_y_ = 280;
        view.DrawScrollRail(dc, client);
        Check(!active(80) && active(100), "offscreen previous turn not highlighted");
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);

        SessionState state;
        ChatMessage user;
        user.role = MessageRole::User;
        user.content = "look at the picture";
        user.timestamp = 1700000000;
        user.image_paths = {"fixture.png"};
        state.messages.push_back(user);
        ChatMessage reply;
        reply.role = MessageRole::Assistant;
        reply.content = "answer";
        reply.timestamp = user.timestamp + 10;
        state.messages.push_back(reply);
        user.image_paths.clear();
        user.timestamp += 300;
        state.messages.push_back(user);
        user.timestamp += 3600;
        state.messages.push_back(user);
        Transcript transcript;
        const auto generation = transcript.BeginReplay(state, {1800000000, 100});
        while (!transcript.ReplayStep(generation)) {}
        std::vector<const ChatItem*> users;
        for (const auto& item : transcript.Items()) if (item.kind == ChatItemKind::User) users.push_back(&item);
        Check(users.size() == 3 && TimelineFields<ChatItem>::Images(*users[0]), "history retains sent image paths");
        Check(users.size() == 3 && TimelineFields<ChatItem>::Separator(*users[0]) &&
              !TimelineFields<ChatItem>::Separator(*users[1]) && TimelineFields<ChatItem>::Separator(*users[2]),
              "persisted first and idle-gap timestamps form timeline groups");
        Check(users.size() == 3 && users[0]->timestamp == 1700000000, "replay does not replace sent timestamp with now");
        CheckSentImage();
        return failures == 0 ? 0 : 1;
    }
};
}  // namespace spark

int main() { return spark::ChatViewLoadTest::Run(); }
