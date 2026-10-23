#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace spark {
namespace markdown {

enum class MarkdownStyle {
    Body,
    Bold,
    Italic,
    BoldItalic,
    Code,
    Link,
    Muted,
    Heading,
    ListMarker,
};

struct MarkdownRun {
    std::wstring text;
    MarkdownStyle style = MarkdownStyle::Body;
};

struct MarkdownLine {
    std::vector<MarkdownRun> runs;
    bool code = false;
    bool rule = false;
    int indent = 0;
};

std::vector<MarkdownLine> ParseMarkdown(const std::wstring& source);
std::shared_ptr<const std::vector<MarkdownLine>> ParseMarkdownCached(const std::wstring& source);

// Incrementally renders a growing Markdown message. Completed top-level blocks
// are parsed once and retained; only the current uncommitted block is reparsed.
class StreamingMarkdown {
public:
    void Append(std::string_view utf8);
    void Finish();
    void Reset();

    const std::vector<MarkdownLine>& StableLines() const { return stable_lines_; }
    std::shared_ptr<const std::vector<MarkdownLine>> TailLines() const;
    std::string_view TailSource() const;
    size_t SourceBytes() const { return source_.size(); }
    bool Empty() const { return source_.empty(); }

private:
    void ScanCompleteLines();
    void CommitThrough(size_t end);

    std::string source_;
    size_t scanned_bytes_ = 0;
    size_t committed_bytes_ = 0;
    std::vector<MarkdownLine> stable_lines_;
    bool in_fence_ = false;
    char fence_marker_ = 0;
    size_t fence_length_ = 0;
    bool container_active_ = false;
    bool after_blank_line_ = false;
    size_t revision_ = 1;
    mutable size_t tail_cache_revision_ = 0;
    mutable std::shared_ptr<const std::vector<MarkdownLine>> tail_lines_;
};

}  // namespace markdown
}  // namespace spark
