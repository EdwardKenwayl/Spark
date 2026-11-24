#pragma once

#include <cstring>
#include <istream>
#include <string>
#include <vector>

namespace spark {

// Journal snapshots can be tens of megabytes. Copy blocks into the line rather
// than advancing the underlying stream buffer a character at a time.
class JournalLineReader {
public:
    explicit JournalLineReader(std::istream& stream) : stream_(stream), block_(256 * 1024) {}

    bool Next(std::string& line) {
        line.clear();
        for (;;) {
            if (position_ == available_) {
                stream_.read(block_.data(), static_cast<std::streamsize>(block_.size()));
                available_ = static_cast<size_t>(stream_.gcount());
                position_ = 0;
                if (available_ == 0) return !line.empty();
            }
            const char* first = block_.data() + position_;
            const auto* newline = static_cast<const char*>(std::memchr(first, '\n', available_ - position_));
            if (newline != nullptr) {
                const auto length = static_cast<size_t>(newline - first);
                line.append(first, length);
                position_ += length + 1;
                return true;
            }
            line.append(first, available_ - position_);
            position_ = available_;
        }
    }

private:
    std::istream& stream_;
    std::vector<char> block_;
    size_t position_ = 0;
    size_t available_ = 0;
};

}  // namespace spark
