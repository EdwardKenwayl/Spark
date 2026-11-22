#pragma once

#include "core/session.h"

#include <cstddef>
#include <cstdint>
#include <algorithm>
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

struct HistoryPage {
    size_t first_turn = 0, end_turn = 0;
    bool Contains(size_t turn) const { return first_turn <= turn && turn < end_turn; }
};

inline HistoryPage HistoryPageAround(size_t turn, size_t live_first) {
    constexpr size_t page_size = 10;
    const size_t first = std::min(turn - std::min<size_t>(turn, 4),
                                  live_first - std::min(live_first, page_size));
    return {first, std::min(first + page_size, live_first)};
}

struct HistoryRailSlots {
    size_t first = 0, count = 0;
    int top = 0;
};

inline HistoryRailSlots HistoryRailLayout(size_t total, size_t skip_latest, int viewport, int margin, int pitch) {
    if (viewport < 2 * margin || pitch <= 0) return {};
    const size_t count = std::min(total, static_cast<size_t>((viewport - 2 * margin) / pitch + 1));
    if (!count) return {};
    return {total - count - std::min(skip_latest, total - count), count,
            (viewport - static_cast<int>(count - 1) * pitch) / 2};
}

class HistoryTurnIndex {
public:
    void Update(const std::vector<ChatMessage>& messages) {
        if (scanned_ > messages.size()) Clear();
        for (; scanned_ < messages.size(); ++scanned_)
            if (messages[scanned_].role == MessageRole::User) turns_.push_back(scanned_);
    }
    void Clear() { scanned_ = 0; turns_.clear(); }
    size_t Count() const { return turns_.size(); }
    size_t Message(size_t turn) const { return turn < Count() ? turns_[turn] : scanned_; }
    size_t Turn(size_t message) const {
        return static_cast<size_t>(std::lower_bound(turns_.begin(), turns_.end(), message) - turns_.begin());
    }

private:
    size_t scanned_ = 0;
    std::vector<size_t> turns_;
};

}  // namespace spark
