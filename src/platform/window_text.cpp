#include "platform/window_text.h"

namespace spark {

std::wstring ReadWindowText(HWND window) {
    if (window == nullptr) return {};
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) return {};
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, text.data(), length + 1);
    if (copied <= 0) return {};
    text.resize(static_cast<size_t>(copied));
    return text;
}

}  // namespace spark
