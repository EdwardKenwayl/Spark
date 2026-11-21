#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>
#include "gui/chat_items.h"

namespace spark {

// Turn ranges are half-open: a previous turn ending at the viewport edge is
// not visible. Use the full turn, not just the user bubble at its beginning.
inline bool ConversationTurnVisible(int start, int end, int scroll, int viewport) {
    return viewport > 0 && start < static_cast<int64_t>(scroll) + viewport && end > scroll;
}

inline bool TimelineLocalTime(uint64_t timestamp, std::tm& result) {
    if (timestamp == 0) return false;
    const auto value = static_cast<std::time_t>(timestamp);
#ifdef _WIN32
    return localtime_s(&result, &value) == 0;
#else
    return localtime_r(&value, &result) != nullptr;
#endif
}

// A persisted timestamp, never a window-open time. A new group starts on the
// first message, after 30 minutes of inactivity, or on a new local calendar day.
inline bool StartsTimelineGroup(uint64_t previous, uint64_t current) {
    if (current == 0) return false;
    if (previous == 0) return true;
    if (current <= previous) return false;
    if (current - previous >= 30 * 60) return true;
    std::tm before{}, after{};
    return TimelineLocalTime(previous, before) && TimelineLocalTime(current, after) &&
           (before.tm_year != after.tm_year || before.tm_yday != after.tm_yday);
}

inline uint64_t PreviousTimelineTime(const std::vector<ChatItem>& items) {
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        if ((it->kind == ChatItemKind::User || it->kind == ChatItemKind::Assistant ||
             it->kind == ChatItemKind::Tool) && it->timestamp != 0) return it->timestamp;
    }
    return 0;
}

inline std::wstring FormatTimelineTime(uint64_t timestamp) {
    std::tm local{};
    if (!TimelineLocalTime(timestamp, local)) return {};
    wchar_t buffer[32]{};
    std::wcsftime(buffer, 32, L"%Y-%m-%d %H:%M", &local);
    return buffer;
}

}  // namespace spark
