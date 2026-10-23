#include "platform/journal_io.h"

#include <algorithm>
#include <limits>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace spark {

bool AppendJournalRecord(const std::string& path, const std::string& text, std::string* error) {
#ifdef _WIN32
    if (text.size() > std::numeric_limits<DWORD>::max()) {
        if (error) *error = "journal record exceeds the Windows write limit";
        return false;
    }
    HANDLE file = CreateFileW(Utf8ToWide(path).c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = FormatWin32Error(GetLastError());
        return false;
    }
    DWORD written = 0;
    const bool success = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                         written == text.size();
    if (!success && error) *error = FormatWin32Error(GetLastError());
    CloseHandle(file);
    return success;
#else
    const auto fail = [&](int code) {
        if (error) *error = FormatWin32Error(static_cast<uint32_t>(code));
        return false;
    };
    if (path.empty() || path.find('\0') != std::string::npos) return fail(EINVAL);
    int descriptor;
    do {
        descriptor = ::open(NativePath(path).c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NONBLOCK, 0600);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) return fail(errno);
    struct stat info {};
    const int inspected = ::fstat(descriptor, &info);
    if (inspected != 0 || !S_ISREG(info.st_mode)) {
        const int code = inspected != 0 ? errno : EINVAL;
        ::close(descriptor);
        return fail(code);
    }
    size_t offset = 0;
    while (offset < text.size()) {
        const auto count = ::write(descriptor, text.data() + offset,
                                  std::min<size_t>(text.size() - offset, 1024 * 1024));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            const int code = count == 0 ? EIO : errno;
            ::close(descriptor);
            return fail(code);
        }
        offset += static_cast<size_t>(count);
    }
    // An interrupted close may have already released the descriptor.
    return ::close(descriptor) == 0 || fail(errno);
#endif
}

Result<std::vector<std::string>> RecentJournalFiles(const std::string& directory, int limit) {
    if (limit <= 0) return Result<std::vector<std::string>>::Success({});
#ifdef _WIN32
    std::vector<std::pair<uint64_t, std::string>> entries;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(Utf8ToWide(JoinPath(directory, "*.jsonl")).c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return Result<std::vector<std::string>>::Success({});
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        ULARGE_INTEGER timestamp{};
        timestamp.LowPart = data.ftLastWriteTime.dwLowDateTime;
        timestamp.HighPart = data.ftLastWriteTime.dwHighDateTime;
        entries.emplace_back(timestamp.QuadPart, WideToUtf8(data.cFileName));
    } while (FindNextFileW(find, &data));
    FindClose(find);
#else
    std::vector<std::pair<spark::fs::file_time_type, std::string>> entries;
    std::error_code error;
    spark::fs::directory_iterator iterator(NativePath(directory), error), end;
    if (error == std::errc::no_such_file_or_directory) return Result<std::vector<std::string>>::Success({});
    if (error) return Result<std::vector<std::string>>::Failure(error.message());
    for (; iterator != end; iterator.increment(error)) {
        if (error) return Result<std::vector<std::string>>::Failure(error.message());
        if (iterator->path().extension() != ".jsonl") continue;
        if (!iterator->is_regular_file(error) || error) { error.clear(); continue; }
        const auto timestamp = iterator->last_write_time(error);
        if (!error) entries.emplace_back(timestamp, PathToUtf8(iterator->path().filename()));
        error.clear();
    }
    if (error) return Result<std::vector<std::string>>::Failure(error.message());
#endif
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.first > right.first;
    });
    std::vector<std::string> output;
    for (const auto& entry : entries) {
        output.push_back(entry.second);
        if (output.size() >= static_cast<size_t>(limit)) break;
    }
    return Result<std::vector<std::string>>::Success(std::move(output));
}

}  // namespace spark
