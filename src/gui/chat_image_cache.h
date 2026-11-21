#pragma once

#include <windows.h>
#include <cstdint>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <string>

namespace spark {

// One decoder and a bounded thumbnail cache; never decode full chat history in Paint.
class ChatImageCache {
public:
    void Paint(HDC device, const RECT& tile, const std::string& path);
    bool Poll();
    bool Pending() const { return active_.valid() || !pending_.empty(); }
    void Clear();
    void Drain();
private:
    struct Image {
        HBITMAP bitmap = nullptr;
        int width = 0, height = 0;
        ~Image() { if (bitmap) DeleteObject(bitmap); }
    };
    struct Request { std::string path; uint64_t generation; };
    struct Result { Request request; std::shared_ptr<Image> image; };
    void Start();
    std::map<std::string, std::shared_ptr<Image>> cache_;
    std::deque<Request> pending_;
    std::future<Result> active_;
    std::string active_path_;
    uint64_t generation_ = 0;
};

}  // namespace spark
