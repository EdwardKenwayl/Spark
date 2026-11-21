#include "core/common.h"
#include "core/context_compactor.h"
#include "core/prompt_image.h"
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

#include <fstream>
#include <iostream>

namespace {
using namespace spark;
int failures = 0;
void Check(bool value, const char* label) {
    if (!value) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
void Append(void* target, void* bytes, int length) {
    static_cast<std::string*>(target)->append(static_cast<char*>(bytes), length);
}
void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
void Orientations(const fs::path& root) {
    const int width = 3072, height = 768;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const size_t index = (static_cast<size_t>(y) * width + x) * 3;
        pixels[index] = x < width / 2 ? 220 : 30;
        pixels[index + 1] = y < height / 2 ? 220 : 30;
        pixels[index + 2] = 60;
    }
    std::string jpeg;
    stbi_write_jpg_to_func(Append, &jpeg, width, height, 3, pixels.data(), 90);
    const std::string exif("\xff\xe1\x00\x22"
                           "Exif\0\0"
                           "II\x2a\0\x08\0\0\0"
                           "\x01\0\x12\x01\x03\0\x01\0\0\0\x01\0\0\0\0\0\0\0", 36);
    // Top-left source quadrants after each of the eight EXIF transforms.
    const int expected[][2] = {{220,220},{30,220},{30,30},{220,30},
                              {220,220},{220,30},{30,30},{30,220}};
    for (int orientation = 1; orientation <= 8; ++orientation) {
        auto metadata = exif;
        metadata[28] = static_cast<char>(orientation);
        const auto input = jpeg.substr(0, 2) + metadata + jpeg.substr(2);
        const auto path = root / ("rotated-" + std::to_string(orientation) + ".jpg");
        Write(path, input);
        const auto prepared = LoadPromptImage(PathToUtf8(path));
        Check(prepared != nullptr, "oriented JPEG prepares");
        if (!prepared) continue;
        int w = 0, h = 0, c = 0;
        auto* decoded = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(prepared->bytes.data()),
            static_cast<int>(prepared->bytes.size()), &w, &h, &c, 3);
        Check(decoded && std::max(w, h) <= 2048 && ((w + 31) / 32) * ((h + 31) / 32) <= 2500,
              "encoded dimensions respect visual patch budget");
        Check((orientation < 5 ? w > h : h > w), "JPEG rotation preserves displayed aspect");
        if (decoded) {
            const auto* p = decoded + (static_cast<size_t>(h / 8) * w + w / 8) * 3;
            Check(std::abs(int(p[0]) - expected[orientation - 1][0]) < 15 &&
                  std::abs(int(p[1]) - expected[orientation - 1][1]) < 15,
                  "EXIF transform preserves corner content");
        }
        stbi_image_free(decoded);
        const auto cached = LoadPromptImage(PathToUtf8(path));
        Check(prepared == cached, "repeated reads reuse prepared image");
        Write(path, "replacement");
        Check(LoadPromptImage(PathToUtf8(path))->bytes == "replacement",
              "changed source invalidates prepared image");
    }
}
void Budgets(const fs::path& root) {
    const auto path = root / "provider-format.webp";
    Write(path, std::string(2 * 1024 * 1024, 'w'));
    ChatMessage message; message.role = MessageRole::User; message.content = "Inspect images.";
    message.image_paths.resize(10, PathToUtf8(path));
    const auto prepared = PreparePromptImageHistory({message});
    Check(prepared[0].image_paths.size() == 5, "base64 expansion participates in cumulative byte budget");
    Check(message.image_paths.size() == 10, "wire preparation does not change source history");
    Write(path, "small");
    message.image_paths.resize(10000, PathToUtf8(path));
    const auto many = PreparePromptImageHistory({message});
    Check(many[0].image_paths.size() == 16, "many small images have a bounded visual count");
    Check(many[0].content.size() <= message.content.size() + 4096, "omission notes have a shared text budget");
    const auto compacted = BuildCompactedHistory({message}, "Images inspected.");
    size_t retained = 0;
    for (const auto& item : compacted) retained += item.image_paths.size();
    Check(retained == 4, "compaction limits retained visual context");
    const auto next = PreparePromptImageHistory(compacted);
    size_t next_count = 0;
    for (const auto& item : next) next_count += item.image_paths.size();
    Check(next_count == 4, "next inference does not reattach summarized images");
    ChatMessage ordered = message;
    ordered.image_paths = {"old-a", "old-b", "old-c", "old-d", "new-a", "new-b", "new-c", "new-d"};
    const auto latest = BuildCompactedHistory({ordered}, "Summary");
    Check(latest.back().image_paths == std::vector<std::string>({"new-a","new-b","new-c","new-d"}),
          "compaction retains the newest images in original order");
}
}
int main() {
    const auto root = spark::fs::temp_directory_path() /
        ("spark-image-preparation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    spark::fs::create_directories(root);
    Orientations(root);
    Budgets(root);
    std::error_code ignored; spark::fs::remove_all(root, ignored);
    std::cout << "Image preparation regression failures: " << failures << '\n';
    return failures ? 1 : 0;
}
