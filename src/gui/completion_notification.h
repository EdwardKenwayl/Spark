#pragma once

#include "gui/completion_notice.h"
#include "gui/theme.h"

#include <functional>
#include <memory>
#include <vector>

namespace spark {

inline constexpr wchar_t kCompletionNotificationClass[] = L"SparkCompletionNotification";

// UI-thread-only, bounded transient cards. No worker, animation or idle polling.
class CompletionNotifications {
public:
    using Activate = std::function<void(const std::string&)>;
    CompletionNotifications();
    ~CompletionNotifications();
    CompletionNotifications(const CompletionNotifications&) = delete;
    CompletionNotifications& operator=(const CompletionNotifications&) = delete;

    bool Show(HWND owner, CompletionNotice notice, theme::Palette palette, Activate activate);
    void Close();
    static bool IsBackground(HWND owner);

private:
    struct Card;
    std::vector<std::unique_ptr<Card>> cards_;
};

}  // namespace spark
