#pragma once

#include <windows.h>

#include "gui/chat_items.h"
#include "render/markdown.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace spark {

class ChatView {
public:
    using Action = std::function<void(size_t)>;

    ChatView();
    ~ChatView();

    bool Create(HWND parent, HINSTANCE instance, int control_id, HFONT text_font, HFONT mono_font);
    HWND hwnd() const { return window_; }
    void SetTheme(bool dark);
    void RefreshLocalization();
    void SetActivityFrame(int frame);
    void SetActivityState(bool active, const std::wstring& label, uint64_t start_tick_ms = 0);

    void SetActions(Action toggle_tool, Action copy_item, Action edit_item);
    void SetGroupAction(Action toggle_group);

    void Clear();
    // Replay only updates rows until EndRestore schedules bounded layout work.
    // Loading remains true through replay and layout; reuse the file-read start tick
    // so only slow restorations show a circle.
    void BeginRestore(uint64_t started_tick_ms = 0);
    void EndRestore();
    bool Loading() const { return loading_; }
    size_t AddUser(const std::string& text, uint64_t timestamp = 0);
    size_t AddTurnStatus(uint64_t timestamp = 0, uint64_t start_tick_ms = 0);
    size_t AddAssistant(const std::string& text = {}, uint64_t timestamp = 0);
    size_t AddReasoning(const std::string& text = {}, uint64_t timestamp = 0);
    size_t AddCompactionNotice(bool automatic = true, uint64_t timestamp = 0);
    size_t AddError(const std::string& text, uint64_t timestamp = 0);
    size_t AddQuotaNotice(uint64_t timestamp = 0);
    size_t AddUsageStats(uint64_t input_tokens, uint64_t output_tokens, uint64_t cached_input_tokens,
                         uint64_t model_calls, uint64_t context_tokens, uint64_t context_limit_tokens,
                         uint64_t elapsed_ms, uint64_t timestamp = 0);
    size_t AddTool(const std::string& name, const std::string& arguments, uint64_t timestamp = 0);

    void AppendAssistant(size_t index, const std::string& text);
    void FinishAssistant(size_t index, uint64_t timestamp = 0);
    void FinishTurnStatus(size_t index, uint64_t elapsed_ms, uint64_t timestamp = 0);
    void MarkTurnPaused(size_t index, uint64_t elapsed_ms, uint64_t timestamp = 0);
    void AppendReasoning(size_t index, const std::string& text);
    void FinishReasoning(size_t index, uint64_t timestamp = 0);
    void HideReasoning(size_t index);
    void FinishTool(size_t index, bool success, const std::string& result, uint64_t timestamp = 0,
                    uint64_t elapsed_ms = 0);
    void ToggleTool(size_t index);
    void ToggleToolGroup(size_t index);
    void ToggleTurnStatus(size_t index);
    void EraseFrom(size_t index);

    size_t Count() const { return items_.size(); }
    const ChatItem* Item(size_t index) const;
    size_t LastUserIndex() const;

    void ScrollToBottom();
    void Rebuild();

private:
    friend struct ChatViewLoadTest;

    struct LayoutCache;
    struct HitArea {
        enum class Type {
            None,
            Message,
            TurnStatus,
            ToolHeader,
            ToolGroup,
            Copy,
            Edit,
            Rail,
        };

        Type type = Type::None;
        size_t item = static_cast<size_t>(-1);
        RECT rect{};
        bool Contains(POINT point) const {
            return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
        }
    };

    // One dash of the conversation rail, i.e. the position of a single user turn.
    struct RailDash {
        size_t item = 0;
        int rail_y = 0;
        int content_top = 0;
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

    int ContentWidth() const;
    int CalculateContentHeight(HDC device);
    std::vector<HitArea> CalculateHitAreas(HDC device);
    HitArea HitTest(POINT point);
    void ClampScroll(int content_height, int viewport_height);
    void UpdateRail(HDC device);
    void DrawScrollRail(HDC device, const RECT& client);
    void SetHover(const HitArea& area);
    void PushItem(ChatItem item);
    void InvalidateItemHeight(size_t index = static_cast<size_t>(-1));
    void EnsureItemHeights(HDC device);
    LayoutCache& EnsureGroups();
    void PrepareItemHeight(HDC device, size_t index);
    void PrepareAssistantMarkdown(size_t index);
    void PrepareRestoreRows();
    void CancelRestore();
    // Reuses one full-window back buffer instead of allocating a compatible bitmap on
    // every paint, which is measurable on the low-end Windows 7 machines this targets.
    HDC EnsureBackBuffer(HDC target, int width, int height);
    void ReleaseBackBuffer();
    int Scale(int pixels) const;

    HWND window_ = nullptr;
    HFONT text_font_ = nullptr;
    HFONT bold_font_ = nullptr;
    HFONT italic_font_ = nullptr;
    HFONT bold_italic_font_ = nullptr;
    HFONT heading_font_ = nullptr;
    HFONT mono_font_ = nullptr;
    HFONT time_font_ = nullptr;
    std::vector<ChatItem> items_;
    Action toggle_tool_;
    Action toggle_group_;
    Action copy_item_;
    Action edit_item_;
    std::vector<RailDash> rail_dashes_;
    std::vector<int> item_heights_;
    // Cached rendered Markdown height for assistant rows. This is deliberately separate
    // from item_heights_, which includes metadata and the inter-item gap.
    std::vector<int> item_text_heights_;
    // Incremental Markdown source for assistant rows. Finished messages keep their
    // committed line model so repainting an old transcript does not reparse it.
    std::vector<markdown::StreamingMarkdown> streaming_markdown_;
    std::vector<bool> markdown_ready_;
    std::unique_ptr<LayoutCache> layout_cache_;
    bool restoring_ = false;
    bool loading_ = false;
    uint64_t restore_started_tick_ = 0;
    bool heights_dirty_ = true;
    UINT_PTR restore_spinner_timer_ = 0;
    UINT_PTR restore_prepare_timer_ = 0;
    // Never reuse cancelled timer ids: queued WM_TIMER messages may arrive later.
    UINT_PTR next_restore_timer_ = 100;
    int restore_frame_ = 0;
    int content_height_ = 18;
    size_t last_user_index_ = static_cast<size_t>(-1);
    int layout_client_width_ = -1;
    int dpi_ = 96;
    HDC back_dc_ = nullptr;
    HBITMAP back_bitmap_ = nullptr;
    HGDIOBJ back_bitmap_old_ = nullptr;
    int back_width_ = 0;
    int back_height_ = 0;
    bool rail_dirty_ = true;
    bool stream_render_pending_ = false;
    // Keeps live output following the newest row until the user explicitly navigates
    // to an older turn. A rail click on the newest dash re-enables following.
    bool auto_scroll_enabled_ = true;
    int scroll_y_ = 0;
    size_t hover_item_ = static_cast<size_t>(-1);
    HitArea::Type hover_type_ = HitArea::Type::None;
    bool tracking_mouse_ = false;
    int activity_frame_ = 0;
    bool activity_visible_ = false;
    std::wstring activity_label_;
    uint64_t activity_start_tick_ms_ = 0;
};

}  // namespace spark
