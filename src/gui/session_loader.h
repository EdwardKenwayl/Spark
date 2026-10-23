#pragma once

#include "core/session.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace spark {

// One reader coalesces rapid sidebar clicks into the most recently requested session.
class SessionLoader {
public:
    struct Loaded {
        uint64_t generation = 0;
        std::string path;
        SessionJournal journal;
        std::string error;
    };

    ~SessionLoader();
    uint64_t Request(const std::string& path);
    std::unique_ptr<Loaded> Poll();
    void Cancel();
    void Stop();

private:
    void ReadRequests();

    std::mutex mutex_;
    std::condition_variable changed_;
    std::thread worker_;
    CancellationToken cancellation_;
    uint64_t generation_ = 0;
    std::string pending_path_;
    std::unique_ptr<Loaded> loaded_;
    bool stopping_ = false;
};

}  // namespace spark
