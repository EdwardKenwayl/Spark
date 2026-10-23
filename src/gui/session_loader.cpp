#include "gui/session_loader.h"

namespace spark {

SessionLoader::~SessionLoader() { Stop(); }

uint64_t SessionLoader::Request(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    cancellation_.Cancel();
    loaded_.reset();
    pending_path_ = path;
    ++generation_;
    if (!worker_.joinable()) {
        stopping_ = false;
        worker_ = std::thread([this] { ReadRequests(); });
    }
    changed_.notify_one();
    return generation_;
}

std::unique_ptr<SessionLoader::Loaded> SessionLoader::Poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::move(loaded_);
}

void SessionLoader::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_;
    cancellation_.Cancel();
    pending_path_.clear();
    loaded_.reset();
}

void SessionLoader::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        ++generation_;
        cancellation_.Cancel();
        pending_path_.clear();
        loaded_.reset();
    }
    changed_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void SessionLoader::ReadRequests() {
    for (;;) {
        auto loaded = std::make_unique<Loaded>();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait(lock, [this] { return stopping_ || !pending_path_.empty(); });
            if (stopping_) return;
            loaded->generation = generation_;
            loaded->path = std::move(pending_path_);
            pending_path_.clear();
            cancellation_.Reset();
        }
        try {
            const auto result = loaded->journal.Resume(loaded->path, &cancellation_);
            if (!result.ok) loaded->error = result.error;
        } catch (const std::exception& exception) {
            loaded->error = exception.what();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!stopping_ && loaded->generation == generation_) loaded_ = std::move(loaded);
        }
    }
}

}  // namespace spark
