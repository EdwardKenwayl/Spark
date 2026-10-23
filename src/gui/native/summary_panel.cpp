#include "gui/native/summary_panel.h"
#include "core/common.h"
#include "core/subagents.h"
#include "gui/localization.h"
#include "gui/native/chat_surface.h"
#include "gui/native/controls.h"

#include <wx/dcbuffer.h>
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/utils.h>
#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
#include <utility>

namespace spark {
namespace {
wxString T(const wchar_t* value) { return wxString(i18n::Text(value)); }
wxString U(const std::string& value) {
    auto text = wxString::FromUTF8(value.data(), value.size());
    text.Replace("\r", " "); text.Replace("\n", " "); text.Replace("\t", " ");
    return text;
}
wxString UDetail(const std::string& value) {
    auto text = wxString::FromUTF8(value.data(), value.size());
    text.Replace("\r\n", "\n"); text.Replace("\r", "\n"); text.Replace("\t", " ");
    return text;
}
std::string Bound(const std::string& value, size_t bytes) {
    return TruncateUtf8(value.substr(0, bytes + 4), bytes);
}
bool Web(const std::string& kind) { return kind == "url_citation" || kind.rfind("web", 0) == 0; }
wxString SourceNote(const std::string& kind) {
    if (kind == "image" || kind == "file") return T(L"Attached to conversation");
    if (kind == "mcp") return T(L"MCP tool called");
    if (kind == "web" || kind == "web_source" || kind == "url_citation") return T(L"Web source");
    if (kind == "web_open") return T(L"Page opened");
    if (kind == "web_find") return T(L"Page content searched");
    if (kind == "web_search") return T(L"Web search performed");
    return T(L"Tool used");
}
wxString Count(const wchar_t* message, uint64_t count) {
    return wxString(i18n::Format(message, {{L"count", std::to_wstring(count)}}));
}
wxString RelativeTime(uint64_t timestamp) {
    if (!timestamp) return {};
    const uint64_t now = UnixTimeSeconds(), seconds = now > timestamp ? now - timestamp : 0;
    if (seconds < 60) return T(L"Just now");
    if (seconds < 3600) return Count(L"{count} min ago", seconds / 60);
    if (seconds < 86400) return Count(L"{count} hr ago", seconds / 3600);
    const uint64_t days = seconds / 86400;
    return Count(days == 1 ? L"{count} day ago" : L"{count} days ago", days);
}
wxString Effort(const std::string& effort) {
    const std::pair<const char*, const wchar_t*> labels[] = {
        {"none", L"None"}, {"minimal", L"Minimal"}, {"low", L"Low"}, {"medium", L"Medium"},
        {"high", L"High"}, {"xhigh", L"Extra high"}, {"max", L"Maximum"}, {"ultra", L"Ultra"}};
    for (const auto& label : labels) if (effort == label.first) return T(label.second);
    return U(effort);
}
void Label(wxDC& dc, const wxString& text, wxRect rect, wxColour color, int alignment = wxALIGN_LEFT) {
    if (rect.width <= 0 || rect.height <= 0) return;
    wxDCClipper clip(dc, rect);
    dc.SetTextForeground(color);
    dc.DrawLabel(wxControl::Ellipsize(text, dc, wxELLIPSIZE_END, rect.width),
                 rect, alignment | wxALIGN_CENTER_VERTICAL);
}
void WrappedDetail(wxDC& dc, const wxString& value, wxRect rect, wxColour color) {
    if (rect.width <= 0 || rect.height <= 0) return;
    wxDCClipper clip(dc, rect);
    dc.SetTextForeground(color);

    wxString remaining = value;
    wxString lines[2];
    const auto take_line = [&](wxString& input) {
        if (input.empty()) return wxString{};
        const size_t newline = input.find('\n');
        const wxString paragraph = newline == wxString::npos ? input : input.Left(newline);
        const wxString after_newline = newline == wxString::npos
            ? wxString{} : input.Mid(newline + 1);
        if (paragraph.empty() && newline != wxString::npos) {
            input = after_newline;
            return wxString{};
        }
        wxArrayInt widths;
        size_t fit = 0;
        if (dc.GetPartialTextExtents(paragraph, widths)) {
            for (const int width : widths) {
                if (width > rect.width) break;
                ++fit;
            }
        } else {
            size_t low = 0, high = paragraph.length();
            while (low < high) {
                const size_t middle = low + (high - low + 1) / 2;
                if (dc.GetTextExtent(paragraph.Left(middle)).x <= rect.width) low = middle;
                else high = middle - 1;
            }
            fit = low;
        }
        if (fit >= paragraph.length()) {
            wxString line = paragraph;
            input = after_newline;
            return line;
        }
        fit = std::max<size_t>(1, fit);
        size_t cut = fit;
        const auto space = paragraph.Left(fit).find_last_of(" \t");
        if (space != wxString::npos && space > 0) cut = space;
        wxString line = paragraph.Left(cut);
        input = paragraph.Mid(cut);
        while (!input.empty() && (input[0] == ' ' || input[0] == '\t')) input.erase(0, 1);
        if (newline != wxString::npos) {
            input += '\n';
            input += after_newline;
        }
        return line;
    };

    lines[0] = take_line(remaining);
    lines[1] = take_line(remaining);
    if (!remaining.empty())
        lines[1] = wxControl::Ellipsize(lines[1] + L"\u2026", dc, wxELLIPSIZE_END, rect.width);

    const int line_height = std::max(1, dc.GetCharHeight());
    dc.DrawText(lines[0], rect.x, rect.y);
    if (!lines[1].empty()) dc.DrawText(lines[1], rect.x, rect.y + line_height);
}
void Avatar(wxDC& dc, const wxRect& rect, const std::string& id) {
    uint32_t hash = 2166136261u;
    for (const unsigned char value : id) hash = (hash ^ value) * 16777619u;
    const wxColour colors[] = {wxColour(218,180,44), wxColour(78,168,179),
                               wxColour(159,119,212), wxColour(75,164,135)};
    dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush(colors[hash % 4]));
    const wxPoint center(rect.x + rect.width / 2, rect.y + rect.height / 2);
    const int radius = std::max(2, std::min(rect.width, rect.height) / 2 - 1);
    const auto point = [&](double angle, double length) {
        return center + wxPoint(static_cast<int>(std::lround(std::cos(angle) * length)),
                                static_cast<int>(std::lround(std::sin(angle) * length)));
    };
    const auto shape = (hash >> 8) % 4;
    if (shape == 0) {
        wxPoint star[16];
        for (int i = 0; i < 16; ++i) star[i] = point(i * 3.141592653589793 / 8, radius * (i % 2 ? 0.4 : 1));
        dc.DrawPolygon(16, star);
    } else if (shape == 1 || shape == 2) {
        const int petal = std::max(1, radius / 3), count = shape == 1 ? 7 : 6;
        for (int i = 0; i < count; ++i) dc.DrawCircle(point(i * 6.283185307179586 / count, radius - petal), petal);
        dc.DrawCircle(center, petal);
    } else {
        const wxPoint triangles[] = {center + wxPoint(-radius,-radius), center + wxPoint(radius,-radius), center,
                                    center, center + wxPoint(radius,radius), center + wxPoint(-radius,radius)};
        dc.DrawPolygon(3, triangles); dc.DrawPolygon(3, triangles + 3);
    }
}
}  // namespace

class NativeSummaryPanel::Rows : public wxPanel {
public:
    explicit Rows(NativeSummaryPanel* owner)
        : wxPanel(owner, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS | wxBORDER_NONE),
          owner_(owner) {
        SetName("spark.summary.rows");
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(1, 1));
        Bind(wxEVT_PAINT, &Rows::Paint, this);
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
            const wxRect thumb = ScrollThumb();
            wxRect target = thumb;
            target.x -= FromDIP(5);
            target.width += FromDIP(5);
            if (!thumb.IsEmpty() && target.Contains(event.GetPosition())) {
                dragging_ = true;
                drag_offset_ = event.GetY() - thumb.y;
                CaptureMouse();
                return;
            }
            pressed_ = Hit(event.GetPosition());
            if (pressed_ != wxNOT_FOUND) SetFocus();
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
            const int hit = Hit(event.GetPosition()), pressed = pressed_;
            pressed_ = wxNOT_FOUND;
            if (dragging_) {
                dragging_ = false;
                if (HasCapture()) ReleaseMouse();
                Refresh(false);
                return;
            }
            if (hit >= 0 && hit == pressed) owner_->ActivateRow(static_cast<size_t>(hit));
        });
        Bind(wxEVT_LEFT_DCLICK, [](wxMouseEvent&) {});
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
            dragging_ = false;
            pressed_ = wxNOT_FOUND;
            Refresh(false);
        });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            if (dragging_) {
                const wxRect thumb = ScrollThumb();
                const int track = GetClientSize().y - thumb.height;
                const int top = std::clamp(event.GetY() - drag_offset_, 0, std::max(0, track));
                const int range = std::max(0, content_height_ - GetClientSize().y);
                ScrollToOffset(track > 0 ? static_cast<int>(static_cast<int64_t>(top) * range / track) : 0);
                return;
            }
            event.Skip();
        });
        Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) {
            const int delta = event.GetWheelDelta();
            if (delta != 0) {
                const int steps = event.GetWheelRotation() / delta;
                ScrollToOffset(ScrollOffset() - steps * FromDIP(48));
            }
        });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            scroll_offset_ = std::clamp(scroll_offset_, 0,
                std::max(0, content_height_ - GetClientSize().y));
            Refresh(false);
            event.Skip();
        });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
            const int code = event.GetKeyCode();
            if (code == WXK_ESCAPE) {
                owner_->Close();
            } else if (code == WXK_DOWN || code == WXK_NUMPAD_DOWN) {
                ScrollToOffset(ScrollOffset() + FromDIP(48));
            } else if (code == WXK_UP || code == WXK_NUMPAD_UP) {
                ScrollToOffset(ScrollOffset() - FromDIP(48));
            } else if (code == WXK_PAGEDOWN || code == WXK_NUMPAD_PAGEDOWN) {
                ScrollToOffset(ScrollOffset() + FromDIP(240));
            } else if (code == WXK_PAGEUP || code == WXK_NUMPAD_PAGEUP) {
                ScrollToOffset(ScrollOffset() - FromDIP(240));
            }
        });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            const int hit = Hit(event.GetPosition());
            if (hit != hovered_) { hovered_ = hit; Refresh(false); }
            event.Skip();
        });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
            hovered_ = wxNOT_FOUND;
            if (!dragging_) pressed_ = wxNOT_FOUND;
            Refresh(false); event.Skip();
        });
    }
    bool InteractionActive() const { return dragging_ || pressed_ != wxNOT_FOUND; }
    void SetRowsCount(size_t count) {
        size_t anchor = 0;
        int within_row = 0;
        if (row_offsets_.size() > 1) {
            const auto next = std::upper_bound(row_offsets_.begin(), row_offsets_.end(), scroll_offset_);
            anchor = std::min(row_offsets_.size() - 2,
                static_cast<size_t>(std::max<std::ptrdiff_t>(0, next - row_offsets_.begin() - 1)));
            within_row = scroll_offset_ - row_offsets_[anchor];
        }
        row_offsets_.assign(count + 1, FromDIP(8));
        for (size_t index = 0; index < count; ++index)
            row_offsets_[index + 1] = row_offsets_[index] +
                FromDIP(owner_->RowHeight(owner_->display_rows_[index]));
        content_height_ = row_offsets_.back() + FromDIP(4);
        if (count > 0 && scroll_offset_ != 0) {
            anchor = std::min(anchor, count - 1);
            const int row_height = row_offsets_[anchor + 1] - row_offsets_[anchor];
            scroll_offset_ = row_offsets_[anchor] + std::min(within_row, std::max(0, row_height - 1));
        } else {
            scroll_offset_ = 0;
        }
        scroll_offset_ = std::clamp(scroll_offset_, 0,
            std::max(0, content_height_ - GetClientSize().y));
        hovered_ = pressed_ = wxNOT_FOUND;
        Refresh(false);
    }
    void ScrollToRow(size_t row) {
        if (row_offsets_.size() <= 1) return;
        scroll_offset_ = std::clamp(row == 0 ? 0 : row_offsets_[std::min(row, row_offsets_.size() - 1)], 0,
            std::max(0, content_height_ - GetClientSize().y));
        Refresh(false);
    }
private:
    int ScrollOffset() const { return scroll_offset_; }
    wxRect ScrollThumb() const {
        const int viewport = GetClientSize().y;
        if (viewport <= 0 || content_height_ <= viewport) return {};
        const int height = std::min(viewport, std::max(FromDIP(28),
            static_cast<int>(static_cast<int64_t>(viewport) * viewport / content_height_)));
        const int track = viewport - height;
        const int range = content_height_ - viewport;
        const int top = track > 0 ? static_cast<int>(static_cast<int64_t>(ScrollOffset()) * track / range) : 0;
        return {GetClientSize().x - FromDIP(7), top, FromDIP(4), height};
    }
    void ScrollToOffset(int offset) {
        const int target = std::clamp(offset, 0, std::max(0, content_height_ - GetClientSize().y));
        if (scroll_offset_ == target) return;
        scroll_offset_ = target;
        Refresh(false);
    }
    int Hit(wxPoint point) const {
        if (point.x < FromDIP(12) || point.x >= GetClientSize().x - FromDIP(12)) return wxNOT_FOUND;
        const int y = point.y + scroll_offset_;
        if (y < 0 || y >= content_height_) return wxNOT_FOUND;
        const auto next = std::upper_bound(row_offsets_.begin(), row_offsets_.end(), y);
        if (next == row_offsets_.begin() || next == row_offsets_.end()) return wxNOT_FOUND;
        return static_cast<int>(next - row_offsets_.begin() - 1);
    }
    void Paint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        const auto p = NativeColors(owner_->dark_);
        dc.SetBackground(wxBrush(p.background));
        dc.Clear();
        if (row_offsets_.size() > 1) {
            const auto first = std::upper_bound(row_offsets_.begin(), row_offsets_.end(), scroll_offset_);
            size_t index = first == row_offsets_.begin() ? 0 :
                static_cast<size_t>(first - row_offsets_.begin() - 1);
            const int bottom = scroll_offset_ + GetClientSize().y;
            for (; index < owner_->display_rows_.size() && row_offsets_[index] < bottom; ++index) {
                const wxRect bounds(0, row_offsets_[index] - scroll_offset_, GetClientSize().x,
                                    row_offsets_[index + 1] - row_offsets_[index]);
                DrawBackground(dc, bounds, index);
                wxDCClipper clip(dc, bounds);
                DrawItem(dc, bounds, index);
            }
        }
        const wxRect thumb = ScrollThumb();
        if (!thumb.IsEmpty()) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(owner_->dark_ ? wxColour(74, 76, 82) : wxColour(180, 183, 188)));
            dc.DrawRoundedRectangle(thumb, FromDIP(2));
        }
    }
    void DrawBackground(wxDC& dc, const wxRect& rect, size_t index) const {
        const auto p = NativeColors(owner_->dark_);
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(p.background));
        dc.DrawRectangle(rect);
        const auto& row = owner_->display_rows_[index];
        if (static_cast<int>(index) == hovered_ &&
            (row.toggle || (row.kind != RowKind::Section && row.kind != RowKind::Empty && row.kind != RowKind::Divider))) {
            auto target = rect; target.Deflate(FromDIP(12), 0);
            dc.SetBrush(wxBrush(p.selection)); dc.DrawRectangle(target);
        }
    }
    void DrawItem(wxDC& dc, const wxRect& bounds, size_t index) const {
        const auto p = NativeColors(owner_->dark_);
        dc.SetFont(GetFont());
        wxRect rect = bounds; rect.Deflate(FromDIP(12), 0);
        const int right = rect.x + rect.width;
        const auto area = [&](int x, int y, int w, int h) {
            return wxRect(rect.x + FromDIP(x), rect.y + FromDIP(y),
                          std::max(0, rect.width - FromDIP(x + w)), FromDIP(h));
        };
        const auto& row = owner_->display_rows_[index];
        if (row.kind == RowKind::Agent) {
            const auto& agent = owner_->agents_[row.item];
            const int top = rect.y + (rect.height - FromDIP(20)) / 2;
            Avatar(dc, wxRect(rect.x + FromDIP(12), top, FromDIP(20), FromDIP(20)), agent.id);
            Label(dc, row.title, area(46, 0, owner_->popup_ ? 12 : 85, owner_->popup_ ? 36 : 46), p.text);
            if (!owner_->popup_) Label(dc, RelativeTime(agent.state == SubAgentState::Running ? agent.started_at : agent.finished_at),
                wxRect(right - FromDIP(84), rect.y, FromDIP(76), rect.height), p.muted, wxALIGN_RIGHT);
            return;
        }
        if (row.kind != RowKind::Source) {
            int left = rect.x + FromDIP(12);
            if (row.kind == RowKind::Group) for (size_t i = 0; i < std::min<size_t>(4, owner_->agent_order_.size()); ++i) {
                Avatar(dc, wxRect(left, rect.y + (rect.height - FromDIP(20)) / 2, FromDIP(20), FromDIP(20)),
                       owner_->agents_[owner_->agent_order_[i]].id);
                left += FromDIP(28);
            }
            if (row.kind == RowKind::Divider) {
                dc.SetPen(wxPen(p.border));
                dc.DrawLine(FromDIP(16), rect.y + rect.height / 2, GetClientSize().x - FromDIP(16), rect.y + rect.height / 2);
                return;
            }
            if (row.toggle) {
                const int x = std::min(right - FromDIP(20), left + dc.GetTextExtent(row.title).x + FromDIP(12));
                const int y = rect.y + rect.height / 2, direction = owner_->agents_expanded_ ? -1 : 1;
                dc.SetPen(wxPen(p.muted));
                dc.DrawLine(x - FromDIP(4), y - direction * FromDIP(2), x, y + direction * FromDIP(2));
                dc.DrawLine(x, y + direction * FromDIP(2), x + FromDIP(4), y - direction * FromDIP(2));
            }
            Label(dc, row.title, wxRect(left, rect.y, std::max(0, right - FromDIP(12) - left), rect.height),
                  row.kind == RowKind::More || row.kind == RowKind::SourceLink ? p.text : p.muted);
            return;
        }
        auto& item = owner_->sources_[row.item];
        const auto& source = item.reference;
        const int edge = std::clamp(FromDIP(28), 1, kMaxThumbnailPixels);
        if (!item.thumbnail_attempted && source.kind == "image") {
            item.thumbnail_attempted = true;
            wxBitmap bitmap;
            if (item.actionable && owner_->callbacks_.thumbnail) {
                try { bitmap = owner_->callbacks_.thumbnail(source, edge); } catch (...) {}
            }
            // Reject before ConvertToImage: the panel never decodes original files.
            if (bitmap.IsOk() && bitmap.GetWidth() > 0 && bitmap.GetHeight() > 0 &&
                bitmap.GetWidth() <= edge && bitmap.GetHeight() <= edge) {
                auto image = bitmap.ConvertToImage();
                if (image.IsOk()) {
                    const double ratio = static_cast<double>(edge) / std::max(bitmap.GetWidth(), bitmap.GetHeight());
                    item.thumbnail = wxBitmap(image.Rescale(std::max(1, static_cast<int>(image.GetWidth() * ratio)),
                        std::max(1, static_cast<int>(image.GetHeight() * ratio)), wxIMAGE_QUALITY_HIGH));
                }
            }
        }
        const wxRect icon(rect.x + FromDIP(10), rect.y + FromDIP(14), edge, edge);
        if (item.thumbnail.IsOk()) dc.DrawBitmap(item.thumbnail, icon.GetPosition(), true);
        else Label(dc, Web(source.kind) ? wxString(L"\u25ce") : wxString(L"\u25a4"), icon, p.muted, wxALIGN_CENTER);
        auto title = source.label.empty() ? source.kind : source.label;
        if (source.kind == "image" || source.kind == "file") {
            const auto slash = source.detail.find_last_of("/\\");
            if (!source.detail.empty()) title = source.detail.substr(slash == std::string::npos ? 0 : slash + 1);
        }
        Label(dc, U(title), area(50, 10, 10, 24), p.text);
        WrappedDetail(dc, UDetail(source.detail), area(50, 36, 12, 39), p.muted);
        Label(dc, SourceNote(source.kind), area(50, 81, 12, 23), p.muted);
        dc.SetPen(wxPen(p.border));
        const int bottom = rect.y + rect.height - FromDIP(1);
        dc.DrawLine(rect.x + FromDIP(4), bottom, right - FromDIP(4), bottom);
    }
    NativeSummaryPanel* owner_;
    std::vector<int> row_offsets_;
    int content_height_ = 0, scroll_offset_ = 0;
    int drag_offset_ = 0;
    int hovered_ = wxNOT_FOUND, pressed_ = wxNOT_FOUND;
    bool dragging_ = false;
};

NativeSummaryPanel::NativeSummaryPanel(wxWindow* parent, Callbacks callbacks)
    : wxPanel(parent, wxID_ANY), callbacks_(std::move(callbacks)) {
    SetName("spark.summary"); SetMinSize(FromDIP(wxSize(360, -1)));
    auto* root = new wxBoxSizer(wxVERTICAL);
    header_ = new wxPanel(this); header_->SetMinSize(FromDIP(wxSize(1, 44)));
    header_->SetBackgroundStyle(wxBG_STYLE_PAINT);
    auto* header = new wxBoxSizer(wxHORIZONTAL);
    const auto button = [this](wxWindow* parent, const wxString& label, int width, bool tab) {
        auto* value = new NativeButton(parent, label);
        value->SetMinSize(FromDIP(wxSize(width, 36)));
        value->Bind(wxEVT_PAINT, [this, value, tab](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(value); const auto p = NativeColors(dark_);
            dc.SetBackground(wxBrush(p.background)); dc.Clear(); dc.SetFont(value->GetFont());
            const bool hover = value->GetClientRect().Contains(value->ScreenToClient(wxGetMousePosition()));
            if (tab && (value->IsSelected() || hover)) {
                dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush(p.selection)); dc.DrawRectangle(value->GetClientRect());
            }
            Label(dc, value->GetLabel(), value->GetClientRect(), tab && value->IsSelected() ? p.text : p.muted, wxALIGN_CENTER);
        });
        return value;
    };
    agents_tab_ = button(header_, T(L"Subagents"), 106, true);
    sources_tab_ = button(header_, T(L"Sources"), 106, true);
    agents_tab_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShowAgents(); });
    sources_tab_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShowSources(); });
    header->AddSpacer(FromDIP(10));
    header->Add(agents_tab_, 0, wxTOP | wxBOTTOM, FromDIP(4));
    header->AddSpacer(FromDIP(6)); header->Add(sources_tab_, 0, wxTOP | wxBOTTOM, FromDIP(4));
    header->AddStretchSpacer();
    auto* close = button(header_, wxString(L"\u00d7"), 32, false);
    close->SetToolTip(T(L"Close"));
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
    header->Add(close, 0, wxTOP | wxBOTTOM, FromDIP(4));
    header->AddSpacer(FromDIP(6)); header_->SetSizer(header);
    header_->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(header_); const auto p = NativeColors(dark_);
        dc.SetBackground(wxBrush(p.background)); dc.Clear(); dc.SetPen(wxPen(p.border));
        dc.DrawLine(FromDIP(16), FromDIP(43), header_->GetClientSize().x - FromDIP(16), FromDIP(43));
    });
    root->Add(header_, 0, wxEXPAND);
    detail_ = new wxPanel(this); detail_->SetMinSize(FromDIP(wxSize(1, 44)));
    detail_->SetBackgroundStyle(wxBG_STYLE_PAINT);
    detail_->Bind(wxEVT_PAINT, &NativeSummaryPanel::PaintDetail, this);
    auto* detail = new wxBoxSizer(wxHORIZONTAL);
    auto* back = button(detail_, wxString(L"\u2190"), 32, false);
    back->SetToolTip(T(L"Back"));
    back->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShowAgents(); });
    detail->AddSpacer(FromDIP(8)); detail->Add(back, 0, wxTOP | wxBOTTOM, FromDIP(4));
    detail_->SetSizer(detail); root->Add(detail_, 0, wxEXPAND);
    rows_ = new Rows(this); root->Add(rows_, 1, wxEXPAND);
    chat_ = new NativeChatSurface(this); root->Add(chat_, 1, wxEXPAND);
    SetSizer(root); SetDark(true); RefreshView();
}
NativeSummaryPanel::~NativeSummaryPanel() = default;

void NativeSummaryPanel::SetDark(bool dark) {
    dark_ = dark; ApplyNativeColors(this, dark); chat_->SetDark(dark);
}
int NativeSummaryPanel::RowHeight(const Row& row) const {
    switch (row.kind) {
        case RowKind::Source: return 112;
        case RowKind::Agent: return popup_ ? 36 : 46;
        case RowKind::Section: return 40;
        case RowKind::Divider: return 12;
        case RowKind::Empty: case RowKind::More: case RowKind::Group: case RowKind::SourceLink: return 38;
    }
    return 38;
}
void NativeSummaryPanel::RebuildRows() {
    display_rows_.clear(); agent_order_.clear();
    std::vector<size_t> running, finished;
    for (size_t i = 0; i < agents_.size(); ++i)
        (agents_[i].state == SubAgentState::Running ? running : finished).push_back(i);
    const auto sort = [this](size_t a, size_t b) {
        return agents_[a].started_at != agents_[b].started_at ? agents_[a].started_at > agents_[b].started_at : agents_[a].id < agents_[b].id;
    };
    std::sort(running.begin(), running.end(), sort); std::sort(finished.begin(), finished.end(), sort);
    agent_order_ = running; agent_order_.insert(agent_order_.end(), finished.begin(), finished.end());
    const auto add_agent = [this](size_t index) {
        const auto& agent = agents_[index]; wxString name = U(agent.name);
        if (!popup_ && (agent.state == SubAgentState::Failed || agent.state == SubAgentState::Cancelled))
            name = wxString(i18n::Format(agent.state == SubAgentState::Failed ? L"{name} \u00b7 Failed" : L"{name} \u00b7 Stopped",
                                        {{L"name", name.ToStdWstring()}}));
        display_rows_.push_back({RowKind::Agent, name, index});
    };
    if (popup_) {
        if (!agent_order_.empty()) {
            const bool grouped = agent_order_.size() > 4;
            display_rows_.push_back({RowKind::Section, T(L"Subagents"), 0, grouped});
            if (!grouped || agents_expanded_) for (const auto index : agent_order_) add_agent(index);
            else {
                wxString counts;
                const std::pair<SubAgentState, const wchar_t*> states[] = {
                    {SubAgentState::Running, L"{count} running"}, {SubAgentState::Completed, L"{count} completed"},
                    {SubAgentState::Failed, L"{count} failed"}, {SubAgentState::Cancelled, L"{count} stopped"}};
                for (const auto& state : states) {
                    const auto count = std::count_if(agents_.begin(), agents_.end(), [&](const Agent& agent) { return agent.state == state.first; });
                    if (count) { if (!counts.empty()) counts += L" \u00b7 "; counts += Count(state.second, count); }
                }
                display_rows_.push_back({RowKind::Group, counts});
            }
            display_rows_.push_back({RowKind::Divider, {}});
        }
        display_rows_.push_back({RowKind::Section, T(L"Sources")});
        bool web = false; size_t images = 0, other = 0;
        for (const auto& source : sources_) {
            if (Web(source.reference.kind)) web = true;
            else if (source.reference.kind == "image") ++images;
            else ++other;
        }
        if (web) display_rows_.push_back({RowKind::SourceLink, wxString(L"\u25ce  ") + T(L"Web search")});
        if (images) display_rows_.push_back({RowKind::SourceLink, Count(L"{count} images", images)});
        if (other) display_rows_.push_back({RowKind::SourceLink, Count(L"{count} tool sources", other)});
        display_rows_.push_back({RowKind::SourceLink, T(L"View all")});
    } else if (page_ == Page::Sources) {
        for (size_t i = 0; i < sources_.size(); ++i) display_rows_.push_back({RowKind::Source, {}, i});
        if (sources_.empty()) display_rows_.push_back({RowKind::Empty, T(L"No sources yet")});
    } else {
        display_rows_.push_back({RowKind::Section, Count(L"Active \u00b7 {count}", running.size())});
        for (const auto index : running) add_agent(index);
        if (running.empty()) display_rows_.push_back({RowKind::Empty, T(L"No active subagents")});
        display_rows_.push_back({RowKind::Section, Count(L"Finished \u00b7 {count}", finished.size())});
        const size_t shown = show_all_ ? finished.size() : std::min<size_t>(10, finished.size());
        for (size_t i = 0; i < shown; ++i) add_agent(finished[i]);
        if (shown < finished.size()) display_rows_.push_back({RowKind::More, Count(L"Show {count} more", finished.size() - shown)});
        if (finished.empty()) display_rows_.push_back({RowKind::Empty, T(L"No finished subagents")});
    }
}
void NativeSummaryPanel::SetPopupMode(bool popup) {
    popup_ = popup; agents_expanded_ = false;
    SetMinSize(FromDIP(wxSize(popup ? 1 : 360, -1)));
    Select(Page::Agents);
}
void NativeSummaryPanel::ResetOverview() {
    show_all_ = false; agents_expanded_ = false; RefreshView();
}
bool NativeSummaryPanel::InteractionActive() const { return rows_->InteractionActive(); }
wxSize NativeSummaryPanel::PopupSize() const {
    int height = 20;
    for (const auto& row : display_rows_) height += RowHeight(row);
    return FromDIP(wxSize(320, height));
}
void NativeSummaryPanel::SetSources(const std::vector<SourceReference>& sources) {
    std::vector<Source> next;
    next.reserve(std::min(sources.size(), kMaxSources));
    std::set<std::pair<std::string, std::string>> seen;
    for (size_t i = 0; i < std::min(sources.size(), kMaxSources); ++i) {
        const auto& source = sources[i];
        if (source.kind == "subagent") continue;
        Source item;
        item.reference = {Bound(source.kind, 64), Bound(source.label, 2048), Bound(source.detail, 4096)};
        item.actionable = source.kind.size() <= 64 && source.detail.size() <= 4096;
        const auto& ref = item.reference;
        const bool web_url = ref.kind == "url_citation" || ref.kind == "web_source" ||
                             ref.kind == "web_open" || ref.kind == "web_find" || ref.kind == "web";
        const auto target = ref.kind == "image" || ref.kind == "file" ? NormalizePathForComparison(ref.detail) : ref.detail;
        if (seen.emplace(web_url ? "web" : ref.kind, target).second) next.push_back(std::move(item));
    }
    if (next.size() == sources_.size() && std::equal(next.begin(), next.end(), sources_.begin(),
        [](const Source& a, const Source& b) {
            return std::tie(a.reference.kind, a.reference.label, a.reference.detail, a.actionable) ==
                   std::tie(b.reference.kind, b.reference.label, b.reference.detail, b.actionable);
        })) {
        bool retry = false;
        for (auto& source : sources_) if (source.reference.kind == "image" && source.actionable &&
                                        source.thumbnail_attempted && !source.thumbnail.IsOk()) {
            source.thumbnail_attempted = false; retry = true;
        }
        if (retry && page_ == Page::Sources) rows_->Refresh(false);
        return;
    }
    sources_ = std::move(next);
    RefreshView();
}
void NativeSummaryPanel::RefreshThumbnails() {
    for (auto& source : sources_) source.thumbnail_attempted = false;
    if (!popup_ && page_ == Page::Sources && rows_->IsShownOnScreen()) rows_->Refresh(false);
}
void NativeSummaryPanel::SetAgents(const std::vector<SubAgentInfo>& agents) {
    std::vector<Agent> next;
    next.reserve(std::min(agents.size(), kMaxAgents));
    for (size_t i = 0; i < std::min(agents.size(), kMaxAgents); ++i) {
        if (agents[i].id.empty() || agents[i].id.size() > 128) continue;
        auto found = std::find_if(next.begin(), next.end(), [&](const Agent& a) { return a.id == agents[i].id; });
        if (found == next.end()) next.emplace_back(agents[i]); else *found = Agent(agents[i]);
    }
    std::sort(next.begin(), next.end(), [](const Agent& a, const Agent& b) { return a.id < b.id; });
    if (next == agents_) {
        if (!popup_ && page_ == Page::Agents) rows_->Refresh(false);
        return;
    }
    agents_ = std::move(next);
    if (page_ == Page::Child && std::none_of(agents_.begin(), agents_.end(),
        [this](const Agent& agent) { return agent.id == active_child_; })) ShowAgents();
    else RefreshView();
}
bool NativeSummaryPanel::AddAgentTab(const SubAgentInfo& info) {
    if (info.id.empty() || info.id.size() > 128) return false;
    auto found = std::find_if(agents_.begin(), agents_.end(), [&](const Agent& agent) { return agent.id == info.id; });
    if (found == agents_.end()) {
        if (agents_.size() >= kMaxAgents) return false;
        agents_.emplace_back(info);
    } else {
        const Agent next(info);
        if (*found == next) return true;
        *found = next;
    }
    std::sort(agents_.begin(), agents_.end(), [](const Agent& a, const Agent& b) { return a.id < b.id; });
    RefreshView(); return true;
}
NativeSummaryPanel::Agent::Agent(const SubAgentInfo& info)
    : id(info.id), name(Bound(info.name, 120)), provider(Bound(info.provider, 256)),
      model(Bound(info.model, 256)), effort(Bound(info.reasoning_effort, 64)),
      state(info.state), started_at(info.started_at), finished_at(info.finished_at) {
    switch (info.state) {
        case SubAgentState::Running: status = T(L"Running"); break;
        case SubAgentState::Completed: status = T(L"Completed"); break;
        case SubAgentState::Failed: status = T(L"Failed"); break;
        case SubAgentState::Cancelled: status = T(L"Stopped"); break;
    }
}
bool NativeSummaryPanel::Agent::operator==(const Agent& other) const {
    return std::tie(id, name, provider, model, effort, state, started_at, finished_at) ==
           std::tie(other.id, other.name, other.provider, other.model, other.effort,
                    other.state, other.started_at, other.finished_at);
}
bool NativeSummaryPanel::SelectAgentTab(const std::string& id) {
    const auto found = std::find_if(agents_.begin(), agents_.end(), [&](const Agent& agent) { return agent.id == id; });
    if (found == agents_.end()) return false;
    if (popup_) {
        const auto callback = callbacks_.active_child; const auto selected = found->id;
        Hide(); if (callback) callback(selected); return true;
    }
    if (page_ != Page::Child || active_child_ != found->id) Select(Page::Child, found->id);
    return true;
}
void NativeSummaryPanel::ShowAgents() { if (page_ != Page::Agents) Select(Page::Agents); }
void NativeSummaryPanel::ShowSources() { if (page_ != Page::Sources) Select(Page::Sources); }
void NativeSummaryPanel::Select(Page page, const std::string& id) {
    const std::string next = page == Page::Child ? id : std::string{};
    const bool changed = active_child_ != next, page_changed = page_ != page;
    if (changed) { chat_->SetLoading(0); chat_->SetTranscript(nullptr, true); }
    active_child_ = next; page_ = page;
    RefreshView();
    if (page_changed) rows_->ScrollToRow(0);
    const auto callback = callbacks_.active_child;
    if (changed && callback) callback(next);
}
void NativeSummaryPanel::Close() {
    const auto closed = callbacks_.close;
    const auto changed = callbacks_.active_child;
    const bool had_child = !active_child_.empty();
    chat_->SetLoading(0); chat_->SetTranscript(nullptr, true);
    active_child_.clear(); page_ = Page::Agents; Hide(); RefreshView();
    if (had_child && changed) changed({});
    if (closed) closed();
}
void NativeSummaryPanel::RefreshView() {
    RebuildRows(); header_->Show(!popup_);
    agents_tab_->SetSelected(page_ != Page::Sources); sources_tab_->SetSelected(page_ == Page::Sources);
    detail_->Show(!popup_ && page_ == Page::Child); chat_->Show(!popup_ && page_ == Page::Child);
    rows_->Show(popup_ || page_ != Page::Child);
    rows_->SetRowsCount(display_rows_.size()); Layout();
    detail_->Refresh(false); rows_->UnsetToolTip(); rows_->Refresh(false);
}
void NativeSummaryPanel::PaintDetail(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(detail_); const auto p = NativeColors(dark_);
    dc.SetBackground(wxBrush(p.background)); dc.Clear(); dc.SetFont(GetFont());
    const int width = detail_->GetClientSize().x, model_width = std::min(FromDIP(190), width * 42 / 100);
    for (const auto& agent : agents_) {
        if (agent.id != active_child_) continue;
        Avatar(dc, wxRect(FromDIP(42), FromDIP(12), FromDIP(20), FromDIP(20)), agent.id);
        Label(dc, U(agent.name), wxRect(FromDIP(70), FromDIP(4), width - model_width - FromDIP(82), FromDIP(36)), p.text);
        const auto model = (agent.provider.empty() ? wxString{} : U(agent.provider) + " / ") + U(agent.model);
        const auto meta = model +
            (agent.effort.empty() ? wxString{} : wxString(L" \u00b7 ") + Effort(agent.effort));
        Label(dc, meta, wxRect(width - model_width, FromDIP(4), model_width - FromDIP(14), FromDIP(36)), p.muted, wxALIGN_RIGHT);
        detail_->SetToolTip(U(agent.name) + L" \u00b7 " + agent.status + "\n" + meta);
        break;
    }
    dc.SetPen(wxPen(p.border));
    dc.DrawLine(FromDIP(16), FromDIP(43), width - FromDIP(16), FromDIP(43));
}
void NativeSummaryPanel::ActivateRow(size_t index) {
    if (index >= display_rows_.size()) return;
    const auto row = display_rows_[index];
    if (row.toggle || row.kind == RowKind::Group) {
        agents_expanded_ = !agents_expanded_; RefreshView();
        const auto callback = callbacks_.popup_size_changed; const auto size = PopupSize();
        if (callback) callback(size); return;
    }
    if (row.kind == RowKind::More) { show_all_ = true; RefreshView(); return; }
    if (row.kind == RowKind::Agent) { SelectAgentTab(agents_[row.item].id); return; }
    if (row.kind == RowKind::SourceLink) {
        const auto callback = callbacks_.show_sources;
        Hide(); if (callback) callback(); return;
    }
    if (row.kind != RowKind::Source || !sources_[row.item].actionable) return;
    const auto callback = callbacks_.open_source;
    const auto source = sources_[row.item].reference;
    if (callback) callback(source);
}

}  // namespace spark
