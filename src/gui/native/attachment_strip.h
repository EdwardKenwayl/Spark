#pragma once

#include <wx/control.h>
#include <wx/bitmap.h>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace spark {

class NativeAttachmentStrip final : public wxControl {
public:
    explicit NativeAttachmentStrip(wxWindow* parent);
    void SetAttachments(const std::vector<std::string>& paths);
    std::function<void(const std::string&)> preview, remove;

private:
    struct Hit { wxRect tile, close; size_t index; };
    std::vector<Hit> Hits() const;
    void Paint(wxPaintEvent&);
    std::vector<std::string> paths_;
    std::set<std::string> images_;
    std::map<std::string, wxBitmap> thumbnails_;
    wxPoint press_{-1, -1};
};

}  // namespace spark
