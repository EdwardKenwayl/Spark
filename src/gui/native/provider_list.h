#pragma once

#include <wx/vlbox.h>

#include <array>
#include <functional>
#include <vector>

namespace spark {

struct ProviderCard {
    wxString id;
    wxString name;
    wxString protocol;
    wxString endpoint;
};

enum class ProviderListAction {
    Test,
    Edit,
    Duplicate,
    Delete,
};

class NativeProviderList : public wxVListBox {
public:
    explicit NativeProviderList(wxWindow* parent);
    void SetProviders(std::vector<ProviderCard> providers);
    void SetActionHandler(std::function<void(size_t, ProviderListAction)> handler);
    void SetReorderHandler(std::function<void(size_t, size_t)> handler);
    void SetActionTooltips(const std::array<wxString, 4>& tooltips, const wxString& reorder_tooltip);
    bool IsCompact() const { return compact_; }

protected:
    void OnDrawItem(wxDC& dc, const wxRect& rect, size_t index) const override;
    void OnDrawBackground(wxDC& dc, const wxRect& rect, size_t index) const override;
    wxCoord OnMeasureItem(size_t index) const override;

private:
    void UpdateDensity();
    int RowAt(const wxPoint& point) const;
    bool IsGrip(const wxRect& row, const wxPoint& point) const;
    int ActionAt(const wxRect& row, const wxPoint& point) const;
    wxRect ActionRect(const wxRect& row, int action) const;
    void UpdateHover(const wxPoint& point);
    void FinishPointer(const wxPoint& point);
    void DrawAction(wxDC& dc, const wxRect& rect, int action, const wxColour& color) const;

    std::vector<ProviderCard> providers_;
    std::function<void(size_t, ProviderListAction)> action_handler_;
    std::function<void(size_t, size_t)> reorder_handler_;
    std::array<wxString, 4> tooltips_;
    wxString reorder_tooltip_;
    wxPoint press_point_;
    int hovered_row_ = wxNOT_FOUND;
    int hovered_action_ = wxNOT_FOUND;
    int pressed_row_ = wxNOT_FOUND;
    int pressed_action_ = wxNOT_FOUND;
    int drop_index_ = wxNOT_FOUND;
    bool dragging_ = false, drag_armed_ = false;
    bool compact_ = false;
};

}  // namespace spark
