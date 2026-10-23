#include "platform/desktop_capture.h"

#ifdef __APPLE__

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace spark {
namespace {

constexpr uint64_t kMaxPixels = 16000000;
constexpr CFIndex kMaxWindows = 512;

template <typename T>
struct ScopedCf {
    explicit ScopedCf(T object) : value(object) {}
    ~ScopedCf() { if (value) CFRelease(value); }
    ScopedCf(const ScopedCf&) = delete;
    ScopedCf& operator=(const ScopedCf&) = delete;
    T value;
};

using WindowImageFunction = CGImageRef (*)(CGRect, CGWindowListOption, CGWindowID, CGWindowImageOption);

WindowImageFunction WindowImage() {
    // Resolve the requested public legacy API without referring to declarations
    // marked unavailable by newer SDKs. Permission checks still apply.
    static const auto function = reinterpret_cast<WindowImageFunction>(
        dlsym(RTLD_DEFAULT, "CGWindowListCreateImage"));
    return function;
}

bool HasPermission() {
    if (__builtin_available(macOS 10.15, *)) return CGPreflightScreenCaptureAccess();
    return true;
}

int64_t Number(CFDictionaryRef dictionary, CFStringRef key, int64_t fallback = -1) {
    const auto value = static_cast<CFNumberRef>(CFDictionaryGetValue(dictionary, key));
    int64_t number = 0;
    return value && CFGetTypeID(value) == CFNumberGetTypeID() &&
                   CFNumberGetValue(value, kCFNumberSInt64Type, &number) ? number : fallback;
}

std::string Text(CFDictionaryRef dictionary, CFStringRef key) {
    const auto value = static_cast<CFStringRef>(CFDictionaryGetValue(dictionary, key));
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return {};
    std::array<UInt8, 4096> bytes{};
    CFIndex used = 0;
    CFStringGetBytes(value, CFRangeMake(0, std::min<CFIndex>(CFStringGetLength(value), 1024)),
                     kCFStringEncodingUTF8, 0, false, bytes.data(), bytes.size(), &used);
    std::string text(reinterpret_cast<const char*>(bytes.data()), static_cast<size_t>(used));
    const auto nul = text.find('\0');
    if (nul != std::string::npos) text.resize(nul);
    return SanitizeDisplayText(TruncateUtf8(std::move(text), 1024));
}

bool Candidate(CFDictionaryRef info, DesktopWindow& window, CGRect& bounds) {
    if (!info || CFGetTypeID(info) != CFDictionaryGetTypeID()) return false;
    const int64_t id = Number(info, kCGWindowNumber);
    const int64_t pid = Number(info, kCGWindowOwnerPID);
    if (id <= 0 || id > std::numeric_limits<uint32_t>::max() || pid <= 0 ||
        pid == static_cast<int64_t>(getpid()) || Number(info, kCGWindowLayer) != 0 ||
        Number(info, kCGWindowSharingState) == kCGWindowSharingNone ||
        CFDictionaryGetValue(info, kCGWindowIsOnscreen) != kCFBooleanTrue) return false;
    const auto rectangle = static_cast<CFDictionaryRef>(CFDictionaryGetValue(info, kCGWindowBounds));
    if (!rectangle || CFGetTypeID(rectangle) != CFDictionaryGetTypeID() ||
        !CGRectMakeWithDictionaryRepresentation(rectangle, &bounds) ||
        !std::isfinite(bounds.origin.x) || !std::isfinite(bounds.origin.y) ||
        !std::isfinite(bounds.size.width) || !std::isfinite(bounds.size.height) ||
        bounds.size.width <= 0 || bounds.size.height <= 0) return false;
    bounds = CGRectIntegral(bounds);
    if (!std::isfinite(bounds.size.width) || !std::isfinite(bounds.size.height) ||
        bounds.size.width * bounds.size.height > kMaxPixels) return false;
    const auto alpha = static_cast<CFNumberRef>(CFDictionaryGetValue(info, kCGWindowAlpha));
    double opacity = 0;
    if (!alpha || CFGetTypeID(alpha) != CFNumberGetTypeID() ||
        !CFNumberGetValue(alpha, kCFNumberDoubleType, &opacity) || !(opacity > 0)) return false;
    window.id = static_cast<uint64_t>(id);
    window.title = Text(info, kCGWindowName);
    if (window.title.empty()) window.title = Text(info, kCGWindowOwnerName);
    if (window.title.empty()) window.title = "Untitled window (" + std::to_string(id) + ")";
    return true;
}

bool FindWindow(CGWindowID id, DesktopWindow& window, CGRect& bounds) {
    ScopedCf<CFArrayRef> list(CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, id));
    if (!list.value) return false;
    for (CFIndex index = 0; index < std::min(CFArrayGetCount(list.value), kMaxWindows); ++index) {
        const auto info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list.value, index));
        if (Candidate(info, window, bounds) && window.id == id) return true;
    }
    return false;
}

}  // namespace

bool DesktopCaptureAvailable() {
    return WindowImage() && HasPermission();
}

std::vector<DesktopWindow> SnapshotWindows() {
    std::vector<DesktopWindow> windows;
    if (!DesktopCaptureAvailable()) return windows;
    const auto options = static_cast<CGWindowListOption>(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements);
    ScopedCf<CFArrayRef> list(CGWindowListCopyWindowInfo(options, kCGNullWindowID));
    if (!list.value) return windows;
    for (CFIndex index = 0; index < std::min(CFArrayGetCount(list.value), kMaxWindows); ++index) {
        const auto info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list.value, index));
        DesktopWindow window{};
        CGRect bounds{};
        if (Candidate(info, window, bounds)) windows.push_back(std::move(window));
    }
    return windows;
}

Result<void> CaptureDesktopWindow(uint64_t id, const std::string& png_path) {
    if (id == 0 || id > std::numeric_limits<uint32_t>::max())
        return Result<void>::Failure("Invalid macOS window id.");
    if (png_path.empty() || png_path.find('\0') != std::string::npos)
        return Result<void>::Failure("Invalid PNG output path.");
    const auto capture = WindowImage();
    if (!capture) return Result<void>::Failure("CGWindowListCreateImage is unavailable on this version of macOS.");
    if (!HasPermission())
        return Result<void>::Failure("Screen Recording permission is required. Enable Spark in System Settings > "
                                     "Privacy & Security > Screen Recording, then restart Spark.");
    DesktopWindow window{};
    CGRect bounds{};
    const auto window_id = static_cast<CGWindowID>(id);
    if (!FindWindow(window_id, window, bounds))
        return Result<void>::Failure("Window closed, is hidden or protected, is owned by Spark, or exceeds 16 million pixels.");

    // IncludingWindow renders this window alone. An explicit bounded rectangle
    // and nominal resolution also bound allocation if the window grows or is on Retina.
    const auto options = static_cast<CGWindowImageOption>(
        kCGWindowImageBoundsIgnoreFraming | kCGWindowImageNominalResolution);
    ScopedCf<CGImageRef> image(capture(bounds, kCGWindowListOptionIncludingWindow, window_id, options));
    if (!image.value)
        return Result<void>::Failure("macOS could not capture this window; check Screen Recording permission and retry.");
    const size_t width = CGImageGetWidth(image.value), height = CGImageGetHeight(image.value);
    if (width == 0 || height == 0 || width > kMaxPixels || height > kMaxPixels / width)
        return Result<void>::Failure("Captured image is empty or exceeds 16 million pixels.");
    DesktopWindow current{};
    CGRect current_bounds{};
    if (!FindWindow(window_id, current, current_bounds) || !CGRectEqualToRect(bounds, current_bounds))
        return Result<void>::Failure("Window moved, resized, or closed during capture; reopen the picker and retry.");
    if (!HasPermission()) return Result<void>::Failure("Screen Recording permission was revoked during capture.");

    ScopedCf<CFMutableDataRef> data(CFDataCreateMutable(kCFAllocatorDefault, 0));
    if (!data.value) return Result<void>::Failure("Could not allocate the PNG output.");
    ScopedCf<CGImageDestinationRef> destination(
        CGImageDestinationCreateWithData(data.value, CFSTR("public.png"), 1, nullptr));
    if (!destination.value) return Result<void>::Failure("Could not create the PNG encoder.");
    CGImageDestinationAddImage(destination.value, image.value, nullptr);
    if (!CGImageDestinationFinalize(destination.value))
        return Result<void>::Failure("Could not encode the window image as PNG.");
    const CFIndex length = CFDataGetLength(data.value);
    if (length <= 0 || length > 64 * 1024 * 1024)
        return Result<void>::Failure("Encoded PNG is empty or exceeds 64 MiB.");
    const std::string_view bytes(reinterpret_cast<const char*>(CFDataGetBytePtr(data.value)),
                                  static_cast<size_t>(length));
    std::string error;
    // The common atomic writer stores the ImageIO bytes verbatim.
    if (!WriteFileUtf8Atomic(png_path, bytes, &error))
        return Result<void>::Failure("Could not save window PNG: " + error);
    return Result<void>::Success();
}

}  // namespace spark

#endif
