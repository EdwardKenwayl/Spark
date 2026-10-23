#include "gui/native/controls.h"
#include "gui/native/popup_menu.h"
#include "platform/clock.h"

#include <wx/dcbuffer.h>
#include <wx/nonownedwnd.h>
#include <wx/popupwin.h>
#include <wx/region.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/weakref.h>
#include <wx/menu.h>
#include <algorithm>
#include <cmath>

namespace spark {
namespace {
bool Dark(const wxWindow* window) { return window->GetForegroundColour().Red() > 180; }
void Glyph(wxDC& dc, const wxArtID& art, wxPoint at, wxColour color) {
    dc.SetPen(wxPen(color, 1));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    const auto line = [&](int x, int y, int a, int b) {
        dc.DrawLine(at.x + x, at.y + y, at.x + a, at.y + b);
    };
    if (art == wxART_PLUS || art == wxART_NEW) {
        line(3, 8, 13, 8); line(8, 3, 8, 13);
    } else if (art == wxART_GO_FORWARD || art == wxART_GO_UP) {
        line(8, 14, 8, 2); line(3, 7, 8, 2); line(8, 2, 13, 7);
    } else if (art == wxART_GO_BACK || art == wxART_UNDO) {
        line(14, 8, 2, 8); line(2, 8, 7, 3); line(2, 8, 7, 13);
    } else if (art == wxART_CROSS_MARK) {
        line(4, 4, 12, 12); line(12, 4, 4, 12);
    } else if (art == wxART_LIST_VIEW) {
        for (int x : {3, 8, 13}) dc.DrawCircle(at.x + x, at.y + 8, 1);
    } else if (art == wxART_INFORMATION) {
        dc.DrawRoundedRectangle(at.x + 2, at.y + 2, 12, 12, 2);
        line(10, 2, 10, 14);
    } else if (art == wxART_EXECUTABLE_FILE) {
        dc.DrawCircle(at.x + 8, at.y + 8, 5);
        dc.DrawCircle(at.x + 8, at.y + 8, 2);
        for (const auto& p : {wxPoint(8, 0), wxPoint(8, 14), wxPoint(0, 8), wxPoint(14, 8)})
            dc.DrawRectangle(at + p, wxSize(2, 2));
    } else if (art == wxART_FOLDER || art == wxART_FOLDER_OPEN) {
        line(1, 4, 6, 4); line(6, 4, 8, 6); line(8, 6, 15, 6);
        line(15, 6, 15, 13); line(15, 13, 1, 13); line(1, 13, 1, 4);
    } else if (art == wxART_DELETE) {
        line(2, 4, 14, 4); line(5, 2, 11, 2);
        line(4, 5, 5, 14); line(5, 14, 11, 14); line(11, 14, 12, 5);
    } else if (art == wxART_COPY) {
        dc.DrawRoundedRectangle(at.x + 5, at.y + 5, 9, 9, 1);
        line(2, 11, 2, 2); line(2, 2, 11, 2);
    } else if (art == wxART_EDIT) {
        line(3, 10, 11, 2); line(11, 2, 14, 5); line(14, 5, 6, 13);
        line(6, 13, 2, 14); line(2, 14, 3, 10);
    } else {
        dc.DrawRoundedRectangle(at.x + 2, at.y + 2, 12, 12, 2);
        line(5, 6, 11, 6); line(5, 10, 11, 10);
    }
}
void Text(wxDC& dc, const wxString& text, const wxRect& bounds) {
    const auto label = wxControl::Ellipsize(text, dc, wxELLIPSIZE_END, std::max(1, bounds.width));
    dc.DrawText(label, bounds.x, bounds.y + (bounds.height - dc.GetTextExtent(label).y) / 2);
}
}  // namespace

NativePalette NativeColors(bool dark) {
    if (dark) return {wxColour(28,29,31), wxColour(37,38,40), wxColour(43,44,47),
        wxColour(48,49,52), wxColour(61,62,66), wxColour(67,68,72), wxColour(236,237,240),
        wxColour(150,153,160)};
    return {wxColour(250,250,250), wxColour(242,242,243), wxColour(255,255,255),
        wxColour(255,255,255), wxColour(231,232,234), wxColour(214,216,219), wxColour(31,32,35),
        wxColour(103,106,112)};
}
void ApplyNativeColors(wxWindow* window, bool dark) {
    const auto palette = NativeColors(dark);
    const bool input = dynamic_cast<wxTextCtrl*>(window) != nullptr;
    window->SetBackgroundColour(input ? palette.input : palette.background);
    window->SetForegroundColour(palette.text);
    for (auto* child : window->GetChildren()) ApplyNativeColors(child, dark);
    window->Refresh(false);
}
void ApplyRoundedPopupShape(wxNonOwnedWindow* window, int radius_dip) {
    if (!window || radius_dip <= 0) return;
    const wxSize size = window->GetClientSize();
    if (size.x <= 1 || size.y <= 1) return;
    const int radius = std::min({window->FromDIP(radius_dip), size.x / 2, size.y / 2});
    if (radius <= 1) return;
    constexpr int steps = 6;
    constexpr double pi = 3.14159265358979323846;
    const wxPoint centers[] = {
        {radius, radius}, {size.x - radius - 1, radius},
        {size.x - radius - 1, size.y - radius - 1}, {radius, size.y - radius - 1},
    };
    const int starts[] = {180, 270, 0, 90};
    std::vector<wxPoint> points;
    points.reserve(4 * (steps + 1));
    for (size_t corner = 0; corner < std::size(centers); ++corner) {
        for (int step = 0; step <= steps; ++step) {
            const double angle = (starts[corner] + 90.0 * step / steps) * pi / 180.0;
            points.emplace_back(
                centers[corner].x + static_cast<int>(std::lround(std::cos(angle) * radius)),
                centers[corner].y + static_cast<int>(std::lround(std::sin(angle) * radius)));
        }
    }
    const wxRegion shape(points.size(), points.data());
    window->SetShape(shape);
}

NativeButton::NativeButton(wxWindow* parent, const wxString& label, const wxArtID& art)
    : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), art_(art) {
    SetLabel(label);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(FromDIP(wxSize(label.empty() ? 32 : 110, 34)));
    Bind(wxEVT_PAINT, &NativeButton::Paint, this);
    Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { hover_ = true; Refresh(); });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hover_ = false; Refresh(); });
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { SetFocus(); pressed_ = true; CaptureMouse(); Refresh(); });
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { pressed_ = false; Refresh(); });
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
        const bool activate = pressed_ && GetClientRect().Contains(event.GetPosition());
        pressed_ = false;
        if (HasCapture()) ReleaseMouse();
        Refresh();
        if (activate) Activate();
    });
    Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_SPACE || event.GetKeyCode() == WXK_RETURN) Activate();
        else event.Skip();
    });
    Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) { Refresh(); event.Skip(); });
    Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) { Refresh(); event.Skip(); });
}
void NativeButton::Activate() {
    if (!IsEnabled()) return;
    wxCommandEvent event(wxEVT_BUTTON, GetId());
    event.SetEventObject(this);
    GetEventHandler()->ProcessEvent(event);
}
void NativeButton::Paint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    const auto p = NativeColors(Dark(this));
    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour())); dc.Clear();
    if (GetName() == "spark.model") {
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(pressed_ ? p.selection : p.input));
        dc.DrawRectangle(GetClientRect());
        dc.SetFont(GetFont());
        int right = GetClientSize().x - FromDIP(24);
        if (!badge_.empty()) {
            const int width = std::min(dc.GetTextExtent(badge_).x + FromDIP(18),
                                      std::max(0, GetClientSize().x / 2));
            const wxRect badge(right - width, FromDIP(4), width,
                               std::max(1, GetClientSize().y - FromDIP(8)));
            dc.SetPen(wxPen(p.border)); dc.SetBrush(wxBrush(p.selection));
            dc.DrawRoundedRectangle(badge, FromDIP(6));
            dc.SetTextForeground(Dark(this) ? wxColour(91, 99, 112) : wxColour(70, 78, 92));
            dc.DrawLabel(wxControl::Ellipsize(badge_, dc, wxELLIPSIZE_END, std::max(1, width)),
                         badge, wxALIGN_CENTER);
            right = badge.x - FromDIP(10);
        }
        dc.SetTextForeground(IsEnabled() ? p.text : p.muted);
        Text(dc, GetLabel(), {FromDIP(14), 0, std::max(1, right - FromDIP(14)), GetClientSize().y});
        const int x = GetClientSize().x - FromDIP(12), y = GetClientSize().y / 2;
        dc.SetPen(wxPen(p.muted));
        dc.DrawLine(x - FromDIP(3), y - FromDIP(2), x, y + FromDIP(1));
        dc.DrawLine(x, y + FromDIP(1), x + FromDIP(3), y - FromDIP(2));
        return;
    }
    const bool fill = selected_ || pressed_ || hover_;
    const bool section = GetName().StartsWith("spark.sidebar.section.");
    const bool sidebar = GetParent()->GetName() == "spark.sidebar";
    const bool sidebar_hover = fill && sidebar && !section;
    const bool sidebar_action = sidebar && GetLabel().empty();
    wxColour ink = sidebar_hover ? wxColour(250, 250, 250) :
        !IsEnabled() || sidebar_action ? p.muted : p.text;
    wxColour background = sidebar_hover ? Dark(this) ? p.selection : wxColour(42, 43, 46) :
        fill && !section ? p.selection : GetParent()->GetBackgroundColour();
    if (appearance_ == NativeButtonAppearance::Primary) {
        background = pressed_ ? wxColour(205, 207, 212) : wxColour(236, 237, 240);
        ink = IsEnabled() ? wxColour(30, 31, 33) : p.muted;
    } else if (appearance_ == NativeButtonAppearance::Destructive) {
        background = pressed_ ? wxColour(85, 48, 48) : wxColour(63, 43, 45);
        ink = IsEnabled() ? wxColour(232, 135, 135) : p.muted;
    }
    dc.SetPen(HasFocus() && !sidebar ? wxPen(p.border) : *wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(background));
    if (appearance_ == NativeButtonAppearance::Plain && !sidebar)
        dc.DrawRoundedRectangle(GetClientRect().Deflate(1), FromDIP(6));
    else dc.DrawRectangle(GetClientRect());
    dc.SetFont(GetFont()); dc.SetTextForeground(ink);
    int left = FromDIP(section ? 0 : 12);
    if (GetName() == "spark.send") {
        const bool dark = Dark(this), busy = art_ == wxART_CROSS_MARK;
        const wxColour accent = dark ? wxColour(91, 99, 112) : wxColour(70, 78, 92);
        const wxColour active = dark ? wxColour(108, 117, 132) : wxColour(49, 56, 68);
        const wxColour error = dark ? wxColour(242, 128, 128) : wxColour(190, 54, 54);
        const wxColour fill = !IsEnabled() ? wxColour(65, 66, 70) : busy ? error : pressed_ ? active : accent;
        const wxColour symbol = IsEnabled() ? p.background : p.muted;
        const int x = GetClientSize().x / 2, y = GetClientSize().y / 2;
        dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush(fill));
        dc.DrawEllipse(GetClientRect());
        dc.SetBrush(wxBrush(symbol));
        if (busy) dc.DrawRectangle(x - FromDIP(5), y - FromDIP(5), FromDIP(10), FromDIP(10));
        else if (art_ == "spark.play") {
            const wxPoint points[] = {{x - FromDIP(4), y - FromDIP(7)},
                {x - FromDIP(4), y + FromDIP(7)}, {x + FromDIP(7), y}};
            dc.DrawPolygon(3, points);
        } else {
            dc.SetPen(wxPen(symbol, FromDIP(2)));
            dc.DrawLine(x, y + FromDIP(7), x, y - FromDIP(7));
            dc.DrawLine(x - FromDIP(5), y - FromDIP(2), x, y - FromDIP(7));
            dc.DrawLine(x, y - FromDIP(7), x + FromDIP(5), y - FromDIP(2));
        }
    } else if (GetName() == "spark.summary") {
        const int x = GetClientSize().x / 2, y = GetClientSize().y / 2;
        dc.SetPen(wxPen(ink)); dc.SetBrush(*wxTRANSPARENT_BRUSH);
        for (const int offset : {-5, 5}) {
            dc.DrawEllipse(x - FromDIP(10), y + FromDIP(offset - 3), FromDIP(6), FromDIP(6));
            dc.DrawLine(x, y + FromDIP(offset), x + FromDIP(9), y + FromDIP(offset));
        }
    } else if (sidebar && !GetLabel().empty() && !section) {
        const int y = GetClientSize().y / 2;
        dc.SetPen(wxPen(ink)); dc.SetBrush(*wxTRANSPARENT_BRUSH);
        if (GetName() == "spark.new-chat") {
            dc.DrawRectangle(FromDIP(9), y - FromDIP(6), FromDIP(13), FromDIP(12));
            dc.DrawLine(FromDIP(13), y + FromDIP(2), FromDIP(19), y - FromDIP(4));
        } else {
            dc.DrawEllipse(FromDIP(11), y - FromDIP(6), FromDIP(12), FromDIP(12));
            dc.DrawEllipse(FromDIP(14), y - FromDIP(3), FromDIP(6), FromDIP(6));
        }
        left = FromDIP(34);
    } else if (sidebar_action && art_ == wxART_LIST_VIEW) {
        const int x = GetClientSize().x / 2, y = GetClientSize().y / 2;
        dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush(ink));
        for (const int offset : {-5, 0, 5})
            dc.DrawCircle(x, y + FromDIP(offset), std::max(1, FromDIP(1)));
    } else if (sidebar_action && art_ == wxART_PLUS) {
        const int x = GetClientSize().x / 2, y = GetClientSize().y / 2;
        dc.SetPen(wxPen(ink));
        dc.DrawLine(x - FromDIP(6), y, x + FromDIP(6), y);
        dc.DrawLine(x, y - FromDIP(6), x, y + FromDIP(6));
    } else if (sidebar_action && art_ == wxART_EDIT) {
        const int x = GetClientSize().x / 2, y = GetClientSize().y / 2;
        dc.SetPen(wxPen(ink)); dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(x - FromDIP(8), y - FromDIP(7), FromDIP(13), FromDIP(14));
        dc.DrawLine(x - FromDIP(1), y + FromDIP(5), x + FromDIP(9), y - FromDIP(5));
        dc.DrawLine(x + FromDIP(9), y - FromDIP(5), x + FromDIP(6), y - FromDIP(8));
        dc.DrawLine(x + FromDIP(6), y - FromDIP(8), x - FromDIP(4), y + FromDIP(2));
        dc.DrawLine(x - FromDIP(4), y + FromDIP(2), x - FromDIP(1), y + FromDIP(5));
    } else if (!art_.empty()) {
        Glyph(dc, art_, wxPoint(GetLabel().empty() ? (GetClientSize().x - 16) / 2 : left,
                               (GetClientSize().y - 16) / 2), ink);
        left += 26;
    }
    if (!GetLabel().empty()) {
        const wxRect label_bounds(left, 0, GetClientSize().x - left -
            FromDIP(disclosure_ == NativeDisclosure::None ? 8 : 24), GetClientSize().y);
        if (label_alignment_ == wxALIGN_CENTER && art_.empty())
            dc.DrawLabel(wxControl::Ellipsize(GetLabel(), dc, wxELLIPSIZE_END,
                std::max(1, GetClientSize().x - FromDIP(8))), GetClientRect(), wxALIGN_CENTER);
        else Text(dc, GetLabel(), label_bounds);
    }
    if (disclosure_ != NativeDisclosure::None) {
        const int x = GetClientSize().x - FromDIP(10), y = GetClientSize().y / 2;
        dc.SetPen(wxPen(p.muted));
        if (disclosure_ == NativeDisclosure::Expanded) {
            dc.DrawLine(x - FromDIP(4), y - FromDIP(2), x, y + FromDIP(2));
            dc.DrawLine(x, y + FromDIP(2), x + FromDIP(4), y - FromDIP(2));
        } else {
            dc.DrawLine(x - FromDIP(2), y - FromDIP(4), x + FromDIP(2), y);
            dc.DrawLine(x + FromDIP(2), y, x - FromDIP(2), y + FromDIP(4));
        }
    }
}

NativeList::NativeList(wxWindow* parent)
    : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {
    SetItemCount(0);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
        const int hovered = HitTest(event.GetPosition());
        if (hovered != hovered_) {
            if (hovered_ >= 0) RefreshRow(hovered_);
            hovered_ = hovered;
            if (hovered_ >= 0) RefreshRow(hovered_);
        }
        event.Skip();
    });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
        const int old = hovered_; hovered_ = wxNOT_FOUND;
        if (old >= 0) RefreshRow(old);
    });
}
void NativeList::Set(const wxArrayString& labels) {
    labels_ = labels; kinds_.assign(labels.size(), NativeListKind::Chat); running_.assign(labels.size(), false);
    hovered_ = wxNOT_FOUND;
    SetItemCount(labels.size()); RefreshAll();
}
void NativeList::SetString(unsigned index, const wxString& label) {
    labels_[index] = label; RefreshRow(index);
}
void NativeList::Style(size_t index, NativeListKind kind, bool running) {
    if (index >= kinds_.size()) return;
    if (kinds_[index] == kind && running_[index] == running) return;
    kinds_[index] = kind; running_[index] = running; RefreshRow(index);
}
void NativeList::Tick() {
    if (!IsShownOnScreen()) return;
    for (size_t index = GetVisibleRowsBegin(); index < GetVisibleRowsEnd(); ++index)
        if (index < running_.size() && running_[index]) RefreshRow(index);
}
void NativeList::SetScrollbar(int orientation, int position, int thumb, int range, bool refresh) {
    const bool shared = GetName() == "spark.history" || GetName() == "spark.projects.list";
    wxVListBox::SetScrollbar(orientation, shared ? 0 : position, shared ? 0 : thumb,
                            shared ? 0 : range, refresh);
}
wxCoord NativeList::OnMeasureItem(size_t) const {
    return FromDIP(GetName() == "spark.history" || GetName() == "spark.projects.list" ? 34 : 36);
}
void NativeList::OnDrawItem(wxDC& dc, const wxRect& rect, size_t index) const {
    if (GetName() == "spark.history" || GetName() == "spark.projects.list") {
        DrawSidebarRow(dc, rect, index);
        return;
    }
    const auto p = NativeColors(Dark(this));
    const bool heading = kinds_[index] == NativeListKind::Heading;
    dc.SetBrush(wxBrush((IsSelected(index) || static_cast<int>(index) == hovered_) && !heading ?
                        p.selection : GetBackgroundColour()));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRoundedRectangle(wxRect(rect).Deflate(6, 1), FromDIP(6));
    dc.SetFont(heading ? GetFont().Smaller().Bold() : GetFont());
    dc.SetTextForeground(heading ? p.muted : p.text);
    int left = rect.x + FromDIP(kinds_[index] == NativeListKind::ProjectChat ? 32 : 16);
    if (kinds_[index] == NativeListKind::ProjectExpanded) {
        Glyph(dc, wxART_FOLDER, wxPoint(left, rect.y + (rect.height - 16) / 2), p.muted);
        left += 24;
    }
    Text(dc, labels_[index], wxRect(left, rect.y, rect.GetRight() - left -
         (heading || kinds_[index] == NativeListKind::ProjectExpanded ? 68 : 28), rect.height));
    if (heading || kinds_[index] == NativeListKind::ProjectExpanded) {
        Glyph(dc, wxART_PLUS, wxPoint(rect.GetRight() - 54, rect.y + (rect.height - 16) / 2), p.muted);
        Glyph(dc, wxART_LIST_VIEW, wxPoint(rect.GetRight() - 26, rect.y + (rect.height - 16) / 2), p.muted);
    }
    if (running_[index]) {
        dc.SetPen(wxPen(p.muted, 2)); dc.SetBrush(*wxTRANSPARENT_BRUSH);
        const double angle = MonotonicMillis() / 160.0;
        const int x = rect.GetRight() - 18, y = rect.y + rect.height / 2;
        dc.DrawArc(x + static_cast<int>(4 * std::cos(angle)), y + static_cast<int>(4 * std::sin(angle)),
                   x + static_cast<int>(4 * std::cos(angle + 4.7)), y + static_cast<int>(4 * std::sin(angle + 4.7)), x, y);
    }
}

NativeChoice::NativeChoice(wxWindow* parent)
    : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {
    SetBackgroundStyle(wxBG_STYLE_PAINT); SetMinSize(FromDIP(wxSize(110, 32)));
    Bind(wxEVT_PAINT, &NativeChoice::Paint, this);
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { SetFocus(); Open(); });
    Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { hover_ = true; Refresh(); });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hover_ = false; Refresh(); });
    Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
        const int key = event.GetKeyCode();
        if (key == WXK_SPACE || key == WXK_RETURN || (event.AltDown() && key == WXK_DOWN)) Open();
        else if (key == WXK_DOWN) Choose(std::min(selected_ + 1, static_cast<int>(items_.size()) - 1));
        else if (key == WXK_UP) Choose(std::max(0, selected_ - 1));
        else event.Skip();
    });
}
int NativeChoice::Append(const wxString& label) {
    items_.Add(label); popup_labels_.Add(label); Refresh(); return items_.size() - 1;
}
void NativeChoice::AppendSeparator() {
    if (separators_.empty() || separators_.back() != items_.size()) separators_.push_back(items_.size());
}
void NativeChoice::Clear() {
    items_.Clear(); popup_labels_.Clear(); separators_.clear(); selected_ = wxNOT_FOUND; Refresh();
}
void NativeChoice::SetSelection(int index) {
    selected_ = index >= 0 && index < static_cast<int>(items_.size()) ? index : wxNOT_FOUND; Refresh();
    SetToolTip(selected_ < 0 ? wxString{} : popup_labels_[selected_]);
}
wxString NativeChoice::GetStringSelection() const { return selected_ < 0 ? wxString{} : items_[selected_]; }
bool NativeChoice::SetStringSelection(const wxString& label) {
    const int index = items_.Index(label); SetSelection(index); return index != wxNOT_FOUND;
}
void NativeChoice::SetString(unsigned index, const wxString& label) {
    items_[index] = label; popup_labels_[index] = label; Refresh();
}
void NativeChoice::SetPopupLabel(unsigned index, const wxString& label) { popup_labels_[index] = label; }
void NativeChoice::Choose(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size()) || index == selected_) return;
    SetSelection(index);
    wxCommandEvent event(wxEVT_CHOICE, GetId());
    event.SetInt(index); event.SetString(GetStringSelection()); event.SetEventObject(this);
    GetEventHandler()->ProcessEvent(event);
}
void NativeChoice::Paint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    const bool dark = Dark(this), permission = GetName() == "spark.permissions";
    const auto p = NativeColors(dark);
    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour())); dc.Clear();
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(hover_ ? p.selection : GetParent()->GetBackgroundColour()));
    if (permission) dc.DrawRectangle(GetClientRect());
    else dc.DrawRoundedRectangle(GetClientRect(), FromDIP(5));
    dc.SetTextForeground(IsEnabled() ? p.text : p.muted); dc.SetFont(GetFont());
    int left = FromDIP(8);
    if (permission) {
        const wxColour dot = selected_ == 2 ? dark ? wxColour(242, 128, 128) : wxColour(190, 54, 54) :
            selected_ == 1 ? dark ? wxColour(111, 190, 140) : wxColour(39, 132, 82) :
            dark ? wxColour(222, 146, 75) : wxColour(171, 99, 25);
        dc.SetPen(wxPen(dot)); dc.SetBrush(wxBrush(dot));
        dc.DrawEllipse(FromDIP(10), GetClientSize().y / 2 - FromDIP(5), FromDIP(10), FromDIP(10));
        left = FromDIP(26);
    }
    if (GetName() == "spark.model") left = std::max(left, GetClientSize().x - 32 - dc.GetTextExtent(GetStringSelection()).x);
    Text(dc, GetStringSelection(), wxRect(left, 0, GetClientSize().x - left -
         FromDIP(permission ? 20 : 24), GetClientSize().y));
    const int x = GetClientSize().x - FromDIP(permission ? 13 : 16), y = GetClientSize().y / 2;
    dc.SetPen(wxPen(p.muted));
    dc.DrawLine(x - FromDIP(3), y - FromDIP(2), x, y + FromDIP(1));
    dc.DrawLine(x, y + FromDIP(1), x + FromDIP(3), y - FromDIP(2));
}
void NativeChoice::Open() {
    if (!IsEnabled() || items_.empty()) return;
    wxMenu menu;
    const size_t count = std::min<size_t>(popup_labels_.size(), 4096);
    size_t separator = 0;
    for (size_t index = 0; index < count; ++index) {
        if (separator < separators_.size() && separators_[separator] == index) {
            menu.AppendSeparator();
            ++separator;
        }
        menu.AppendCheckItem(100 + static_cast<int>(index), popup_labels_[index])
            ->Check(static_cast<int>(index) == selected_);
    }
    while (separator < separators_.size() && separators_[separator] == count) {
        menu.AppendSeparator();
        ++separator;
    }
    wxWeakRef<NativeChoice> guard(this);
    const bool above = GetName() == "spark.permissions";
    const int choice = ShowNativeMenu(this, menu, ClientToScreen(wxPoint(0, above ? 0 : GetClientSize().y)),
        above ? NativeMenuAlignment::Bottom : NativeMenuAlignment::Top);
    if (!guard || guard->IsBeingDeleted()) return;
    if (choice >= 100 && static_cast<size_t>(choice - 100) < count) Choose(choice - 100);
    SetFocus();
}

NativeComposer::NativeComposer(wxWindow* parent) : wxPanel(parent) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        const auto p = NativeColors(Dark(this));
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour())); dc.Clear();
        dc.SetBrush(wxBrush(p.panel)); dc.SetPen(wxPen(p.border));
        dc.DrawRoundedRectangle(GetClientRect().Deflate(1), FromDIP(7));
    });
}
}  // namespace spark
