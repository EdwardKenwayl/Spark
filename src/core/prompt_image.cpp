#include "core/prompt_image.h"
#include "core/common.h"
#include "platform/binary_io.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <mutex>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 32768
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace spark {
namespace {
constexpr size_t kImageBytes = 1024 * 1024;
constexpr size_t kRequestImageBytes = 16 * 1024 * 1024;
constexpr size_t kCacheBytes = 16 * 1024 * 1024;
struct Entry {
    std::string path;
    uintmax_t size;
    fs::file_time_type time;
    std::shared_ptr<const PromptImage> image;
};
std::mutex cache_mutex;
std::list<Entry> cache;
size_t cached_bytes = 0;

std::string Mime(std::string_view path) {
    const auto ext = LowerAscii(PathToUtf8(NativePath(path).extension()));
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".png") return "image/png";
    if (ext == ".gif") return "image/gif";
    if (ext == ".webp") return "image/webp";
    if (ext == ".bmp") return "image/bmp";
    return "application/octet-stream";
}
void Append(void* context, void* bytes, int length) {
    static_cast<std::string*>(context)->append(static_cast<const char*>(bytes), static_cast<size_t>(length));
}
int Orientation(const std::string& bytes) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    if (bytes.size() < 4 || data[0] != 0xff || data[1] != 0xd8) return 1;
    for (size_t offset = 2; offset + 4 <= bytes.size();) {
        if (data[offset] != 0xff) break;
        const auto marker = data[offset + 1];
        if (marker == 0xda || marker == 0xd9) break;
        const size_t length = data[offset + 2] * 256u + data[offset + 3];
        if (length < 2 || length > bytes.size() - offset - 2) break;
        if (marker == 0xe1 && length >= 16 && bytes.compare(offset + 4, 6, "Exif\0\0", 6) == 0) {
            const size_t base = offset + 10, end = offset + 2 + length;
            const bool little = data[base] == 'I' && data[base + 1] == 'I';
            if (!little && !(data[base] == 'M' && data[base + 1] == 'M')) return 1;
            const auto read = [&](size_t pos, size_t count) -> uint32_t {
                if (pos > end || count > end - pos) return 0;
                uint32_t value = 0;
                for (size_t i = 0; i < count; ++i)
                    value |= static_cast<uint32_t>(data[pos + i]) << (8 * (little ? i : count - i - 1));
                return value;
            };
            if (read(base + 2, 2) != 42) return 1;
            const uint32_t relative = read(base + 4, 4);
            if (relative > end - base || end - base - relative < 2) return 1;
            const size_t directory = base + relative;
            const size_t count = std::min<size_t>(read(directory, 2), (end - directory - 2) / 12);
            for (size_t i = 0; i < count; ++i) {
                const size_t entry = directory + 2 + i * 12;
                if (read(entry, 2) == 0x112 && read(entry + 2, 2) == 3 && read(entry + 4, 4) == 1) {
                    const auto value = read(entry + 8, 2);
                    return value >= 1 && value <= 8 ? static_cast<int>(value) : 1;
                }
            }
        }
        offset += 2 + length;
    }
    return 1;
}
std::shared_ptr<const PromptImage> Prepare(const std::string& bytes, std::string_view path) {
    int width = 0, height = 0, channels = 0;
    const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int length = static_cast<int>(bytes.size());
    if (!stbi_info_from_memory(data, length, &width, &height, &channels)) {
        // Formats supplied by a provider, including WebP, can still pass through.
        return std::make_shared<PromptImage>(PromptImage{bytes, Mime(path)});
    }
    const double scale = std::min({1.0, 2048.0 / width, 2048.0 / height,
                                  std::sqrt(2500.0 * 32 * 32 / (static_cast<double>(width) * height))});
    int target_width = std::max(1, static_cast<int>(std::floor(width * scale)));
    int target_height = std::max(1, static_cast<int>(std::floor(height * scale)));
    while (((target_width + 31) / 32) * ((target_height + 31) / 32) > 2500) {
        if (target_width >= target_height) --target_width; else --target_height;
    }
    const auto mime = Mime(path);
    if (target_width == width && target_height == height && bytes.size() <= kImageBytes &&
        (mime == "image/png" || mime == "image/jpeg" || mime == "image/gif")) {
        return std::make_shared<PromptImage>(PromptImage{bytes, mime});
    }
    // Bound transient decode memory on older machines, independently of file size.
    if (static_cast<uint64_t>(width) * height > 32 * 1024 * 1024) return {};
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(data, length, &width, &height, &channels, 4), stbi_image_free);
    if (!pixels) return {};
    const int source_width = width, source_height = height, orientation = Orientation(bytes);
    if (orientation >= 5) {
        std::swap(width, height);
        std::swap(target_width, target_height);
    }
    PromptImage output;
    for (;;) {
        std::vector<unsigned char> rgb(static_cast<size_t>(target_width) * target_height * 3);
        for (int y = 0; y < target_height; ++y) {
            const double sy = static_cast<double>(y) * height / target_height;
            const int y0 = static_cast<int>(sy), y1 = std::min(y0 + 1, height - 1);
            for (int x = 0; x < target_width; ++x) {
                const double sx = static_cast<double>(x) * width / target_width;
                const int x0 = static_cast<int>(sx), x1 = std::min(x0 + 1, width - 1);
                for (int c = 0; c < 3; ++c) {
                    const auto sample = [&](int px, int py) {
                        int source_x = px, source_y = py;
                        switch (orientation) {
                        case 2: source_x = source_width - 1 - px; break;
                        case 3: source_x = source_width - 1 - px; source_y = source_height - 1 - py; break;
                        case 4: source_y = source_height - 1 - py; break;
                        case 5: source_x = py; source_y = px; break;
                        case 6: source_x = py; source_y = source_height - 1 - px; break;
                        case 7: source_x = source_width - 1 - py; source_y = source_height - 1 - px; break;
                        case 8: source_x = source_width - 1 - py; source_y = px; break;
                        default: break;
                        }
                        const auto* p = pixels.get() + (static_cast<size_t>(source_y) * source_width + source_x) * 4;
                        return (p[c] * p[3] + 255.0 * (255 - p[3])) / 255.0;
                    };
                    const double top = sample(x0, y0) * (1 - sx + x0) + sample(x1, y0) * (sx - x0);
                    const double bottom = sample(x0, y1) * (1 - sx + x0) + sample(x1, y1) * (sx - x0);
                    rgb[(static_cast<size_t>(y) * target_width + x) * 3 + c] =
                        static_cast<unsigned char>(top * (1 - sy + y0) + bottom * (sy - y0));
                }
            }
        }
        output.bytes.clear();
        output.mime = "image/png";
        if (!stbi_write_png_to_func(Append, &output.bytes, target_width, target_height, 3,
                                    rgb.data(), target_width * 3)) return {};
        if (output.bytes.size() > kImageBytes) {
            output.bytes.clear(); output.mime = "image/jpeg";
            if (!stbi_write_jpg_to_func(Append, &output.bytes, target_width, target_height, 3,
                                       rgb.data(), 85)) return {};
        }
        if (output.bytes.size() <= kImageBytes) break;
        target_width = std::max(1, target_width * 3 / 4);
        target_height = std::max(1, target_height * 3 / 4);
    }
    return std::make_shared<PromptImage>(std::move(output));
}
}  // namespace

std::shared_ptr<const PromptImage> LoadPromptImage(std::string_view path) {
    std::error_code error;
    const auto native = NativePath(path);
    const auto size = fs::file_size(native, error);
    if (error) return {};
    const auto time = fs::last_write_time(native, error);
    if (error) return {};
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        for (auto it = cache.begin(); it != cache.end(); ++it) {
            if (it->path == path && it->size == size && it->time == time) {
                const auto image = it->image;
                cache.splice(cache.begin(), cache, it);
                return image;
            }
        }
    }
    const auto bytes = ReadAttachmentBytes(path);
    if (!bytes) return {};
    const auto image = Prepare(*bytes, path);
    if (!image || image->bytes.size() > kCacheBytes) return image;
    std::lock_guard<std::mutex> lock(cache_mutex);
    for (auto it = cache.begin(); it != cache.end();) {
        if (it->path == path) {
            cached_bytes -= it->image->bytes.size();
            it = cache.erase(it);
        } else ++it;
    }
    cache.push_front({std::string(path), size, time, image});
    cached_bytes += image->bytes.size();
    while (cache.size() > 16 || cached_bytes > kCacheBytes) {
        cached_bytes -= cache.back().image->bytes.size();
        cache.pop_back();
    }
    return image;
}

std::vector<ChatMessage> PreparePromptImageHistory(const std::vector<ChatMessage>& messages) {
    auto result = messages;
    size_t remaining = kRequestImageBytes;
    size_t remaining_images = 16, remaining_notes = 4096;
    for (auto message = result.rbegin(); message != result.rend(); ++message) {
        std::vector<std::string> retained;
        for (auto path = message->image_paths.rbegin(); path != message->image_paths.rend(); ++path) {
            const auto image = remaining_images ? LoadPromptImage(*path) : nullptr;
            const size_t wire_bytes = image ? (image->bytes.size() + 2) / 3 * 4 : 0;
            if (remaining_images && wire_bytes <= remaining) {
                retained.push_back(*path);
                remaining -= wire_bytes;
                --remaining_images;
            } else if (remaining_notes) {
                const auto note = TruncateUtf8(
                    "\n[Earlier image omitted from this request; use view_image if needed: " +
                    TruncateUtf8(*path, 1024) + "]", remaining_notes);
                message->content += note;
                remaining_notes -= note.size();
            }
        }
        std::reverse(retained.begin(), retained.end());
        message->image_paths = std::move(retained);
    }
    return result;
}
}  // namespace spark
