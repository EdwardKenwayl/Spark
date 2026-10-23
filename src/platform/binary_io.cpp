#include "platform/binary_io.h"

#include "core/common.h"

#include <algorithm>
#include <cstdint>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace spark {
namespace {
constexpr size_t kMaxAttachmentBytes = 16u * 1024u * 1024u;
constexpr size_t kReadChunkBytes = 1u << 20;

#ifdef _WIN32
using NativeFile = HANDLE;
#else
using NativeFile = int;

template <typename Operation>
auto RetryInterrupted(Operation operation) {
    auto result = operation();
    for (int retry = 0; result < 0 && errno == EINTR && retry < 63; ++retry) {
        result = operation();
    }
    return result;
}
#endif

class ScopedFile {
public:
    explicit ScopedFile(NativeFile file) : value(file) {}
    ScopedFile(const ScopedFile&) = delete;
    ScopedFile& operator=(const ScopedFile&) = delete;
    ~ScopedFile() {
#ifdef _WIN32
        CloseHandle(value);
#else
        // Do not retry close: an interrupted call may have released the descriptor.
        ::close(value);
#endif
    }

    const NativeFile value;
};
}  // namespace

std::optional<std::string> ReadAttachmentBytes(std::string_view path) {
    if (path.empty() || path.find('\0') != std::string_view::npos) return std::nullopt;
    const auto native_path = NativePath(path);
    size_t length = 0;
#ifdef _WIN32
    const HANDLE handle = CreateFileW(native_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
    const ScopedFile file(handle);
    BY_HANDLE_FILE_INFORMATION information{};
    LARGE_INTEGER size{};
    if (GetFileType(file.value) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(file.value, &information) ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        !GetFileSizeEx(file.value, &size) || size.QuadPart <= 0 ||
        size.QuadPart > static_cast<LONGLONG>(kMaxAttachmentBytes)) {
        return std::nullopt;
    }
    length = static_cast<size_t>(size.QuadPart);
#else
    // Open first, without waiting for a FIFO writer, then inspect that same object.
    const int descriptor = RetryInterrupted([&] {
        return ::open(native_path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    });
    if (descriptor < 0) return std::nullopt;
    const ScopedFile file(descriptor);
    struct stat information {};
    if (RetryInterrupted([&] { return ::fstat(file.value, &information); }) != 0 ||
        !S_ISREG(information.st_mode) || information.st_size <= 0 ||
        static_cast<std::uintmax_t>(information.st_size) > kMaxAttachmentBytes) {
        return std::nullopt;
    }
    length = static_cast<size_t>(information.st_size);
#endif
    std::string bytes(length, '\0');
    size_t offset = 0;
    while (offset < bytes.size()) {
        const size_t requested = std::min<size_t>(bytes.size() - offset, kReadChunkBytes);
#ifdef _WIN32
        DWORD count = 0;
        if (!ReadFile(file.value, bytes.data() + offset, static_cast<DWORD>(requested), &count, nullptr) ||
            count == 0) {
            return std::nullopt;
        }
#else
        const auto count = RetryInterrupted([&] { return ::read(file.value, bytes.data() + offset, requested); });
        if (count <= 0) return std::nullopt;
#endif
        offset += static_cast<size_t>(count);
    }
    return bytes;
}

}  // namespace spark
