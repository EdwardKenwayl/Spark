#include "gui/native/thumbnail_cache.h"
#include "core/common.h"

#include <algorithm>
#include <chrono>

namespace spark {

void NativeThumbnailCache::Clear() {
    ++generation_;
    cache_.clear(); requested_.clear(); pending_.clear();
}
void NativeThumbnailCache::Request(const std::string& path, int edge) {
    if (path.empty() || requested_.size() >= 32 || !requested_.insert(path).second) return;
    pending_.push_back({path, std::clamp(edge, 1, 128), generation_});
}
wxBitmap NativeThumbnailCache::Get(const std::string& path, int edge) const {
    const auto found = cache_.find(path);
    if (found == cache_.end() || !found->second.IsOk() ||
        found->second.GetWidth() > edge || found->second.GetHeight() > edge) return {};
    return found->second;
}
bool NativeThumbnailCache::Drain() {
    pending_.clear();
    if (!active_.valid()) return true;
    if (active_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    try { active_.get(); } catch (const std::exception&) {}
    return true;
}
bool NativeThumbnailCache::Poll() {
    bool changed = false;
    if (active_.valid() && active_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto decoded = active_.get();
            if (decoded.request.generation == generation_) {
                cache_[decoded.request.path] = decoded.image.IsOk() ? wxBitmap(decoded.image) : wxBitmap{};
                changed = true;
            }
        } catch (const std::exception&) { /* A failed preview does not fail the conversation. */ }
    }
    if (!active_.valid() && !pending_.empty()) {
        auto request = std::move(pending_.front()); pending_.pop_front();
        active_ = std::async(std::launch::async, [request = std::move(request)] {
            Decoded result{request, {}};
            std::error_code error;
            const auto file = NativePath(request.path);
            if (!spark::fs::is_regular_file(file, error) || error) return result;
            const auto bytes = spark::fs::file_size(file, error);
            if (error || bytes > 8 * 1024 * 1024) return result;
            wxImage image;
            image.SetLoadFlags(0);
            image.SetOption(wxIMAGE_OPTION_MAX_WIDTH, request.edge);
            image.SetOption(wxIMAGE_OPTION_MAX_HEIGHT, request.edge);
            if (!image.LoadFile(wxString::FromUTF8(request.path)) || !image.IsOk()) return result;
            const double scale = std::min({1.0, static_cast<double>(request.edge) / image.GetWidth(),
                                          static_cast<double>(request.edge) / image.GetHeight()});
            image.Rescale(std::max(1, static_cast<int>(image.GetWidth() * scale)),
                          std::max(1, static_cast<int>(image.GetHeight() * scale)), wxIMAGE_QUALITY_NORMAL);
            result.image = std::move(image);
            return result;
        });
    }
    return changed;
}

}  // namespace spark
