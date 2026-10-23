#pragma once

#include "core/session.h"
#include "gui/chat_items.h"

#include <cstddef>
#include <memory>

namespace spark {

inline constexpr size_t kNoChatItem = static_cast<size_t>(-1);
struct TranscriptTime { uint64_t timestamp = 0; uint64_t tick_ms = 0; };
struct TranscriptRun { bool active = false; uint64_t started_tick_ms = 0; };
enum class TranscriptEventKind { Add, AppendText, Finish, Hide, FinishTurn, PauseTurn, ResumeTurn };
struct TranscriptEvent {
    TranscriptEventKind kind = TranscriptEventKind::Add;
    size_t index = kNoChatItem;
    // Add: complete row; AppendText: text; Finish: result/success/elapsed_ms/timestamp.
    ChatItem item;
};
struct TranscriptGroup {
    bool leader = false;
    size_t size = 0, last_member = 0, finished = 0;
    size_t read_count = 0, search_count = 0, command_count = 0;
    uint64_t elapsed_ms = 0;
    bool hidden = false, indented = false, active = false, failed = false;
};

// No windows, widgets, timers, or journal writes. The adapter owns its clock and render scheduling.
class Transcript {
public:
    Transcript();
    ~Transcript();
    size_t Apply(const TranscriptEvent& event, TranscriptTime now);
    void Clear();
    void EraseFrom(size_t index);
    void ToggleTool(size_t index);
    void ToggleToolGroup(size_t index);
    void ToggleTurnStatus(size_t index);
    const std::vector<ChatItem>& Items() const { return items_; }
    size_t Count() const { return items_.size(); }
    uint64_t Generation() const { return generation_; }
    const ChatItem* Item(size_t index) const;
    size_t LastUserIndex() const;
    std::vector<TranscriptGroup> Groups(uint64_t now_tick_ms) const;
    bool NeedsAnimation() const;
    static uint64_t Elapsed(const ChatItem& item, uint64_t now_tick_ms);

    // Pass journal.State(); the owned snapshot cannot race a later journal adoption.
    // Defer live Apply events until ReplayStep completes, as AppWindow does during restoration.
    // Active metadata.live_turn is merged only into its matching final user turn, even with run set.
    // Saved started_tick values use the adapter's monotonic epoch; an explicit run start takes precedence.
    uint64_t BeginReplay(const SessionState& state, TranscriptTime now, TranscriptRun run = {});
    // At most 64 messages/live tools per call. Old generations are ignored after Clear/EraseFrom.
    bool ReplayStep(uint64_t generation, size_t max_messages = 64);
    bool Loading() const { return replay_ != nullptr; }

private:
    void FoldTurn(size_t index);
    struct Replay;
    std::unique_ptr<Replay> replay_;
    uint64_t generation_ = 0;
    std::vector<ChatItem> items_;
};

struct TranscriptHistory {
    std::string path, title, workspace, scope;
    uint64_t updated_at = 0;
    bool pinned = false, has_messages = false;
};
// Streams summary events only; has_messages stays true across empty edited snapshots.
Result<TranscriptHistory> ReadTranscriptHistory(const std::string& path,
                                                const CancellationToken* cancellation = nullptr);

}  // namespace spark
