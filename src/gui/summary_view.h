#pragma once

#include "core/session.h"
#include "gui/theme.h"

#include <functional>
#include <string>
#include <vector>

namespace spark {

enum class SummaryTab { Agents, Sources };
enum class SummaryAction { Agents, Sources, OpenAgent, OpenSource, More, Back, Close, ToggleAgents };
enum class SummaryRowKind { Section, Agent, Source, Link, Empty, AgentGroup, Divider };

struct SummaryRow {
    SummaryRowKind kind = SummaryRowKind::Empty;
    std::wstring title;
    std::wstring detail;
    std::wstring note;
    std::string id;
    std::string source_kind;
    uint64_t timestamp = 0;
    bool running = false;
    SummaryAction action = SummaryAction::Sources;
    std::vector<std::string> agent_ids;
    bool expanded = false;
};

// Collect provenance from the full visible transcript, independently of model
// context compaction. The panel never infers a search from ordinary prose.
std::vector<SourceReference> CollectSummarySources(const SessionState& state);
void DrawSummaryGlyph(HDC device, const RECT& rect, COLORREF color, int dpi);
void DrawSubAgentGlyph(HDC device, const RECT& rect, const std::string& id);

class SummaryView {
public:
    using Action = std::function<void(SummaryAction, const std::string&)>;
    using Thumbnail = std::function<HBITMAP(const std::string&)>;
    ~SummaryView();
    bool Create(HWND owner, HINSTANCE instance, HFONT font, bool popup);
    HWND hwnd() const { return window_; }
    void SetActions(Action action, Thumbnail thumbnail = {});
    void SetAppearance(const std::string& preference);
    void SetContent(SummaryTab tab, std::vector<SummaryRow> rows,
                    const std::wstring& title = {}, const std::wstring& model = {}, bool detail = false,
                    const std::string& agent_id = {});
    void ShowPopup(POINT anchor);
    void Hide();
    int HeaderHeight() const;

private:
    struct Hit {
        RECT rect{};
        SummaryAction action = SummaryAction::Sources;
        std::string id;
    };
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
    void Paint(HDC device);
    int Scale(int value) const { return MulDiv(value, dpi_, 96); }
    int RowHeight(const SummaryRow& row) const;
    void Scroll(int delta);
    void DragScroll(int y);
    HWND window_ = nullptr;
    HFONT font_ = nullptr;
    int dpi_ = 96;
    bool popup_ = false;
    bool detail_ = false;
    bool tracking_ = false;
    SummaryTab tab_ = SummaryTab::Agents;
    theme::Palette palette_;
    std::vector<SummaryRow> rows_;
    std::vector<Hit> hits_;
    std::wstring title_;
    std::wstring model_;
    std::string agent_id_;
    POINT popup_anchor_{};
    int scroll_ = 0;
    int content_height_ = 0;
    RECT scroll_thumb_{};
    bool scroll_dragging_ = false;
    int scroll_drag_offset_ = 0;
    POINT mouse_{-1, -1};
    Action action_;
    Thumbnail thumbnail_;
};

}  // namespace spark
