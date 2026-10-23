#include "gui/native/attachment_strip.h"

#include "core/common.h"
#include "gui/localization.h"
#include "gui/native/controls.h"

#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/image.h>

#include <algorithm>
#include <set>

namespace spark {
namespace {
bool Image(const std::string& path) {
    const auto extension = LowerAscii(PathToUtf8(NativePath(path).extension()));
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
           extension == ".gif" || extension == ".bmp" || extension == ".webp";
}
}

NativeAttachmentStrip::NativeAttachmentStrip(wxWindow* parent)
    : wxControl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {
    SetName("spark.attachments");
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &NativeAttachmentStrip::Paint, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { Refresh(false); event.Skip(); });
    Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
        wxString tooltip;
        for (const auto& hit : Hits()) if (hit.tile.Contains(event.GetPosition())) {
            tooltip = hit.close.Contains(event.GetPosition()) ? wxString(i18n::Text(L"Remove attachment"))
                : wxString::FromUTF8(paths_[hit.index]);
            break;
        }
        if (tooltip != GetToolTipText()) SetToolTip(tooltip);
        event.Skip();
    });
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
        press_ = event.GetPosition(); SetFocus(); CaptureMouse();
    });
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { press_ = {-1, -1}; });
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
        const auto down = press_;
        press_ = {-1, -1};
        if (HasCapture()) ReleaseMouse();
        for (const auto& hit : Hits()) {
            if (!hit.tile.Contains(down) || !hit.tile.Contains(event.GetPosition())) continue;
            const bool close = hit.close.Contains(event.GetPosition());
            if (close != hit.close.Contains(down)) return;
            const auto path = paths_[hit.index];
            const auto callback = close ? remove : preview;
            if (callback) callback(path);
            return;
        }
    });
}

void NativeAttachmentStrip::SetAttachments(const std::vector<std::string>& paths) {
    if (paths == paths_) { Refresh(false); return; }
    paths_.assign(paths.begin(), paths.begin() + std::min<size_t>(paths.size(), 64));
    images_.clear();
    for (const auto& path : paths_) {
        if (!Image(path)) continue;
        std::error_code error;
        if (spark::fs::is_regular_file(NativePath(path), error) && !error) images_.insert(path);
    }
    std::set<std::string> visible;
    for (size_t index = 0; index < std::min<size_t>(paths_.size(), 5); ++index) {
        const auto& path = paths_[index];
        if (!images_.count(path)) continue;
        visible.insert(path);
        if (thumbnails_.find(path) != thumbnails_.end()) continue;
        wxImage image;
        image.SetOption(wxIMAGE_OPTION_MAX_WIDTH, FromDIP(160));
        image.SetOption(wxIMAGE_OPTION_MAX_HEIGHT, FromDIP(160));
        wxBitmap thumbnail;
        if (image.LoadFile(wxString::FromUTF8(path)) && image.IsOk()) {
            const double scale = std::min(static_cast<double>(FromDIP(160)) / image.GetWidth(),
                                           static_cast<double>(FromDIP(160)) / image.GetHeight());
            image.Rescale(std::max(1, static_cast<int>(image.GetWidth() * scale)),
                           std::max(1, static_cast<int>(image.GetHeight() * scale)), wxIMAGE_QUALITY_NORMAL);
            thumbnail = wxBitmap(image);
        }
        thumbnails_.emplace(path, std::move(thumbnail));
    }
    for (auto item = thumbnails_.begin(); item != thumbnails_.end();) {
        if (visible.find(item->first) == visible.end()) item = thumbnails_.erase(item);
        else ++item;
    }
    Refresh(false);
}

std::vector<NativeAttachmentStrip::Hit> NativeAttachmentStrip::Hits() const {
    std::vector<Hit> hits;
    const int size = GetClientSize().y;
    int x = 0;
    for (size_t index = 0; index < std::min<size_t>(paths_.size(), 5); ++index) {
        const int width = images_.count(paths_[index]) ? size : std::max(size, FromDIP(196));
        if (x + width > GetClientSize().x) break;
        hits.push_back({{x, 0, width, size}, {x + width - FromDIP(27), FromDIP(3), FromDIP(24), FromDIP(24)}, index});
        x += width + FromDIP(8);
    }
    return hits;
}

void NativeAttachmentStrip::Paint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    const auto palette = NativeColors(GetParent()->GetBackgroundColour().Red() < 128);
    dc.SetBackground(wxBrush(palette.panel)); dc.Clear();
    dc.SetFont(GetFont());
    const auto hits = Hits();
    for (const auto& hit : hits) {
        dc.SetPen(wxPen(palette.border)); dc.SetBrush(wxBrush(palette.input));
        dc.DrawRoundedRectangle(wxRect(hit.tile).Deflate(1), FromDIP(4));
        const auto& path = paths_[hit.index];
        const bool is_image = images_.count(path) != 0;
        const auto image = thumbnails_.find(path);
        if (image != thumbnails_.end() && image->second.IsOk()) {
            const auto& bitmap = image->second;
            const int width = std::max(1, std::min(hit.tile.width - FromDIP(4),
                hit.tile.width * bitmap.GetWidth() / bitmap.GetHeight()));
            const int height = std::max(1, std::min(hit.tile.height - FromDIP(4),
                hit.tile.height * bitmap.GetHeight() / bitmap.GetWidth()));
            wxBitmap source_bitmap = bitmap;
            wxMemoryDC source(source_bitmap);
            dc.StretchBlit(hit.tile.x + (hit.tile.width - width) / 2, (hit.tile.height - height) / 2,
                           width, height, &source, 0, 0, bitmap.GetWidth(), bitmap.GetHeight());
        } else {
            dc.SetTextForeground(is_image ? palette.muted : palette.text);
            const auto label = is_image ? wxString(i18n::Text(L"Image")) :
                wxString::FromUTF8(PathToUtf8(NativePath(path).filename()));
            const wxRect text = is_image ? hit.tile :
                wxRect(hit.tile.x + FromDIP(10), FromDIP(6),
                       hit.close.x - hit.tile.x - FromDIP(16), hit.tile.height - FromDIP(12));
            dc.DrawLabel(wxControl::Ellipsize(label, dc, wxELLIPSIZE_END, std::max(1, text.width)),
                         text, (is_image ? wxALIGN_CENTER : wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL));
        }
        dc.SetPen(wxPen(palette.border)); dc.SetBrush(wxBrush(palette.panel));
        dc.DrawEllipse(hit.close);
        dc.SetPen(wxPen(palette.text));
        const int inset = FromDIP(8);
        dc.DrawLine(hit.close.x + inset, hit.close.y + inset,
                    hit.close.x + hit.close.width - inset, hit.close.y + hit.close.height - inset);
        dc.DrawLine(hit.close.x + hit.close.width - inset, hit.close.y + inset,
                    hit.close.x + inset, hit.close.y + hit.close.height - inset);
    }
    if (paths_.size() > hits.size() && hits.size() < 5) {
        const int x = hits.empty() ? 0 : hits.back().tile.GetRight() + 1 + FromDIP(8);
        dc.SetTextForeground(palette.muted);
        dc.DrawLabel(wxString::Format("+%zu", paths_.size() - hits.size()),
                     {x, 0, FromDIP(52), GetClientSize().y}, wxALIGN_CENTER);
    }
}

}  // namespace spark
