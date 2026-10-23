#pragma once

#include <wx/vlbox.h>

#include <functional>
#include <vector>

namespace spark {

struct ArchiveListRow {
    bool header = false;
    wxString title, detail, tooltip;
    size_t entry = 0;
};

class NativeArchiveList : public wxVListBox {
public:
    explicit NativeArchiveList(wxWindow* parent);
    void SetRows(std::vector<ArchiveListRow> rows);
    void SetLabels(const wxString& unarchive, const wxString& remove);
    void SetActionHandler(std::function<void(size_t, bool)> handler);

protected:
    void OnDrawItem(wxDC& dc, const wxRect& rect, size_t index) const override;
    void OnDrawBackground(wxDC& dc, const wxRect& rect, size_t index) const override;
    wxCoord OnMeasureItem(size_t index) const override;

private:
    int RowAt(const wxPoint& point) const;
    wxRect ActionRect(const wxRect& row, bool restore) const;
    int ActionAt(const wxRect& row, const wxPoint& point) const;
    void Hover(const wxPoint& point);
    void Activate(int row, bool restore);

    std::vector<ArchiveListRow> rows_;
    std::function<void(size_t, bool)> action_;
    wxString unarchive_, remove_;
    int restore_width_ = 0;
    int hovered_ = wxNOT_FOUND, hovered_action_ = 0;
    int pressed_ = wxNOT_FOUND, pressed_action_ = 0;
};

}  // namespace spark
