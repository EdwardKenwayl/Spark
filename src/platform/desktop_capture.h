#pragma once

#include "core/common.h"

#include <cstdint>
#include <string>
#include <vector>

namespace spark {

struct DesktopWindow {
    uint64_t id;
    std::string title;
};

// Call on the GUI thread, on demand. Only mapped windows of other processes
// are offered. Availability checks do not request screen-recording permission.
std::vector<DesktopWindow> SnapshotWindows();
// UTF-8 output path; its parent must exist. Images are limited to 16 million
// pixels and committed atomically. No fallback to a screenshot of the screen.
Result<void> CaptureDesktopWindow(uint64_t id, const std::string& png_path);
bool DesktopCaptureAvailable();

}  // namespace spark
