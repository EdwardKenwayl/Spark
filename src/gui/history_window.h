#pragma once

#include "core/session.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace spark {

struct HistoryWindow {
    size_t first_message = 0, first_turn = 0, total_turns = 0;
};

// Select earlier user boundaries, retaining every message from the result to the journal tail.
inline HistoryWindow SelectHistoryWindow(const std::vector<ChatMessage>& messages,
                                         size_t before_message = SIZE_MAX, size_t turn_limit = 8) {
    HistoryWindow window;
    const auto before = before_message < messages.size() ? before_message : messages.size();
    size_t selected = 0;
    if (turn_limit == 0) turn_limit = 1;
    for (size_t index = before; index > 0;) {
        --index;
        if (messages[index].role == MessageRole::User) {
            window.first_message = index;
            if (++selected == turn_limit) break;
        }
    }
    for (size_t index = 0; index < messages.size(); ++index) {
        if (messages[index].role != MessageRole::User) continue;
        if (selected == 0 && window.total_turns == 0) window.first_message = index;
        ++window.total_turns;
        if (index < window.first_message) ++window.first_turn;
    }
    return window;
}

}  // namespace spark
