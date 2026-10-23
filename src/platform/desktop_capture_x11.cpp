#include "platform/desktop_capture.h"

#if defined(__linux__)
#if defined(SPARK_HAVE_XCOMPOSITE) && SPARK_HAVE_XCOMPOSITE

#include <wx/image.h>
#include <wx/imagpng.h>
#include <wx/mstream.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <mutex>

namespace spark {
namespace {

constexpr int kXSuccess = Success;
#undef Success

constexpr uint64_t kMaxPixels = 16000000;
constexpr size_t kMaxWindows = 512;
std::mutex connection_mutex;
std::atomic<Display*> capture_display{nullptr};
std::atomic<int> capture_error{0};
std::atomic<XErrorHandler> previous_handler{nullptr};

int HandleXError(Display* display, XErrorEvent* event) {
    if (display == capture_display.load()) {
        capture_error.store(event->error_code);
        return 0;
    }
    const auto previous = previous_handler.load();
    return previous ? previous(display, event) : 0;
}

// The private connection and scoped handler catch disappearing windows and
// invalid pixmaps without swallowing errors belonging to wx's X connection.
class Connection {
public:
    Connection() : lock_(connection_mutex), display(XOpenDisplay(nullptr)) {
        if (display) {
            capture_display.store(display);
            previous_handler.store(XSetErrorHandler(HandleXError));
            capture_error.store(0);
        }
    }
    ~Connection() {
        if (!display) return;
        XSync(display, False);
        XCloseDisplay(display);
        XSetErrorHandler(previous_handler.load());
        capture_display.store(nullptr);
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    bool Sync() {
        XSync(display, False);
        return capture_error.exchange(0) == 0;
    }
    bool Composite() {
        int event = 0, error = 0, major = 0, minor = 0;
        const bool supported = XCompositeQueryExtension(display, &event, &error) &&
                               XCompositeQueryVersion(display, &major, &minor) &&
                               (major > 0 || minor >= 2);
        return Sync() && supported;
    }
    bool Compositor(int screen) {
        const std::string name = "_NET_WM_CM_S" + std::to_string(screen);
        const Atom selection = XInternAtom(display, name.c_str(), True);
        return selection != None && XGetSelectionOwner(display, selection) != None;
    }

private:
    std::unique_lock<std::mutex> lock_;
public:
    Display* const display;
};

struct Property {
    std::unique_ptr<unsigned char, decltype(&XFree)> data{nullptr, XFree};
    unsigned long count = 0;
    int format = 0;
};

Property ReadProperty(Display* display, Window window, const char* name, Atom type, long limit) {
    Property result;
    const Atom property = XInternAtom(display, name, True);
    if (property == None || type == None) return result;
    Atom actual = None;
    unsigned long remaining = 0;
    unsigned char* bytes = nullptr;
    const int status = XGetWindowProperty(display, window, property, 0, limit, False, type,
                                          &actual, &result.format, &result.count, &remaining, &bytes);
    result.data.reset(bytes);
    if (status != kXSuccess || actual != type || !bytes) {
        result.data.reset();
        result.count = 0;
    }
    return result;
}

std::string WindowTitle(Display* display, Window window) {
    auto title = ReadProperty(display, window, "_NET_WM_NAME",
                              XInternAtom(display, "UTF8_STRING", True), 256);
    std::string text;
    if (title.data && title.format == 8) {
        text.assign(reinterpret_cast<const char*>(title.data.get()), title.count);
        text = TruncateUtf8(std::move(text), 1020);
        if (!IsValidUtf8(text)) text.clear();
    }
    if (text.empty()) {
        title = ReadProperty(display, window, "WM_NAME", XA_STRING, 256);
        if (title.data && title.format == 8) {
            std::wstring latin1;
            for (unsigned long index = 0; index < title.count; ++index) latin1.push_back(title.data.get()[index]);
            text = WideToUtf8(latin1);
        }
    }
    const auto nul = text.find('\0');
    if (nul != std::string::npos) text.resize(nul);
    text = SanitizeDisplayText(TruncateUtf8(std::move(text), 1024));
    return text.empty() ? "Untitled window (" + std::to_string(window) + ")" : text;
}

bool Candidate(Connection& connection, Window window, XWindowAttributes& attributes) {
    if (!XGetWindowAttributes(connection.display, window, &attributes)) {
        connection.Sync();
        return false;
    }
    auto pid = ReadProperty(connection.display, window, "_NET_WM_PID", XA_CARDINAL, 1);
    const bool owned = pid.data && pid.format == 32 && pid.count == 1 &&
                       *reinterpret_cast<const unsigned long*>(pid.data.get()) != 0 &&
                       *reinterpret_cast<const unsigned long*>(pid.data.get()) != static_cast<unsigned long>(getpid());
    return connection.Sync() && owned && window != attributes.root &&
           attributes.c_class == InputOutput && attributes.map_state == IsViewable &&
           attributes.width > 0 && attributes.height > 0 &&
           static_cast<uint64_t>(attributes.width) * attributes.height <= kMaxPixels;
}

Property WindowList(Connection& connection, int screen) {
    if (!connection.Compositor(screen)) return {};
    const Window root = RootWindow(connection.display, screen);
    auto list = ReadProperty(connection.display, root, "_NET_CLIENT_LIST_STACKING", XA_WINDOW, 2048);
    // The unsorted client list is mapping order, not the frontmost-window order
    // needed by Smart Snapshot. Do not silently choose a potentially wrong app.
    if (!connection.Sync() || list.format != 32) return {};
    return list;
}

struct Backing {
    Pixmap pixmap = None;
    Visual* visual = nullptr;
    int x = 0, y = 0;
};

bool FindBacking(Connection& connection, Window client, const XWindowAttributes& client_attributes, Backing& backing) {
    Window ancestor = client;
    for (unsigned level = 0; level < 16; ++level) {
        XWindowAttributes attributes{};
        if (!XGetWindowAttributes(connection.display, ancestor, &attributes)) {
            connection.Sync();
            return false;
        }
        const Pixmap pixmap = XCompositeNameWindowPixmap(connection.display, ancestor);
        if (connection.Sync() && pixmap != None) {
            Window root = None, child = None;
            int x = 0, y = 0, origin_x = 0, origin_y = 0;
            unsigned width = 0, height = 0, border = 0, depth = 0;
            const bool geometry = XGetGeometry(connection.display, pixmap, &root, &x, &y,
                                               &width, &height, &border, &depth);
            const bool translated = XTranslateCoordinates(connection.display, client, ancestor, 0, 0,
                                                           &origin_x, &origin_y, &child);
            const int64_t left = static_cast<int64_t>(origin_x) + attributes.border_width;
            const int64_t top = static_cast<int64_t>(origin_y) + attributes.border_width;
            if (connection.Sync() && geometry && translated && depth <= 32 && left >= 0 && top >= 0 &&
                left + client_attributes.width <= width && top + client_attributes.height <= height &&
                attributes.visual && attributes.visual->c_class == TrueColor &&
                attributes.visual->red_mask && attributes.visual->green_mask && attributes.visual->blue_mask) {
                backing = {pixmap, attributes.visual, static_cast<int>(left), static_cast<int>(top)};
                return true;
            }
        }
        Window root = None, parent = None;
        Window* children = nullptr;
        unsigned count = 0;
        const bool queried = XQueryTree(connection.display, ancestor, &root, &parent, &children, &count);
        if (children) XFree(children);
        if (!connection.Sync() || !queried || parent == None || parent == root || parent == ancestor) break;
        ancestor = parent;
    }
    return false;
}

struct DestroyImage {
    void operator()(XImage* image) const { if (image) XDestroyImage(image); }
};

struct Channel {
    explicit Channel(unsigned long value) : mask(value), maximum(value) {
        while (maximum && !(maximum & 1)) { maximum >>= 1; ++shift; }
    }
    unsigned char Get(unsigned long pixel) const {
        const uint64_t value = (pixel & mask) >> shift;
        return static_cast<unsigned char>((value * 255 + maximum / 2) / maximum);
    }
    unsigned long mask;
    unsigned long maximum;
    unsigned shift = 0;
};

}  // namespace

std::vector<DesktopWindow> SnapshotWindows() {
    Connection connection;
    std::vector<DesktopWindow> windows;
    if (!connection.display || !connection.Composite()) return windows;
    for (int screen = 0; screen < std::min(ScreenCount(connection.display), 16); ++screen) {
        auto list = WindowList(connection, screen);
        if (!list.data) continue;
        const auto* ids = reinterpret_cast<const unsigned long*>(list.data.get());
        for (unsigned long index = list.count; index > 0 && windows.size() < kMaxWindows; --index) {
            XWindowAttributes attributes{};
            if (!Candidate(connection, ids[index - 1], attributes)) continue;
            std::string title = WindowTitle(connection.display, ids[index - 1]);
            if (connection.Sync()) windows.push_back({ids[index - 1], std::move(title)});
        }
    }
    return windows;
}

bool DesktopCaptureAvailable() {
    Connection connection;
    if (!connection.display || !connection.Composite()) return false;
    for (int screen = 0; screen < std::min(ScreenCount(connection.display), 16); ++screen) {
        const auto list = WindowList(connection, screen);
        if (list.data && list.count > 0) return true;
    }
    return false;
}

Result<void> CaptureDesktopWindow(uint64_t id, const std::string& png_path) {
    if (id == 0 || id > std::numeric_limits<uint32_t>::max())
        return Result<void>::Failure("Invalid X11 window id.");
    if (png_path.empty() || png_path.find('\0') != std::string::npos)
        return Result<void>::Failure("Invalid PNG output path.");
    Connection connection;
    if (!connection.display)
        return Result<void>::Failure("No X11 display; native Wayland window capture is unavailable.");
    if (!connection.Composite())
        return Result<void>::Failure("XComposite 0.2 or newer is required for window capture.");
    const Window window = static_cast<Window>(id);
    XWindowAttributes attributes{};
    if (!Candidate(connection, window, attributes))
        return Result<void>::Failure("Window closed, is hidden or owned by Spark, or exceeds 16 million pixels.");
    if (!connection.Compositor(XScreenNumberOfScreen(attributes.screen)))
        return Result<void>::Failure("No X11 compositing manager; occlusion-safe window capture is unavailable.");
    // Read only an existing composited backing pixmap. Redirecting a window
    // here or reading the root/window drawable could expose stale or occluded pixels.
    // Reparenting WMs often redirect the frame instead of its client. Crop the
    // client from that backing store, stopping before the root in every case.
    Backing backing;
    if (!FindBacking(connection, window, attributes, backing))
        return Result<void>::Failure("Window has no usable RGB composited backing pixmap; reopen the picker and retry.");
    const unsigned width = static_cast<unsigned>(attributes.width), height = static_cast<unsigned>(attributes.height);
    std::unique_ptr<XImage, DestroyImage> source(
        XGetImage(connection.display, backing.pixmap, backing.x, backing.y, width, height, AllPlanes, ZPixmap));
    if (!connection.Sync() || !source)
        return Result<void>::Failure("Could not read the composited window pixels.");
    wxImage image(static_cast<int>(width), static_cast<int>(height), false);
    if (!image.IsOk()) return Result<void>::Failure("Could not allocate the window image.");
    const Channel red(backing.visual->red_mask), green(backing.visual->green_mask),
                  blue(backing.visual->blue_mask);
    unsigned char* destination = image.GetData();
    for (unsigned row = 0; row < height; ++row) {
        for (unsigned column = 0; column < width; ++column) {
            const unsigned long pixel = XGetPixel(source.get(), static_cast<int>(column), static_cast<int>(row));
            *destination++ = red.Get(pixel);
            *destination++ = green.Get(pixel);
            *destination++ = blue.Get(pixel);
        }
    }
    wxMemoryOutputStream encoded;
    wxPNGHandler encoder;
    if (!encoder.SaveFile(&image, encoded, false) || !encoded.IsOk())
        return Result<void>::Failure("Could not encode the window image as PNG.");
    const size_t size = encoded.GetSize();
    if (size == 0 || size > 64u * 1024u * 1024u)
        return Result<void>::Failure("Encoded PNG is empty or exceeds 64 MiB.");
    std::string bytes(size, '\0');
    if (encoded.CopyTo(bytes.data(), bytes.size()) != bytes.size())
        return Result<void>::Failure("Could not retrieve the encoded PNG.");
    std::string error;
    // The common atomic writer stores the bytes verbatim, including NULs.
    if (!WriteFileUtf8Atomic(png_path, bytes, &error))
        return Result<void>::Failure("Could not save window PNG: " + error);
    return Result<void>::Success();
}

}  // namespace spark
#else

namespace spark {
std::vector<DesktopWindow> SnapshotWindows() { return {}; }
bool DesktopCaptureAvailable() { return false; }
Result<void> CaptureDesktopWindow(uint64_t, const std::string&) {
    return Result<void>::Failure("Window capture was built without X11/XComposite support; native Wayland capture is unavailable.");
}
}  // namespace spark

#endif
#endif
