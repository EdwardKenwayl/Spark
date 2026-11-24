#pragma once

#include "gui/history_window.h"
#include "gui/transcript.h"
#include "core/common.h"

#include <functional>

namespace spark {

// A read-only historical page. The active transcript keeps receiving live events
// with its original item indices while the page is displayed.
class HistoryBrowser {
public:
    using Source = std::function<const SessionState*()>;
    explicit HistoryBrowser(Source source) : source_(std::move(source)) {}

    void Reset() {
        index_.Clear();
        page = {};
        skip_latest = 0;
        transcript.Clear();
        Hover(kNoChatItem, 0);
    }
    void Update() {
        if (const auto* state = source_()) index_.Update(state->messages);
    }
    size_t Count() const { return index_.Count(); }
    size_t Turn(size_t message) const { return index_.Turn(message); }
    void Reveal(size_t turn, size_t visible_slots) {
        if (turn >= Count() || !visible_slots) return;
        const auto first = Count() - std::min(Count(), visible_slots + skip_latest);
        if (turn >= first && turn - first < visible_slots) return;
        const auto after = Count() - turn - 1;
        skip_latest = after - std::min(after, visible_slots / 2);
    }
    void Open(size_t turn, size_t live_first, TranscriptTime now) {
        const auto* state = source_();
        if (!state || turn >= live_first || live_first > Count()) return;
        page = HistoryPageAround(turn, live_first);
        target = turn;
        generation_ = transcript.BeginHistoryReplay(*state, now, index_.Message(page.first_turn),
            index_.Message(page.end_turn), page.first_turn, page.end_turn);
    }
    bool Step() { return transcript.ReplayStep(generation_); }
    void Hover(size_t turn, uint64_t now) {
        if (hover_ == turn) return;
        hover_ = turn;
        hover_at_ = now;
        preview_ready_ = false;
        prompt.clear();
        reply.clear();
    }
    bool PreviewUnavailable() const {
        constexpr size_t preview_horizon = 256;
        return hover_ >= Count() || Count() - hover_ > preview_horizon;
    }
    bool PreviewReady() const { return preview_ready_; }
    bool UpdatePreview(uint64_t now) {
        if (hover_ == kNoChatItem || preview_ready_ || PreviewUnavailable() || now - hover_at_ < 150) return false;
        const auto* state = source_();
        if (!state) return false;
        const auto first = index_.Message(hover_);
        const auto end = std::min(index_.Message(hover_ + 1), state->messages.size());
        if (first >= end) return false;
        const auto& user = state->messages[first];
        prompt = TruncateUtf8(user.content.substr(0, 224), 220);
        if (prompt.empty() && !user.image_paths.empty()) prompt = "[Image]";
        for (size_t i = end; i > first + 1;) {
            const auto& message = state->messages[--i];
            if (message.role == MessageRole::Assistant && HasAssistantText(message.content) && message.tool_calls.empty()) {
                reply = TruncateUtf8(message.content.substr(0, 304), 300);
                break;
            }
        }
        preview_ready_ = true;
        return true;
    }

    HistoryPage page;
    Transcript transcript;
    size_t target = 0;
    size_t skip_latest = 0;
    std::string prompt, reply;

private:
    Source source_;
    HistoryTurnIndex index_;
    size_t hover_ = kNoChatItem;
    uint64_t hover_at_ = 0, generation_ = 0;
    bool preview_ready_ = false;
};

}  // namespace spark
