#pragma once

#include <cstdint>

#ifdef _WIN32
#include "platform/windows_compat.h"
#else
#include <chrono>
#endif

namespace spark {

// Monotonic milliseconds with a platform-dependent epoch. Persisted timestamps
// are historical only; elapsed time uses timestamps from the current process.
inline uint64_t MonotonicMillis() noexcept {
#ifdef _WIN32
    // Existing GDI elapsed-time consumers use the same Windows boot-time epoch.
    return windows_compat::TickMilliseconds();
#else
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

}  // namespace spark
