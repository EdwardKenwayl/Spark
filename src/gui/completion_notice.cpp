#include "gui/completion_notice.h"
#include "render/markdown.h"

#include <algorithm>

namespace spark {
namespace {

std::wstring Preview(const std::string& source, size_t limit) {
    size_t bytes = std::min(source.size(), size_t{8192});
    while (bytes < source.size() && bytes > 0 &&
           (static_cast<unsigned char>(source[bytes]) & 0xc0) == 0x80) --bytes;
    const auto lines = markdown::ParseMarkdown(Utf8ToWide(source.substr(0, bytes)));
    std::wstring text;
    for (const auto& line : lines) {
        for (const auto& run : line.runs) {
            for (wchar_t character : run.text) {
                if (character <= L' ' || character == 0x7f) {
                    if (!text.empty() && text.back() != L' ') text += L' ';
                } else {
                    text += character;
                }
                if (text.size() > limit) break;
            }
            if (text.size() > limit) break;
        }
        if (text.size() > limit) break;
        if (!text.empty() && text.back() != L' ') text += L' ';
    }
    while (!text.empty() && text.back() == L' ') text.pop_back();
    if (text.size() > limit || bytes < source.size()) {
        if (text.size() > limit) text.resize(limit);
        // UTF-16 Windows must not truncate between the two halves of an emoji.
        if (!text.empty() && text.back() >= 0xd800 && text.back() <= 0xdbff) text.pop_back();
        text += L"\u2026";
    }
    return text;
}

}  // namespace

std::optional<CompletionNotice> MakeCompletionNotice(const std::string& path, const SessionState& state) {
    if (path.empty() || state.metadata.contains("paused_turn")) return std::nullopt;
    const auto origin = state.metadata.find("origin");
    if (origin != state.metadata.end() && (*origin == "mcp" || *origin == "subagent")) return std::nullopt;
    for (auto message = state.messages.rbegin(); message != state.messages.rend(); ++message) {
        if (message->role == MessageRole::User) break;
        if (message->role != MessageRole::Assistant) continue;
        if (!message->tool_calls.empty()) return std::nullopt;
        auto body = Preview(message->content, 260);
        if (body.empty()) return std::nullopt;
        return CompletionNotice{path, Preview(state.title, 80), std::move(body)};
    }
    return std::nullopt;
}

}  // namespace spark
