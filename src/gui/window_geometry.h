#pragma once

#include <optional>
#include <string>

namespace spark {

struct WindowSize { int width, height; };

struct SavedWindowGeometry {
    int width = 0, height = 0;
    double backing_scale = 1;
    bool maximized = false;
};

WindowSize FitWindowSize(WindowSize requested, WindowSize work, WindowSize minimum, int margin);
WindowSize InitialWindowSize(WindowSize work, WindowSize minimum, int margin);
WindowSize RestoredWindowSize(const SavedWindowGeometry& saved, double backing_scale,
                              WindowSize work, WindowSize minimum, int margin);
std::optional<SavedWindowGeometry> LoadWindowGeometry(const std::string& data_directory);
bool SaveWindowGeometry(const std::string& data_directory, const SavedWindowGeometry& geometry);

}  // namespace spark
