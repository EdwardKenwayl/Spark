#include "gui/native/chat_surface.h"
#include "platform/clock.h"

#include <wx/dc.h>
#include <algorithm>
#include <cwctype>

namespace spark {

void NativeChatSurface::LayoutText(wxDC& dc, Row& row, const std::vector<markdown::MarkdownLine>& lines,
                                  int left, int width, int& top, bool muted) {
    for (const auto& line : lines) {
        const int indent = std::min(width / 3, line.indent * FromDIP(18));
        int x = left + indent, line_height = FromDIP(24);
        const auto next = [&] { top += line_height; x = left + indent; line_height = FromDIP(24); };
        for (const auto& run : line.runs) {
            const wxFont font = line.code ? mono_ : body_;
            dc.SetFont(font);
            const wxString text(run.text);
            size_t offset = 0;
            while (offset < text.length()) {
                const int remaining = std::max(8, left + width - x);
                size_t low = 0, high = text.length() - offset;
                while (low < high) {
                    const size_t mid = (low + high + 1) / 2;
                    if (dc.GetTextExtent(text.Mid(offset, mid)).x <= remaining) low = mid;
                    else high = mid - 1;
                }
                size_t length = low;
                if (length == 0 && x > left + indent) { next(); continue; }
                length = std::max<size_t>(1, length);
                if (offset + length < text.length() && !line.code) {
                    const auto segment = text.Mid(offset, length);
                    const int space = segment.Find(' ', true);
                    if (space > 0 && static_cast<size_t>(space) > length / 2) length = static_cast<size_t>(space + 1);
                }
                const wxString fragment = text.Mid(offset, length);
                const wxSize extent = dc.GetTextExtent(fragment);
                row.fragments.push_back({fragment, wxPoint(x, top), font, muted ? Muted() : Foreground()});
                line_height = std::max(line_height, extent.y + FromDIP(5));
                x += extent.x;
                offset += length;
                if (offset < text.length()) next();
            }
        }
        top += line_height;
    }
}

void NativeChatSurface::LayoutMarkdown(wxDC& dc, Row& row,
    const std::vector<markdown::MarkdownLine>& lines, int left, int width, int& top) {
    using Style = markdown::MarkdownStyle;
    const wxColour accent = dark_ ? wxColour(142, 151, 168) : wxColour(70, 78, 92);
    for (const auto& line : lines) {
        const int line_left = left + std::clamp(line.indent * FromDIP(14), 0, std::max(0, width - 1));
        const int right = left + width;
        if (line.rule) {
            row.panels.push_back({wxRect(line_left, top + FromDIP(5), right - line_left, 1), PanelKind::Rule});
            top += FromDIP(12);
            continue;
        }
        if (line.runs.empty()) { top += FromDIP(8); continue; }
        const int start = top;
        int x = line_left, height = FromDIP(18);
        const auto next = [&] { top += height + FromDIP(2); x = line_left; height = FromDIP(18); };
        for (const auto& run : line.runs) {
            wxFont font = line.code || run.style == Style::Code ? mono_ : body_;
            if (!line.code) {
                if (run.style == Style::Bold || run.style == Style::BoldItalic || run.style == Style::Heading)
                    font.MakeBold();
                if (run.style == Style::Italic || run.style == Style::BoldItalic) font.MakeItalic();
                if (run.style == Style::Heading) font.SetPointSize(std::max(1, body_.GetPointSize() * 5 / 4));
            }
            dc.SetFont(font);
            wxCoord text_height = 0, leading = 0;
            dc.GetTextExtent("Ag", nullptr, &text_height, nullptr, &leading);
            const wxColour ink = line.code || run.style == Style::Muted || run.style == Style::ListMarker ? Muted() :
                run.style == Style::Code || run.style == Style::Link ? accent : Foreground();
            const auto append = [&](const wxString& token, int token_width) {
                if (!line.code && run.style == Style::Code)
                    row.panels.push_back({wxRect(x - FromDIP(2), top, token_width + FromDIP(4), height), PanelKind::InlineCode});
                row.fragments.push_back({token, wxPoint(x, top), font, ink});
                x += token_width;
                height = std::max(height, std::max(FromDIP(14), text_height + leading));
            };
            for (size_t begin = 0; begin < run.text.size();) {
                const bool whitespace = std::iswspace(run.text[begin]) != 0;
                size_t end = begin + 1;
                while (end < run.text.size() && (std::iswspace(run.text[end]) != 0) == whitespace) ++end;
                const wxString token(run.text.substr(begin, end - begin));
                const int token_width = dc.GetTextExtent(token).x;
                if (!whitespace && x > line_left && x + token_width > right) next();
                if (!whitespace || x != line_left) {
                    if (!whitespace && token_width > right - line_left && token.length() > 1) {
                        for (size_t i = 0; i < token.length(); ++i) {
                            const auto piece = token.Mid(i, 1);
                            const int piece_width = dc.GetTextExtent(piece).x;
                            if (x > line_left && x + piece_width > right) next();
                            append(piece, piece_width);
                        }
                    } else append(token, token_width);
                }
                begin = end;
            }
        }
        if (line.code)
            row.panels.push_back({wxRect(left, start - FromDIP(2), width, top + height - start + FromDIP(4)), PanelKind::Code});
        top += height + (line.code ? FromDIP(4) : 0);
    }
}

void NativeChatSurface::PaintText(wxDC& dc, const Row& row) {
    const wxColour panel = dark_ ? wxColour(31, 32, 35) : wxColour(247, 247, 248);
    const wxColour accent = dark_ ? wxColour(142, 151, 168) : wxColour(70, 78, 92);
    for (const auto& surface : row.panels) {
        const auto& rect = surface.bounds;
        if (surface.kind == PanelKind::Rule) {
            dc.SetPen(wxPen(dark_ ? wxColour(67, 68, 72) : wxColour(214, 216, 219)));
            dc.DrawLine(rect.x, rect.y, rect.x + rect.width, rect.y);
            continue;
        }
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(panel));
        dc.DrawRectangle(rect);
        if (surface.kind == PanelKind::Code) {
            dc.SetPen(wxPen(accent, FromDIP(2)));
            dc.DrawLine(rect.x, rect.y + FromDIP(2), rect.x, rect.y + rect.height - FromDIP(2));
        }
    }
    for (const auto& part : row.fragments) {
        dc.SetFont(part.font);
        dc.SetTextForeground(row.item == hovered_ && part.highlight ? Foreground() : part.color);
        dc.DrawText(part.text, part.origin);
        if (part.live) {
            const int sweep = static_cast<int>(MonotonicMillis() / 4 % static_cast<uint64_t>(std::max(1, row.bounds.width)));
            wxDCClipper clip(dc, row.bounds.x + sweep - 40, part.origin.y, 80, 26);
            dc.SetTextForeground(Foreground());
            dc.DrawText(part.text, part.origin);
        }
    }
}

}  // namespace spark
