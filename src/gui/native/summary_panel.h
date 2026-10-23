#pragma once

#include "core/agent_types.h"
#include <wx/bitmap.h>
#include <wx/panel.h>
#include <functional>
#include <string>
#include <vector>

namespace spark {

struct SubAgentInfo;
enum class SubAgentState;
class NativeButton;
class NativeChatSurface;

// UI-thread-only view. wx owns child windows; main owns transcripts and workers.
class NativeSummaryPanel : public wxPanel {
public:
    static constexpr size_t kMaxSources = 512;
    static constexpr size_t kMaxAgents = 64;
    static constexpr int kMaxThumbnailPixels = 128;

    struct Callbacks {
        std::function<void()> close;
        // Called after detaching the previous transcript; empty ID means exit.
        std::function<void(const std::string&)> active_child;
        std::function<void(const SourceReference&)> open_source;
        // Cache-only, non-reentrant, no GUI mutation. Return an already bounded
        // bitmap <= max_edge_pixels in both axes, or an invalid bitmap.
        std::function<wxBitmap(const SourceReference&, int max_edge_pixels)> thumbnail;
        // Popup source-summary links: main opens docked Sources and dismisses host.
        std::function<void()> show_sources;
        // Popup group toggled: main clamps/resizes/repositions its host.
        std::function<void(const wxSize&)> popup_size_changed;
    };

    explicit NativeSummaryPanel(wxWindow* parent, Callbacks callbacks);
    ~NativeSummaryPanel() override;
    void SetDark(bool dark);
    // Inspects at most kMaxSources inputs; excludes subagents and deduplicates.
    // Unchanged metadata preserves layout/cache hits and retries cache-only misses.
    void SetSources(const std::vector<SourceReference>& sources);
    // Async cache completion: retry lookups and repaint visible sources only.
    // Preserves metadata, cached bitmaps, selection, scroll and child transcript.
    void RefreshThumbnails();
    // Replaces changed bounded metadata only; preserves a surviving child/transcript.
    // Never retains task/output/error or journals.
    void SetAgents(const std::vector<SubAgentInfo>& agents);
    // Compatibility name: upserts metadata, not a per-child visual tab.
    // False: empty/>128-byte ID or agent limit reached.
    bool AddAgentTab(const SubAgentInfo& agent);
    // False leaves selection unchanged. Real transitions notify active_child.
    // Re-selecting the same child/tab preserves layout, row selection and scroll.
    bool SelectAgentTab(const std::string& id);
    void ShowAgents();
    void ShowSources();
    void Close();
    // Call with true on every popup open to reset popup-only collapse state.
    void SetPopupMode(bool popup);
    // Call on parent-session reload, not ordinary live metadata refresh.
    void ResetOverview();
    // Popup mode only: preferred 320-DIP width and visible-row height +20 DIP.
    // Main clamps to work area, positions and dismisses its popup host.
    wxSize PopupSize() const;
    bool InteractionActive() const;
    const std::string& ActiveChildId() const { return active_child_; }
    // Borrowed window. SetTranscript borrows main's transcript; detach it before
    // destroying that transcript. Main must drive Changed/Tick/SetLoading.
    NativeChatSurface* ChildChatSurface() const { return chat_; }

private:
    enum class Page { Agents, Sources, Child };
    struct Source {
        SourceReference reference;
        wxBitmap thumbnail;
        bool thumbnail_attempted = false;
        bool actionable = true;
    };
    struct Agent {
        explicit Agent(const SubAgentInfo& info);
        bool operator==(const Agent& other) const;
        std::string id, name, provider, model, effort;
        wxString status;
        SubAgentState state{};
        uint64_t started_at = 0, finished_at = 0;
    };
    enum class RowKind { Section, Agent, Empty, More, Group, Divider, Source, SourceLink };
    struct Row {
        RowKind kind;
        wxString title;
        size_t item = 0;
        bool toggle = false;
    };
    class Rows;
    void RebuildRows();
    int RowHeight(const Row& row) const;
    void Select(Page page, const std::string& id = {});
    void RefreshView();
    void PaintDetail(wxPaintEvent&);
    void ActivateRow(size_t index);
    Callbacks callbacks_;
    Page page_ = Page::Agents;
    bool dark_ = true, popup_ = false, show_all_ = false, agents_expanded_ = false;
    std::string active_child_;
    std::vector<Source> sources_;
    std::vector<Agent> agents_;
    std::vector<size_t> agent_order_;
    std::vector<Row> display_rows_;
    wxPanel *header_ = nullptr, *detail_ = nullptr;
    NativeButton *agents_tab_ = nullptr, *sources_tab_ = nullptr;
    Rows* rows_ = nullptr;
    NativeChatSurface* chat_ = nullptr;
};

}  // namespace spark
