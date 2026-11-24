#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace spark {

enum class ChatItemKind {
    User,
    TurnStatus,
    Assistant,
    Reasoning,
    Tool,
    Compaction,
    Error,
    Quota,
    UsageStats,
    Reconnect,
};

inline bool HasAssistantText(std::string_view text) {
    return text.find_first_not_of(" \t\r\n\f\v") != std::string_view::npos;
}

inline const wchar_t* CompactionLabel(bool finished, bool automatic) {
    if (!finished) return L"Compacting context...";
    return automatic ? L"Context automatically compacted" : L"Context manually compacted";
}

struct ChatItem {
    ChatItemKind kind = ChatItemKind::Assistant;
    std::string text;
    std::vector<std::string> image_paths;
    bool timeline_separator = false;
    std::string tool_name;
    std::string arguments;
    std::string result;
    std::string call_id;
    bool success = true;
    bool finished = true;
    bool paused = false;
    bool visible = true;
    bool expanded = false;
    // Set once when the owning turn finishes, so copy metadata belongs only to
    // the final assistant reply and never to intermediate text before tool calls.
    bool final_assistant = false;
    // Collapse state for a run of consecutive tool calls. Only the first tool of
    // the run owns the flag; the others follow it.
    bool group_collapsed = true;
    uint64_t timestamp = 0;
    uint64_t start_tick_ms = 0;
    uint64_t elapsed_ms = 0;
    uint64_t input_tokens = 0;
    uint64_t output_tokens = 0;
    uint64_t cached_input_tokens = 0;
    uint64_t model_calls = 0;
    uint64_t context_tokens = 0;
    uint64_t context_limit_tokens = 0;
    uint64_t revision = 0;
};

inline size_t PendingCompaction(const std::vector<ChatItem>& items) {
    for (size_t index = items.size(); index > 0; --index) {
        const auto& item = items[index - 1];
        if (item.kind == ChatItemKind::TurnStatus || item.kind == ChatItemKind::User) break;
        if (item.kind == ChatItemKind::Compaction && item.visible && !item.finished) return index - 1;
    }
    return items.size();
}

}  // namespace spark
