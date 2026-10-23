#include "gui/window_geometry.h"
#include "core/common.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace spark {
namespace {
bool Valid(const SavedWindowGeometry& geometry) {
    return geometry.width > 0 && geometry.width <= 100000 &&
        geometry.height > 0 && geometry.height <= 100000 &&
        std::isfinite(geometry.backing_scale) &&
        geometry.backing_scale >= 0.25 && geometry.backing_scale <= 16;
}
}  // namespace

WindowSize FitWindowSize(WindowSize requested, WindowSize work, WindowSize minimum, int margin) {
    const auto fit = [margin](int size, int available, int lower) {
        available = std::max(1, available - std::clamp(margin, 0, std::max(0, available)));
        return std::clamp(size, std::clamp(lower, 1, available), available);
    };
    return {fit(requested.width, work.width, minimum.width),
            fit(requested.height, work.height, minimum.height)};
}

WindowSize InitialWindowSize(WindowSize work, WindowSize minimum, int margin) {
    return FitWindowSize({static_cast<int>(work.width * 84LL / 100),
                          static_cast<int>(work.height * 80LL / 100)}, work, minimum, margin);
}

WindowSize RestoredWindowSize(const SavedWindowGeometry& saved, double backing_scale,
                              WindowSize work, WindowSize minimum, int margin) {
    if (!Valid(saved) || !std::isfinite(backing_scale) || backing_scale < 0.25 || backing_scale > 16)
        return InitialWindowSize(work, minimum, margin);
    const double ratio = saved.backing_scale / backing_scale;
    return FitWindowSize({static_cast<int>(std::lround(saved.width * ratio)),
                          static_cast<int>(std::lround(saved.height * ratio))}, work, minimum, margin);
}

std::optional<SavedWindowGeometry> LoadWindowGeometry(const std::string& data_directory) {
    if (data_directory.empty()) return std::nullopt;
    const auto path = JoinPath(data_directory, "window-state.json");
    std::error_code error;
    const auto size = fs::file_size(NativePath(path), error);
    if (error || size > 4096) return std::nullopt;
    const auto contents = ReadFileUtf8(path);
    if (!contents) return std::nullopt;
    const auto state = nlohmann::json::parse(*contents, nullptr, false);
    if (!state.is_object() || state.value("schema_version", nlohmann::json{}) != 1)
        return std::nullopt;
    try {
        for (const auto* key : {"width", "height"})
            if (!state.at(key).is_number_integer() || state.at(key) < 1 || state.at(key) > 100000)
                return std::nullopt;
        if (!state.at("backing_scale").is_number() || !state.at("maximized").is_boolean())
            return std::nullopt;
        SavedWindowGeometry geometry{state.at("width").get<int>(), state.at("height").get<int>(),
            state.at("backing_scale").get<double>(), state.at("maximized").get<bool>()};
        if (Valid(geometry)) return geometry;
    } catch (const nlohmann::json::exception&) {}
    return std::nullopt;
}

bool SaveWindowGeometry(const std::string& data_directory, const SavedWindowGeometry& geometry) {
    if (data_directory.empty() || !Valid(geometry)) return false;
    std::error_code error;
    fs::create_directories(NativePath(data_directory), error);
    if (error) return false;
    const nlohmann::json state = {{"schema_version", 1}, {"width", geometry.width},
        {"height", geometry.height}, {"backing_scale", geometry.backing_scale},
        {"maximized", geometry.maximized}};
    return WriteFileUtf8Atomic(JoinPath(data_directory, "window-state.json"), state.dump(2) + "\n");
}

}  // namespace spark
