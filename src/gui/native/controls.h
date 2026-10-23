#pragma once

#include <wx/artprov.h>
#include <wx/control.h>
#include <wx/panel.h>
#include <wx/vlbox.h>
#include <vector>

class wxNonOwnedWindow;

namespace spark {

struct NativePalette {
    wxColour background, sidebar, panel, input, selection, border, text, muted;
};
NativePalette NativeColors(bool dark);
void ApplyNativeColors(wxWindow* window, bool dark);
void ApplyRoundedPopupShape(wxNonOwnedWindow* window, int radius_dip);

// Lightweight painted controls; editing and IME remain native wxTextCtrl.
enum class NativeDisclosure { None, Collapsed, Expanded };
enum class NativeButtonAppearance { Plain, Primary, Destructive };

class NativeButton : public wxControl {
public:
    NativeButton(wxWindow* parent, const wxString& label = {}, const wxArtID& art = {});
    void SetArt(const wxArtID& art) { art_ = art; Refresh(); }
    void SetSelected(bool selected) { selected_ = selected; Refresh(); }
    bool IsSelected() const { return selected_; }
    void SetDisclosure(NativeDisclosure disclosure) { disclosure_ = disclosure; Refresh(); }
    void SetAppearance(NativeButtonAppearance appearance) { appearance_ = appearance; Refresh(); }
    void SetLabelAlignment(int alignment) { label_alignment_ = alignment; Refresh(); }
    void SetBadge(const wxString& badge) { badge_ = badge; Refresh(); }
private:
    void Paint(wxPaintEvent&);
    void Activate();
    wxArtID art_;
    bool hover_ = false, pressed_ = false, selected_ = false;
    NativeDisclosure disclosure_ = NativeDisclosure::None;
    NativeButtonAppearance appearance_ = NativeButtonAppearance::Plain;
    int label_alignment_ = wxALIGN_LEFT;
    wxString badge_;
};

enum class NativeListKind { Chat, Heading, ProjectExpanded, ProjectChat, ProjectCollapsed, ProjectEmpty, More };

class NativeList : public wxVListBox {
public:
    explicit NativeList(wxWindow* parent);
    void Set(const wxArrayString& labels);
    void SetString(unsigned index, const wxString& label);
    wxString GetString(unsigned index) const { return labels_[index]; }
    void Style(size_t index, NativeListKind kind, bool running = false);
    void Tick();
    int HitTest(const wxPoint& point) const { return VirtualHitTest(point.y); }
    void SetScrollbar(int orientation, int position, int thumb, int range, bool refresh = true) override;
protected:
    void OnDrawItem(wxDC& dc, const wxRect& rect, size_t index) const override;
    wxCoord OnMeasureItem(size_t index) const override;
    void OnDrawBackground(wxDC&, const wxRect&, size_t) const override {}
private:
    void DrawSidebarRow(wxDC& dc, const wxRect& rect, size_t index) const;
    wxArrayString labels_;
    std::vector<NativeListKind> kinds_;
    std::vector<bool> running_;
    int hovered_ = wxNOT_FOUND;
};

class NativeChoice : public wxControl {
public:
    explicit NativeChoice(wxWindow* parent);
    int Append(const wxString& label);
    void AppendSeparator();
    void Clear();
    bool IsEmpty() const { return items_.IsEmpty(); }
    unsigned GetCount() const { return items_.size(); }
    int GetSelection() const { return selected_; }
    void SetSelection(int index);
    wxString GetString(unsigned index) const { return items_[index]; }
    wxString GetStringSelection() const;
    int FindString(const wxString& label, bool case_sensitive = false) const { return items_.Index(label, case_sensitive); }
    bool SetStringSelection(const wxString& label);
    void SetString(unsigned index, const wxString& label);
    void SetPopupLabel(unsigned index, const wxString& label);
    void Open();
private:
    void Choose(int index);
    void Paint(wxPaintEvent&);
    wxArrayString items_;
    wxArrayString popup_labels_;
    std::vector<size_t> separators_;
    int selected_ = wxNOT_FOUND;
    bool hover_ = false;
};

class NativeComposer : public wxPanel {
public:
    explicit NativeComposer(wxWindow* parent);
};

}  // namespace spark
