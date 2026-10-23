#include "gui/native/history_loader.h"

#include <algorithm>
#include <exception>
#include "platform/filesystem.h"
#include <map>
#include <utility>

namespace spark {
namespace {
constexpr int kHistoryLimit = 1000;
constexpr size_t kCacheLimit = 1024;

struct FileStamp {
    spark::fs::file_time_type modified;
    uintmax_t size = 0;
    bool operator==(const FileStamp& other) const {
        return modified == other.modified && size == other.size;
    }
};

std::optional<FileStamp> ReadStamp(const std::string& path) {
    const auto native = NativePath(path);
    std::error_code error;
    const auto modified = spark::fs::last_write_time(native, error);
    if (error) return std::nullopt;
    const auto size = spark::fs::file_size(native, error);
    if (error) return std::nullopt;
    return FileStamp{modified, size};
}

struct CachedSummary {
    FileStamp stamp;
    TranscriptHistory summary;
    uint64_t used = 0;
};

// Owned exclusively by the worker; Request/Poll never wait for journal I/O or cache operations.
struct SummaryCache {
    std::map<std::string, CachedSummary> entries;
    uint64_t clock = 0;

    std::vector<TranscriptHistory> Load(const std::string& directory, const CancellationToken& cancellation) {
        std::vector<TranscriptHistory> summaries;
        if (directory.empty() || cancellation.IsCancelled()) return summaries;
        const auto root = AbsolutePath(directory);
        if (root.empty()) return summaries;
        SessionJournal journal;
        journal.SetStorageRoot(root);
        const auto recent = journal.ListRecent(kHistoryLimit);
        if (!recent.ok) return summaries;
        summaries.reserve(recent.value.size());
        for (const auto& filename : recent.value) {
            if (cancellation.IsCancelled()) break;
            try {
                const auto path = JoinPath(JoinPath(root, "sessions"), filename);
                const auto before = ReadStamp(path);
                if (!before) continue;
                const auto cached = entries.find(path);
                if (cached != entries.end() && cached->second.stamp == *before) {
                    cached->second.used = ++clock;
                    summaries.push_back(cached->second.summary);
                    continue;
                }
                auto loaded = ReadTranscriptHistory(path, &cancellation);
                if (cancellation.IsCancelled()) break;
                if (!loaded.ok) continue;
                const auto after = ReadStamp(path);
                if (!after) continue;
                // Appends during a streaming read must not label a partial summary as up to date.
                if (*before == *after) {
                    if (cached == entries.end() && entries.size() >= kCacheLimit) {
                        const auto oldest = std::min_element(entries.begin(), entries.end(),
                            [](const auto& left, const auto& right) {
                                return left.second.used < right.second.used;
                            });
                        entries.erase(oldest);
                    }
                    entries.insert_or_assign(path, CachedSummary{*after, loaded.value, ++clock});
                }
                summaries.push_back(std::move(loaded.value));
            } catch (const std::exception&) {
                // Missing, unreadable, or concurrently replaced journals never abort the sidebar scan.
            }
        }
        return summaries;
    }
};
}  // namespace

NativeHistoryLoader::~NativeHistoryLoader() { Stop(); }

void NativeHistoryLoader::Request(std::string data_directory) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        // Periodic refreshes must neither cancel a long same-root read nor erase its unread result.
        if (data_directory == directory_ && (pending_ || working_ || ready_.has_value())) return;
        cancellation_.Cancel();
        directory_ = std::move(data_directory);
        ++generation_;
        pending_ = true;
        ready_.reset();
        if (!worker_.joinable()) worker_ = std::thread([this] { ReadRequests(); });
    }
    changed_.notify_one();
}

std::optional<std::vector<TranscriptHistory>> NativeHistoryLoader::Poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(ready_, std::nullopt);
}

void NativeHistoryLoader::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        ++generation_;
        pending_ = false;
        ready_.reset();
        cancellation_.Cancel();
    }
    changed_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void NativeHistoryLoader::ReadRequests() {
    SummaryCache cache;
    for (;;) {
        std::string directory;
        uint64_t generation;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [this] { return stopping_ || pending_; });
            if (stopping_) return;
            directory = directory_;
            generation = generation_;
            pending_ = false;
            working_ = true;
            cancellation_.Reset();
        }
        std::vector<TranscriptHistory> summaries;
        try {
            summaries = cache.Load(directory, cancellation_);
        } catch (const std::exception&) {
            // Directory enumeration errors are a completed, empty result, not a worker failure.
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            working_ = false;
            if (!stopping_ && generation == generation_ && !cancellation_.IsCancelled())
                ready_ = std::move(summaries);
        }
    }
}

}  // namespace spark
