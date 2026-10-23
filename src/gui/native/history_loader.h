#pragma once

#include "gui/transcript.h"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace spark {

class NativeHistoryLoader {
public:
    ~NativeHistoryLoader();
    // Same-root requests coalesce until the active scan's result is consumed by Poll.
    void Request(std::string data_directory);
    // nullopt means no result yet; an engaged empty vector is a completed empty scan.
    std::optional<std::vector<TranscriptHistory>> Poll();
    // Cancels and joins the worker. Further requests are ignored.
    void Stop();

private:
    void ReadRequests();

    std::mutex mutex_;
    std::condition_variable changed_;
    std::thread worker_;
    CancellationToken cancellation_;
    std::string directory_;
    std::optional<std::vector<TranscriptHistory>> ready_;
    uint64_t generation_ = 0;
    bool pending_ = false;
    bool working_ = false;
    bool stopping_ = false;
};

}  // namespace spark
