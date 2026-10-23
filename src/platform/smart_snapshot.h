#pragma once

#include "core/common.h"

namespace spark {

struct SmartSnapshot {
    std::string image_path;
    std::string context;
};

// Synchronous rendering is used only by the isolated capture helper.
Result<SmartSnapshot> CaptureSmartSnapshot(HWND target, const std::string& image_path);
std::optional<int> RunSnapshotHelper();

class SnapshotCapture {
public:
    SnapshotCapture() = default;
    ~SnapshotCapture();
    SnapshotCapture(const SnapshotCapture&) = delete;
    SnapshotCapture& operator=(const SnapshotCapture&) = delete;
    Result<void> Start(HWND target, const std::string& directory);
    std::optional<Result<SmartSnapshot>> Poll();
    void Cancel();
    bool Pending() const { return process_ != nullptr; }

private:
    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
    uint64_t started_ = 0;
    SmartSnapshot snapshot_;
};

class SnapshotShortcut {
public:
    ~SnapshotShortcut();
    void Start(HWND owner, UINT message);
    void Stop();
    HWND Target() const;
    std::wstring TargetTitle() const;

private:
    friend struct AppWindowComposerTest;
    static void CALLBACK ForegroundChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    static LRESULT CALLBACK KeyboardProc(int code, WPARAM wparam, LPARAM lparam);
    void Remember(HWND window);
    inline static SnapshotShortcut* active_ = nullptr;
    HWND owner_ = nullptr;
    HWND target_ = nullptr;
    DWORD target_process_ = 0;
    UINT message_ = 0;
    HHOOK keyboard_ = nullptr;
    HWINEVENTHOOK foreground_ = nullptr;
    bool left_alt_ = false;
    bool right_alt_ = false;
    bool triggered_ = false;
};

}  // namespace spark
