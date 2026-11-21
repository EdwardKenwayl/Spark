#include "gui/chat_image_cache.h"
#include "core/common.h"

#include "platform/gdiplus_headers.h"
#include <algorithm>
#include <chrono>

namespace spark {

void ChatImageCache::Clear() {
    ++generation_;
    cache_.clear();
    pending_.clear();
    active_path_.clear();
}

void ChatImageCache::Drain() {
    Clear();
    // Child windows are destroyed before the application's GDI+ shutdown.
    // No decoder may retain a GDI+ image beyond that point.
    if (active_.valid()) {
        try { active_.get(); } catch (...) {}
    }
}

void ChatImageCache::Start() {
    if (active_.valid() || pending_.empty()) return;
    const auto request = pending_.front();
    pending_.pop_front();
    active_path_ = request.path;
    active_ = std::async(std::launch::async, [request] {
        auto image = std::make_shared<Image>();
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        const auto path = Utf8ToWide(request.path);
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) &&
            attributes.nFileSizeHigh == 0 && attributes.nFileSizeLow <= 32 * 1024 * 1024) {
            Gdiplus::Bitmap source(path.c_str(), FALSE);
            const auto width = source.GetWidth(), height = source.GetHeight();
            if (source.GetLastStatus() == Gdiplus::Ok && width && height &&
                static_cast<uint64_t>(width) * height <= 32 * 1024 * 1024) {
                const double scale = std::min(240.0 / width, 180.0 / height);
                image->width = std::max(1, static_cast<int>(width * scale));
                image->height = std::max(1, static_cast<int>(height * scale));
                Gdiplus::Bitmap thumbnail(image->width, image->height, PixelFormat32bppPARGB);
                Gdiplus::Graphics graphics(&thumbnail);
                graphics.Clear(Gdiplus::Color(255, 48, 49, 52));
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
                if (graphics.DrawImage(&source, 0, 0, image->width, image->height) == Gdiplus::Ok)
                    thumbnail.GetHBITMAP(Gdiplus::Color(255, 48, 49, 52), &image->bitmap);
            }
        }
        return Result{request, image};
    });
}

bool ChatImageCache::Poll() {
    bool changed = false;
    if (active_.valid() && active_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto result = active_.get();
            if (result.request.generation == generation_) {
                if (cache_.size() >= 48) cache_.erase(cache_.begin());
                cache_[result.request.path] = std::move(result.image);
                changed = true;
            }
        } catch (...) { /* Decoder failure leaves the filename tile clickable. */ }
        active_path_.clear();
    }
    Start();
    return changed;
}

void ChatImageCache::Paint(HDC device, const RECT& tile, const std::string& path) {
    const auto found = cache_.find(path);
    if (found == cache_.end()) {
        const bool queued = std::any_of(pending_.begin(), pending_.end(),
            [&](const Request& request) { return request.path == path; });
        if (active_path_ != path && !queued && pending_.size() < 16) {
            pending_.push_back({path, generation_});
            Start();
        }
    }
    if (found == cache_.end() || !found->second->bitmap) {
        auto label = Utf8ToWide(PathToUtf8(NativePath(path).filename()));
        RECT text = tile;
        DrawTextW(device, label.c_str(), -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        return;
    }
    const auto& image = *found->second;
    const double scale = std::min(static_cast<double>(tile.right - tile.left) / image.width,
                                  static_cast<double>(tile.bottom - tile.top) / image.height);
    const int width = std::max(1, static_cast<int>(image.width * scale));
    const int height = std::max(1, static_cast<int>(image.height * scale));
    HDC source = CreateCompatibleDC(device);
    auto previous = SelectObject(source, image.bitmap);
    SetStretchBltMode(device, HALFTONE);
    StretchBlt(device, tile.left + (tile.right - tile.left - width) / 2,
               tile.top + (tile.bottom - tile.top - height) / 2, width, height,
               source, 0, 0, image.width, image.height, SRCCOPY);
    SelectObject(source, previous);
    DeleteDC(source);
}

}  // namespace spark
