#pragma once

#include <wx/bitmap.h>
#include <wx/image.h>
#include <cstdint>
#include <deque>
#include <future>
#include <map>
#include <set>
#include <string>

namespace spark {

// Decodes one image at a time; bitmap creation and cache access stay on the UI thread.
class NativeThumbnailCache {
public:
    void Clear();
    void Request(const std::string& path, int edge);
    bool Poll();
    // Drop queued work and report whether the in-flight decoder has finished.
    bool Drain();
    wxBitmap Get(const std::string& path, int edge) const;
private:
    struct RequestInfo { std::string path; int edge; uint64_t generation; };
    struct Decoded { RequestInfo request; wxImage image; };
    std::map<std::string, wxBitmap> cache_;
    std::set<std::string> requested_;
    std::deque<RequestInfo> pending_;
    std::future<Decoded> active_;
    uint64_t generation_ = 0;
};

}  // namespace spark
