#pragma once

#include "gui/transcript.h"
#include "render/markdown.h"

#include <wx/scrolwin.h>
#include <functional>
#include <map>

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
    std::function<void(size_t)> edit;

private:
    enum class Action { None, Turn, Tool, Group, Copy, Edit, Rail };
    struct Hit { size_t item = kNoChatItem; Action action = Action::None; };
    struct RailDash { size_t item; int content_top, y; };
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
        wxString timestamp;
        bool user = false, expanded = false, group_expanded = false, group_active = false, group_failed = false;
        uint64_t revision = 0;
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
    void LayoutText(wxDC& dc, Row& row, const std::vector<markdown::MarkdownLine>& lines,
                    int left, int width, int& top, bool muted);
    void LayoutMarkdown(wxDC& dc, Row& row, const std::vector<markdown::MarkdownLine>& lines,
                        int left, int width, int& top);
    void PaintText(wxDC& dc, const Row& row);
    wxColour Foreground() const;
    wxColour Muted() const;
    Transcript* transcript_ = nullptr;
    bool dark_ = true, dirty_ = true, follow_ = true;
    uint64_t loading_ = 0;
    uint64_t elapsed_frame_ = 0;
    uint64_t generation_ = 0;
    int layout_width_ = 0;
    size_t hovered_ = kNoChatItem;
    Action hovered_action_ = Action::None;
    wxString empty_title_;
    wxFont body_, mono_, time_;
    std::vector<size_t> rows_;
    std::vector<RailDash> rail_;
    std::map<size_t, Row> row_cache_;
    std::map<size_t, MessageMarkdown> markdown_;
};

}  // namespace spark
