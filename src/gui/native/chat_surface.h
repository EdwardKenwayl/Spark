#pragma once

#include "gui/transcript.h"
#include "gui/native/thumbnail_cache.h"
#include "render/markdown.h"

#include <wx/scrolwin.h>
#include <deque>
#include <functional>
#include <map>
#include <set>

namespace spark {

class NativeChatSurface : public wxScrolledWindow {
public:
    explicit NativeChatSurface(wxWindow* parent);
    void SetTranscript(Transcript* transcript, bool reset_scroll = false);
    void SetDark(bool dark);
    void SetEmptyTitle(const wxString& title);
    void SetLoading(uint64_t started);
    bool Loading() const { return loading_ != 0; }
    void Changed();
    void Tick();
    void ScrollToLatest();
    void RequestEarlier();
    void SetEarlierLoading(bool loading);
    bool EarlierLoading() const { return earlier_loading_; }
    // Replacing a replay after prepending keeps the old visible row at its screen position.
    void PrependTranscript(Transcript* transcript, size_t added_items);
    std::function<void(size_t)> edit;
    std::function<void(const std::string&)> open_image;
    std::function<void()> load_earlier;

private:
    friend struct NativeGuiTestAccess;
    enum class Action { None, Turn, Tool, Group, Copy, Edit, Rail, Earlier, Image };
    struct Hit { size_t item = kNoChatItem; Action action = Action::None; size_t image = kNoChatItem; };
    struct RailDash { size_t item; int content_top, content_end, y; };
    struct Fragment {
        wxString text;
        wxPoint origin;
        wxFont font;
        wxColour color;
        bool live = false, highlight = false;
    };
    enum class PanelKind { Code, InlineCode, Rule };
    struct MarkdownPanel { wxRect bounds; PanelKind kind; };
    struct Row {
        size_t item = 0;
        wxRect bounds, message, bubble, header, group_header, copy, edit, time;
        wxRect separator;
        wxString timestamp, timeline;
        std::vector<wxRect> images;
        bool user = false, expanded = false, group_expanded = false, group_active = false, group_failed = false;
        uint64_t revision = 0;
        size_t reasoning = kNoChatItem, active_tool = kNoChatItem;
        std::vector<Fragment> fragments;
        std::vector<MarkdownPanel> panels;
    };
    struct MessageMarkdown {
        markdown::StreamingMarkdown parser;
        size_t source_bytes = 0;
        bool finished = false;
    };
    void Reflow(wxDC& dc);
    void Paint(wxPaintEvent&);
    void Click(wxMouseEvent&);
    void Context(wxContextMenuEvent&);
    void Copy(size_t index);
    Hit HitAt(const wxPoint& client_point) const;
    void ScrollBy(int pixels);
    void UpdateRail(int content_height);
    void DrawRail(wxDC& dc);
    bool IsTurnVisible(const RailDash& dash) const;
    void ResetThumbnails();
    bool PollThumbnails();
    wxBitmap Thumbnail(const std::string& path, const wxSize& bounds) const;
    void LayoutText(wxDC& dc, Row& row, const std::vector<markdown::MarkdownLine>& lines,
                    int left, int width, int& top, bool muted);
    void LayoutMarkdown(wxDC& dc, Row& row, const std::vector<markdown::MarkdownLine>& lines,
                        int left, int width, int& top);
    void PaintText(wxDC& dc, const Row& row);
    wxString ActivityText() const;
    wxColour Foreground() const;
    wxColour Muted() const;
    Transcript* transcript_ = nullptr;
    bool dark_ = true, dirty_ = true, follow_ = true;
    uint64_t loading_ = 0;
    uint64_t elapsed_frame_ = 0;
    uint64_t generation_ = 0;
    bool earlier_loading_ = false;
    size_t anchor_item_ = kNoChatItem, activity_item_ = kNoChatItem, activity_status_ = kNoChatItem;
    int anchor_offset_ = 0;
    wxRect earlier_bounds_;
    int layout_width_ = 0;
    size_t hovered_ = kNoChatItem;
    Action hovered_action_ = Action::None;
    wxString empty_title_;
    wxFont body_, mono_, time_;
    std::vector<size_t> rows_;
    std::vector<RailDash> rail_;
    std::map<size_t, Row> row_cache_;
    std::map<size_t, MessageMarkdown> markdown_;
    NativeThumbnailCache thumbnails_;
    std::set<std::string> thumbnail_paths_;
    std::map<std::string, wxBitmap> thumbnail_bitmaps_;
    std::deque<std::string> thumbnail_lru_;
    int thumbnail_edge_ = 0;
};

}  // namespace spark
