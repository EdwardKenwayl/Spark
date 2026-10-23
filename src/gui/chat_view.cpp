#include "gui/chat_view.h"

#include "core/common.h"
#include "gui/localization.h"
#include "gui/theme.h"
#include "platform/clock.h"
#include "render/markdown.h"

#include <nlohmann/json.hpp>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <ctime>
#include <limits>
#include <memory>

namespace spark {
namespace {

constexpr wchar_t kChatViewClass[] = L"SparkChatView";
COLORREF kBackground = RGB(28, 29, 31);
COLORREF kUserBubble = RGB(52, 53, 57);
COLORREF kToolPanel = RGB(31, 32, 35);
COLORREF kToolBorder = RGB(50, 51, 56);
COLORREF kText = RGB(238, 239, 242);
COLORREF kMuted = RGB(144, 147, 154);
COLORREF kSubtle = RGB(104, 107, 114);
COLORREF kError = RGB(242, 128, 128);
COLORREF kWarning = RGB(222, 146, 75);
COLORREF kSuccess = RGB(111, 190, 140);
COLORREF kRailIdle = RGB(74, 76, 82);
COLORREF kRailActive = RGB(150, 153, 160);
COLORREF kRailHover = RGB(244, 245, 248);
COLORREF kRailTip = RGB(46, 47, 51);
COLORREF kRailTipBorder = RGB(78, 79, 85);
COLORREF kAccent = RGB(142, 151, 168);

constexpr int kSideMargin = 22;
constexpr int kRailGutter = 24;
constexpr int kItemGap = 18;
constexpr int kMetaHeight = 18;
constexpr int kUserPaddingX = 14;
constexpr int kUserPaddingY = 11;
constexpr int kToolHeaderHeight = 28;
constexpr int kToolGap = 8;
constexpr int kContentBottomPadding = 28;
constexpr UINT_PTR kStreamRenderTimer = 2;
constexpr UINT kRestoreSpinnerDelayMs = 50;
constexpr uint64_t kRestoreSpinnerRevealDelayMs = 200;
constexpr UINT kRestorePrepareDelayMs = 10;
constexpr uint64_t kRestoreBudgetMs = 8;
constexpr size_t kRestoreRowsPerTick = 64;
// Codex drains its streaming queue on the target frame interval, so the backlog never
// ages far past one frame. Keep normal replies responsive, then back off for very
// large markdown blocks so a 4 GB Windows 7 machine does not reparse them at 30 Hz.
constexpr UINT kStreamRenderFastDelayMs = 33;
constexpr UINT kStreamRenderMediumDelayMs = 50;
constexpr UINT kStreamRenderSlowDelayMs = 80;
constexpr size_t kStreamRenderMediumBytes = 64 * 1024;
constexpr size_t kStreamRenderSlowBytes = 256 * 1024;
constexpr size_t kMaxReasoningBytes = 16384;
constexpr size_t kMaxReasoningSentenceBytes = 2048;
int g_dpi = 96;

bool IsAsciiWhitespace(char character) {
    return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

UINT StreamRenderDelay(size_t bytes) {
    if (bytes >= kStreamRenderSlowBytes) return kStreamRenderSlowDelayMs;
    if (bytes >= kStreamRenderMediumBytes) return kStreamRenderMediumDelayMs;
    return kStreamRenderFastDelayMs;
}

std::string QuotaNoticeText() {
    return WideToUtf8(i18n::Text(
        L"Quota exhausted: this API key has no remaining balance or quota. Recharge or switch API key/model, then retry."));
}

void TrimIncompleteUtf8Tail(std::string& text) {
    if (text.empty()) return;
    size_t index = text.size();
    while (index > 0 && (static_cast<unsigned char>(text[index - 1]) & 0xC0) == 0x80) --index;
    if (index == 0) {
        text.clear();
        return;
    }
    const unsigned char lead = static_cast<unsigned char>(text[index - 1]);
    const size_t expected = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : lead < 0xF8 ? 4 : 1;
    if (index - 1 + expected > text.size()) text.resize(index - 1);
}

void TrimReasoningBuffer(std::string& text) {
    if (text.size() <= kMaxReasoningBytes) return;
    text.erase(0, text.size() - kMaxReasoningBytes);
    size_t offset = 0;
    while (offset < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[offset]);
        const size_t expected = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : lead < 0xF8 ? 4 : 0;
        if (expected == 0 || offset + expected > text.size()) {
            ++offset;
            continue;
        }
        bool valid = true;
        for (size_t index = 1; index < expected; ++index) {
            if ((static_cast<unsigned char>(text[offset + index]) & 0xC0) != 0x80) {
                valid = false;
                break;
            }
        }
        if (valid) break;
        ++offset;
    }
    if (offset > 0) text.erase(0, offset);
}

bool IsCjkSentencePunctuationAt(const std::string& text, size_t index) {
    if (index + 2 >= text.size()) return false;
    const unsigned char first = static_cast<unsigned char>(text[index]);
    const unsigned char second = static_cast<unsigned char>(text[index + 1]);
    const unsigned char third = static_cast<unsigned char>(text[index + 2]);
    return (first == 0xE3 && second == 0x80 && third == 0x82) ||  // 。
           (first == 0xEF && second == 0xBC && third == 0x81) ||  // ！
           (first == 0xEF && second == 0xBC && third == 0x9F);    // ？
}

bool IsReasoningSentenceBoundary(const std::string& text, size_t index) {
    const unsigned char character = static_cast<unsigned char>(text[index]);
    if (character == '\r' || character == '\n') return true;
    if (IsCjkSentencePunctuationAt(text, index)) return true;
    if (character != '.' && character != '!' && character != '?') return false;
    return index + 1 >= text.size() || IsAsciiWhitespace(text[index + 1]);
}

std::string NormalizeReasoningBuffer(const std::string& text) {
    std::string normalized = SanitizeDisplayText(text);
    TrimReasoningBuffer(normalized);
    return normalized;
}

std::wstring CurrentReasoningSentence(const std::string& text) {
    size_t start = 0;
    for (size_t index = 0; index < text.size(); ++index) {
        if (IsReasoningSentenceBoundary(text, index)) start = index + 1;
    }
    while (start < text.size() && IsAsciiWhitespace(text[start])) ++start;

    std::string sentence = text.substr(start);
    bool has_content = false;
    for (char character : sentence) {
        if (!IsAsciiWhitespace(character)) {
            has_content = true;
            break;
        }
    }
    if (!has_content) sentence = text;

    for (char& character : sentence) {
        if (character == '\r' || character == '\n' || character == '\t') character = ' ';
    }
    if (sentence.size() > kMaxReasoningSentenceBytes) {
        sentence.resize(kMaxReasoningSentenceBytes);
        TrimIncompleteUtf8Tail(sentence);
    }
    return Utf8ToWide(sentence);
}

// Messages on the rail are only a navigation hint, so they are trimmed to a single line.
std::wstring RailPreview(const std::string& text) {
    std::wstring wide = Trim(Utf8ToWide(text));
    const size_t newline = wide.find_first_of(L"\r\n");
    if (newline != std::wstring::npos) {
        wide = wide.substr(0, newline) + L" …";
    }
    if (wide.size() > 72) wide = wide.substr(0, 72) + L"…";
    return wide;
}

struct ItemLayout {
    int top = 0;
    int bottom = 0;
    RECT message{};
    RECT bubble{};
    RECT text{};
    RECT meta{};
    RECT group_header{};
    RECT tool_header{};
    RECT tool_body{};
    RECT copy{};
    RECT edit{};
    RECT time{};
};

struct GroupInfo {
    bool leader = false;
    size_t size = 0;
    size_t last_member = 0;
    size_t finished = 0;
    size_t read_count = 0;
    size_t search_count = 0;
    size_t command_count = 0;
    uint64_t elapsed_ms = 0;
    bool hidden = false;
    bool indented = false;
    bool active = false;
    bool failed = false;
};

bool LooksLikeImageTool(const std::string& name) {
    const std::string lower = LowerAscii(name);
    return lower.find("image") != std::string::npos || lower.find("vision") != std::string::npos ||
           lower.find("screenshot") != std::string::npos || lower.find("photo") != std::string::npos ||
           lower.find("picture") != std::string::npos || lower.find("ocr") != std::string::npos;
}

std::wstring ToolLabel(const std::string& name) {
    if (name == "run_command") return i18n::Text(L"Ran command");
    if (LooksLikeImageTool(name)) return i18n::Text(L"Viewed image");
    if (name == "list_files") return i18n::Text(L"Listed files");
    if (name == "read_file") return i18n::Text(L"Read file");
    if (name == "read_skill") return i18n::Text(L"Read skill");
    if (name == "write_file") return i18n::Text(L"Wrote file");
    if (name == "apply_patch") return i18n::Text(L"Applied patch");
    if (name == "search_text") return i18n::Text(L"Searched text");
    if (name.empty()) return i18n::Text(L"Ran tool");
    return i18n::Format(L"Ran {tool}", {{L"tool", Utf8ToWide(name)}});
}

enum class ToolCategory {
    Command,
    Image,
    Read,
    Search,
    Write,
    Other,
};

ToolCategory CategoryOf(const std::string& tool_name) {
    if (tool_name == "run_command") return ToolCategory::Command;
    if (LooksLikeImageTool(tool_name)) return ToolCategory::Image;
    if (tool_name == "read_file" || tool_name == "list_files" || tool_name == "read_skill") return ToolCategory::Read;
    if (tool_name == "search_text") return ToolCategory::Search;
    if (tool_name == "write_file" || tool_name == "apply_patch") return ToolCategory::Write;
    return ToolCategory::Other;
}

std::string GroupKey(const std::string& tool_name) {
    switch (CategoryOf(tool_name)) {
        case ToolCategory::Command:
            return "command";
        case ToolCategory::Image:
            return "image";
        case ToolCategory::Read:
            return "read";
        case ToolCategory::Search:
            return "search";
        case ToolCategory::Write:
            return "write";
        case ToolCategory::Other:
            return "tool:" + LowerAscii(tool_name);
    }
    return "tool:" + LowerAscii(tool_name);
}

bool CanJoinActivityGroup(const ChatItem& previous, const ChatItem& next) {
    const ToolCategory previous_category = CategoryOf(previous.tool_name);
    const ToolCategory next_category = CategoryOf(next.tool_name);
    const bool previous_exploration = previous_category == ToolCategory::Command ||
                                      previous_category == ToolCategory::Read || previous_category == ToolCategory::Search;
    const bool next_exploration =
        next_category == ToolCategory::Command || next_category == ToolCategory::Read || next_category == ToolCategory::Search;
    if (previous_exploration && next_exploration) return true;
    return GroupKey(previous.tool_name) == GroupKey(next.tool_name);
}

std::wstring JoinFragments(const std::vector<std::wstring>& fragments) {
    if (fragments.empty()) return {};
    if (fragments.size() == 1) return fragments.front();
    if (fragments.size() == 2) {
        return i18n::Format(L"{first}, {second}", {{L"first", fragments[0]}, {L"second", fragments[1]}});
    }
    return i18n::Format(L"{first}, {second}, {third}",
                        {{L"first", fragments[0]}, {L"second", fragments[1]}, {L"third", fragments[2]}});
}

std::wstring GroupLabel(const GroupInfo& group, const std::string& key) {
    const std::wstring count = std::to_wstring(group.size);
    if (group.read_count == 0 && group.search_count == 0 && group.command_count == 0) {
        const wchar_t* msgid = group.active ? L"Running {n} tools" : L"Ran {n} tools";
        if (key == "command") msgid = group.active ? L"Running {n} commands" : L"Ran {n} commands";
        else if (key == "image") msgid = group.active ? L"Viewing {n} images" : L"Viewed {n} images";
        else if (key == "read") msgid = group.active ? L"Reading {n} files" : L"Read {n} files";
        else if (key == "search") msgid = group.active ? L"Searching {n} times" : L"Searched {n} times";
        else if (key == "write") msgid = group.active ? L"Modifying {n} files" : L"Modified {n} files";
        return i18n::Format(msgid, {{L"n", count}});
    }

    if (!group.active && group.read_count == 1 && group.command_count == 1 && group.search_count == 0) {
        return i18n::Text(L"Read a file and ran a command");
    }

    std::vector<std::wstring> fragments;
    const auto add = [&](size_t amount, const wchar_t* singular, const wchar_t* plural) {
        if (amount == 0) return;
        if (amount == 1) {
            fragments.emplace_back(i18n::Text(singular));
            return;
        }
        fragments.push_back(i18n::Format(plural, {{L"n", std::to_wstring(amount)}}));
    };
    add(group.read_count, group.active ? L"Reading a file" : L"Read a file",
        group.active ? L"Reading {n} files" : L"Read {n} files");
    add(group.search_count, group.active ? L"Searching text" : L"Searched text",
        group.active ? L"Searching {n} times" : L"Searched {n} times");
    add(group.command_count, group.active ? L"Running a command" : L"Ran a command",
        group.active ? L"Running {n} commands" : L"Ran {n} commands");
    return JoinFragments(fragments);
}

std::wstring ToolSummary(const ChatItem& item);

std::wstring ActiveToolSummary(const std::vector<ChatItem>& items, const GroupInfo& group, size_t leader) {
    const size_t end = std::min(items.size(), group.last_member + 1);
    for (size_t index = leader; index < end; ++index) {
        if (items[index].kind != ChatItemKind::Tool || items[index].finished) continue;
        const std::wstring summary = ToolSummary(items[index]);
        if (!summary.empty()) return summary;
    }
    return {};
}

std::wstring InlinePreview(std::string value, size_t limit = 120) {
    for (char& character : value) {
        if (character == '\r' || character == '\n' || character == '\t') character = ' ';
    }
    std::wstring text = Trim(Utf8ToWide(value));
    if (text.size() > limit) text = text.substr(0, limit) + L"...";
    return text;
}

// Live status text grows at its end, so an oversized line is trimmed from the left;
// finished rows keep their head so they do not jump when the row settles.
std::wstring FitText(HDC device, const std::wstring& text, int max_width, bool keep_tail) {
    if (text.empty() || max_width <= 0) return {};
    SIZE extent{};
    if (!GetTextExtentPoint32W(device, text.c_str(), static_cast<int>(text.size()), &extent)) return text;
    if (extent.cx <= max_width) return text;
    const std::wstring ellipsis = L"...";
    SIZE ellipsis_extent{};
    GetTextExtentPoint32W(device, ellipsis.c_str(), 1, &ellipsis_extent);
    const int budget = max_width - ellipsis_extent.cx;
    if (budget <= 0) return ellipsis;
    size_t start = text.size();
    size_t count = 0;
    int width = 0;
    while (start > 0 && count < text.size()) {
        SIZE glyph{};
        const size_t index = keep_tail ? start - 1 : count;
        if (!GetTextExtentPoint32W(device, text.c_str() + index, 1, &glyph)) break;
        if (width + glyph.cx > budget) break;
        width += glyph.cx;
        if (keep_tail) {
            --start;
        } else {
            ++count;
        }
    }
    if (keep_tail) return ellipsis + text.substr(start);
    return text.substr(0, count) + ellipsis;
}

std::wstring ToolSummary(const ChatItem& item) {
    try {
        const nlohmann::json arguments = nlohmann::json::parse(item.arguments.empty() ? "{}" : item.arguments);
        if (item.tool_name == "run_command") return InlinePreview(arguments.value("command", std::string{}));
        if (item.tool_name == "read_file" || item.tool_name == "write_file" || item.tool_name == "view_image") {
            return InlinePreview(arguments.value("path", std::string{}));
        }
        if (item.tool_name == "list_files") return InlinePreview(arguments.value("path", std::string(".")));
        if (item.tool_name == "read_skill") {
            const std::string name = arguments.value("name", std::string{});
            const std::string path = arguments.value("path", std::string{});
            return InlinePreview(path.empty() ? name : name + "/" + path);
        }
        if (item.tool_name == "search_text") return InlinePreview(arguments.value("query", std::string{}));
        if (item.tool_name == "apply_patch") return InlinePreview(arguments.value("path", std::string{}));
    } catch (const nlohmann::json::exception&) {
    }
    return InlinePreview(item.arguments, 90);
}

std::wstring FormatDuration(uint64_t milliseconds) {
    if (milliseconds < 1000) {
        return i18n::Format(L"{milliseconds}ms", {{L"milliseconds", std::to_wstring(milliseconds)}});
    }
    const uint64_t seconds = milliseconds / 1000;
    if (seconds < 60) {
        return i18n::Format(L"{seconds}s",
                            {{L"seconds", std::to_wstring(seconds) + L"." +
                                              std::to_wstring((milliseconds / 100) % 10)}});
    }
    const uint64_t minutes = seconds / 60;
    return i18n::Format(L"{minutes}m {seconds}s",
                        {{L"minutes", std::to_wstring(minutes)}, {L"seconds", std::to_wstring(seconds % 60)}});
}

std::wstring FormatTurnDuration(uint64_t milliseconds) {
    const uint64_t seconds = milliseconds / 1000;
    if (seconds < 60) return i18n::Format(L"{seconds} sec", {{L"seconds", std::to_wstring(seconds)}});
    const uint64_t minutes = seconds / 60;
    if (minutes < 60) {
        return i18n::Format(L"{minutes} min {seconds} sec",
                            {{L"minutes", std::to_wstring(minutes)}, {L"seconds", std::to_wstring(seconds % 60)}});
    }
    const uint64_t hours = minutes / 60;
    return i18n::Format(L"{hours} hr {minutes} min",
                        {{L"hours", std::to_wstring(hours)}, {L"minutes", std::to_wstring(minutes % 60)}});
}

std::wstring FormatTokenCount(uint64_t tokens) {
    std::wstring text = std::to_wstring(tokens);
    int insert_at = static_cast<int>(text.size()) - 3;
    while (insert_at > 0) {
        text.insert(static_cast<size_t>(insert_at), 1, L',');
        insert_at -= 3;
    }
    return text;
}

std::wstring FormatUsageDuration(uint64_t milliseconds) {
    wchar_t buffer[32]{};
    if (milliseconds < 60000) {
        swprintf_s(buffer, L"%.1f", static_cast<double>(milliseconds) / 1000.0);
        return i18n::Format(L"{seconds}s", {{L"seconds", buffer}});
    }
    const uint64_t seconds = milliseconds / 1000;
    const uint64_t minutes = seconds / 60;
    if (minutes < 60) {
        return i18n::Format(L"{minutes}m {seconds}s",
                            {{L"minutes", std::to_wstring(minutes)}, {L"seconds", std::to_wstring(seconds % 60)}});
    }
    return i18n::Format(L"{hours}h {minutes}m",
                        {{L"hours", std::to_wstring(minutes / 60)}, {L"minutes", std::to_wstring(minutes % 60)}});
}

std::wstring FormatUsageStats(const ChatItem& item) {
    const uint64_t total_tokens = item.input_tokens + item.output_tokens;
    const double cache_rate = item.input_tokens == 0
                                  ? 0.0
                                  : std::min(100.0, 100.0 * static_cast<double>(item.cached_input_tokens) /
                                                        static_cast<double>(item.input_tokens));
    const double context_rate = item.context_limit_tokens == 0
                                    ? 0.0
                                    : std::min(100.0, 100.0 * static_cast<double>(item.context_tokens) /
                                                          static_cast<double>(item.context_limit_tokens));
    wchar_t cache_buffer[32]{};
    wchar_t context_buffer[32]{};
    swprintf_s(cache_buffer, L"%.1f%%", cache_rate);
    swprintf_s(context_buffer, L"%.1f%%", context_rate);
    return i18n::Format(
        L"Turn total {total} · Input {input} · Output {output} · Cache read {cached} · Cache hit {cache_rate} · Context {context}/{limit} ({context_rate}) · Calls {calls} · Elapsed {elapsed}",
        {{L"total", FormatTokenCount(total_tokens)}, {L"input", FormatTokenCount(item.input_tokens)},
         {L"output", FormatTokenCount(item.output_tokens)}, {L"cached", FormatTokenCount(item.cached_input_tokens)},
         {L"cache_rate", cache_buffer}, {L"context", FormatTokenCount(item.context_tokens)},
         {L"limit", FormatTokenCount(item.context_limit_tokens)}, {L"context_rate", context_buffer},
         {L"calls", std::to_wstring(item.model_calls)}, {L"elapsed", FormatUsageDuration(item.elapsed_ms)}});
}

uint64_t ToolElapsed(const ChatItem& item) {
    if (item.finished) return item.elapsed_ms;
    if (item.start_tick_ms == 0) return 0;
    return MonotonicMillis() - item.start_tick_ms;
}

std::vector<GroupInfo> ComputeGroups(const std::vector<ChatItem>& items) {
    std::vector<GroupInfo> groups(items.size());
    size_t index = 0;
    while (index < items.size()) {
        if (items[index].kind != ChatItemKind::Tool) {
            ++index;
            continue;
        }
        size_t end = index;
        size_t count = 1;
        // Codex keeps one exploration cell alive across hidden reasoning. Treat a
        // reasoning row as transparent here as well, while every real transcript
        // item (reply, compaction, error, user turn) still ends the activity group.
        while (end + 1 < items.size()) {
            size_t next = end + 1;
            while (next < items.size() && items[next].kind == ChatItemKind::Reasoning) ++next;
            if (next >= items.size() || items[next].kind != ChatItemKind::Tool ||
                !CanJoinActivityGroup(items[end], items[next])) {
                break;
            }
            end = next;
            ++count;
        }
        if (count > 1) {
            groups[index].leader = true;
            groups[index].size = count;
            groups[index].last_member = end;
            for (size_t member = index; member <= end; ++member) {
                if (items[member].kind != ChatItemKind::Tool) continue;
                if (items[member].finished) {
                    ++groups[index].finished;
                    groups[index].elapsed_ms += items[member].elapsed_ms;
                } else {
                    groups[index].active = true;
                    groups[index].elapsed_ms += items[member].start_tick_ms == 0
                                                   ? 0
                                                   : MonotonicMillis() - items[member].start_tick_ms;
                }
                if (items[member].finished && !items[member].success) groups[index].failed = true;
                switch (CategoryOf(items[member].tool_name)) {
                    case ToolCategory::Read:
                        ++groups[index].read_count;
                        break;
                    case ToolCategory::Search:
                        ++groups[index].search_count;
                        break;
                    case ToolCategory::Command:
                        ++groups[index].command_count;
                        break;
                    case ToolCategory::Image:
                    case ToolCategory::Write:
                    case ToolCategory::Other:
                        break;
                }
            }
            const bool collapsed = items[index].group_collapsed;
            for (size_t member = index + 1; member <= end; ++member) {
                if (items[member].kind != ChatItemKind::Tool) continue;
                groups[member].hidden = collapsed;
                groups[member].indented = true;
            }
        }
        index = end + 1;
    }
    return groups;
}

bool IsCopyableAssistant(const std::vector<ChatItem>& items, size_t index) {
    return index < items.size() && items[index].kind == ChatItemKind::Assistant && items[index].final_assistant;
}

struct ContentBounds {
    int left = 0;
    int right = 0;
    int width = 0;
};

ContentBounds CalculateContentBounds(int client_width) {
    const int available_left = kSideMargin + kRailGutter;
    const int available_right = std::max(available_left + 120, client_width - kSideMargin);
    const int available_width = available_right - available_left;
    const int column_width = std::min(920, available_width);
    const int column_left = available_left + (available_width - column_width) / 2;
    return ContentBounds{column_left, column_left + column_width, column_width};
}

int DpiScale(int pixels);

std::wstring FormatTime(uint64_t timestamp) {
    if (timestamp == 0) return {};
    const std::time_t now = static_cast<std::time_t>(timestamp);
    std::tm local{};
    if (localtime_s(&local, &now) != 0) return {};
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"%02d:%02d", local.tm_hour, local.tm_min);
    return buffer;
}

int MeasureTextHeight(HDC device, HFONT font, const std::wstring& text, int width) {
    if (text.empty() || width <= 0) return 0;
    HFONT previous = static_cast<HFONT>(SelectObject(device, font));
    RECT rect{0, 0, width, 0};
    DrawTextW(device, text.c_str(), -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    SelectObject(device, previous);
    return std::max(0, static_cast<int>(rect.bottom - rect.top));
}

int MeasureTextWidth(HDC device, HFONT font, const std::wstring& text) {
    if (text.empty()) return 0;
    HFONT previous = static_cast<HFONT>(SelectObject(device, font));
    SIZE size{};
    GetTextExtentPoint32W(device, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(device, previous);
    return size.cx;
}

void DrawWrappedText(HDC device, HFONT font, const std::wstring& text, const RECT& rect, COLORREF color) {
    if (text.empty()) return;
    HFONT previous = static_cast<HFONT>(SelectObject(device, font));
    SetBkMode(device, TRANSPARENT);
    SetTextColor(device, color);
    RECT target = rect;
    DrawTextW(device, text.c_str(), -1, &target, DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    SelectObject(device, previous);
}

void DrawRoundPanel(HDC device, const RECT& rect, COLORREF fill, COLORREF border, int radius) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_brush = SelectObject(device, brush);
    HGDIOBJ old_pen = SelectObject(device, pen);
    RoundRect(device, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(device, old_pen);
    SelectObject(device, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawToolbarTooltip(HDC device, HFONT font, const RECT& client, const RECT& anchor, const wchar_t* text) {
    const int margin = DpiScale(4);
    const int padding_x = DpiScale(8);
    const int padding_y = DpiScale(5);
    const int gap = DpiScale(4);
    const int max_width = std::min(DpiScale(240), static_cast<int>(client.right - client.left) -
                                                   margin * 2 - padding_x * 2);
    if (max_width <= 0 || client.bottom - client.top <= margin * 2 + padding_y * 2) return;
    HFONT previous = static_cast<HFONT>(SelectObject(device, font));
    RECT measured{0, 0, max_width, 0};
    DrawTextW(device, text, -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    const int width = std::min(max_width, static_cast<int>(measured.right)) + padding_x * 2;
    const int height = std::min(static_cast<int>(client.bottom - client.top) - margin * 2,
                               static_cast<int>(measured.bottom) + padding_y * 2);
    const int left = std::max(static_cast<int>(client.left) + margin,
                              std::min(static_cast<int>((anchor.left + anchor.right) / 2) - width / 2,
                                       static_cast<int>(client.right) - margin - width));
    int top = anchor.top - gap - height;
    if (top < client.top + margin) top = anchor.bottom + gap;
    top = std::max(static_cast<int>(client.top) + margin,
                   std::min(top, static_cast<int>(client.bottom) - margin - height));
    const RECT box{left, top, left + width, top + height};
    DrawRoundPanel(device, box, kRailTip, kRailTipBorder, DpiScale(6));
    SetTextColor(device, kText);
    RECT text_rect{box.left + padding_x, box.top + padding_y, box.right - padding_x, box.bottom - padding_y};
    DrawTextW(device, text, -1, &text_rect, DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(device, previous);
}

void DrawActivityGlyph(HDC device, int center_x, int center_y, int radius, int frame, COLORREF active_color) {
    constexpr int kSegments = 8;
    const int dot = std::max(2, DpiScale(2));
    for (int index = 0; index < kSegments; ++index) {
        const int relative = (index - frame + kSegments * 4) % kSegments;
        const int strength = relative == 0 ? 255 : relative == 1 ? 165 : relative == 2 ? 90 : 35;
        const int angle = index * 45;
        const double radians = static_cast<double>(angle) * 3.14159265358979323846 / 180.0;
        const int x = center_x + static_cast<int>(radius * std::cos(radians) + 0.5);
        const int y = center_y + static_cast<int>(radius * std::sin(radians) + 0.5);
        const int inverse = 255 - strength;
        const COLORREF color = RGB((GetRValue(kSubtle) * inverse + GetRValue(active_color) * strength) / 255,
                                   (GetGValue(kSubtle) * inverse + GetGValue(active_color) * strength) / 255,
                                   (GetBValue(kSubtle) * inverse + GetBValue(active_color) * strength) / 255);
        HBRUSH brush = CreateSolidBrush(color);
        RECT rect{x - dot / 2, y - dot / 2, x - dot / 2 + dot, y - dot / 2 + dot};
        FillRect(device, &rect, brush);
        DeleteObject(brush);
    }
}

void DrawRestoreLoader(HDC device, const RECT& client, int frame) {
    const int radius = DpiScale(16);
    const int center_x = (client.left + client.right) / 2;
    const int center_y = (client.top + client.bottom) / 2;
    const int thickness = std::max(2, DpiScale(3));
    HGDIOBJ old_brush = SelectObject(device, GetStockObject(HOLLOW_BRUSH));
    HPEN track = CreatePen(PS_SOLID, thickness, kToolBorder);
    HGDIOBJ old_pen = SelectObject(device, track);
    Ellipse(device, center_x - radius, center_y - radius, center_x + radius, center_y + radius);
    HPEN arc = CreatePen(PS_SOLID, thickness, kMuted);
    SelectObject(device, arc);
    const double angle = frame * 3.14159265358979323846 / 12.0;
    const POINT start{center_x + static_cast<LONG>(radius * std::cos(angle)),
                      center_y + static_cast<LONG>(radius * std::sin(angle))};
    const POINT end{center_x + static_cast<LONG>(radius * std::cos(angle + 1.7)),
                    center_y + static_cast<LONG>(radius * std::sin(angle + 1.7))};
    const int old_direction = SetArcDirection(device, AD_CLOCKWISE);
    Arc(device, center_x - radius, center_y - radius, center_x + radius, center_y + radius,
        start.x, start.y, end.x, end.y);
    SetArcDirection(device, old_direction);
    SelectObject(device, old_pen);
    SelectObject(device, old_brush);
    DeleteObject(arc);
    DeleteObject(track);
}

// Codex renders its live status line as plain dim text with a white highlight sweeping
// from left to right. Colouring each glyph individually reproduces that sweep without a
// mask bitmap, and the phase comes from the wall clock so the speed stays constant no
// matter how often the activity timer fires.
void DrawShimmerText(HDC device, const std::wstring& text, const RECT& bounds, COLORREF base,
                     COLORREF highlight) {
    if (text.empty() || bounds.right <= bounds.left) return;
    SIZE extent{};
    if (!GetTextExtentPoint32W(device, text.c_str(), static_cast<int>(text.size()), &extent)) return;
    const int visible_width = std::min<int>(extent.cx, bounds.right - bounds.left);
    if (visible_width <= 0) return;
    const int band = std::max(DpiScale(30), visible_width / 3);
    const int travel = visible_width + band * 2;
    const int center =
        bounds.left + static_cast<int>((MonotonicMillis() * 320 / 1000) % static_cast<uint64_t>(travel)) - band;
    const int baseline = bounds.top + ((bounds.bottom - bounds.top) - extent.cy) / 2;
    SetBkMode(device, TRANSPARENT);
    int x = bounds.left;
    for (size_t index = 0; index < text.size();) {
        size_t next = index + 1;
        if (text[index] >= 0xD800 && text[index] <= 0xDBFF && next < text.size() && text[next] >= 0xDC00 &&
            text[next] <= 0xDFFF) {
            ++next;
        }
        const std::wstring character = text.substr(index, next - index);
        SIZE glyph{};
        if (!GetTextExtentPoint32W(device, character.c_str(), static_cast<int>(character.size()), &glyph)) break;
        const int glyph_center = x + glyph.cx / 2;
        const int distance = glyph_center >= center ? glyph_center - center : center - glyph_center;
        int strength = 0;
        if (distance < band) {
            const int linear = (band - distance) * 255 / band;
            strength = linear * linear / 255;
        }
        const int inverse = 255 - strength;
        SetTextColor(device, RGB((GetRValue(base) * inverse + GetRValue(highlight) * strength) / 255,
                                 (GetGValue(base) * inverse + GetGValue(highlight) * strength) / 255,
                                 (GetBValue(base) * inverse + GetBValue(highlight) * strength) / 255));
        ExtTextOutW(device, x, baseline, 0, nullptr, character.c_str(), static_cast<UINT>(character.size()), nullptr);
        x += glyph.cx;
        if (x >= bounds.right) break;
        index = next;
    }
}

void DrawSuccessGlyph(HDC device, int center_x, int center_y, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, std::max(1, DpiScale(1)), color);
    HGDIOBJ old_pen = SelectObject(device, pen);
    MoveToEx(device, center_x - DpiScale(4), center_y, nullptr);
    LineTo(device, center_x - DpiScale(1), center_y + DpiScale(3));
    LineTo(device, center_x + DpiScale(5), center_y - DpiScale(4));
    SelectObject(device, old_pen);
    DeleteObject(pen);
}

void DrawFailureGlyph(HDC device, int center_x, int center_y, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, std::max(1, DpiScale(1)), color);
    HGDIOBJ old_pen = SelectObject(device, pen);
    const int radius = DpiScale(4);
    MoveToEx(device, center_x - radius, center_y - radius, nullptr);
    LineTo(device, center_x + radius, center_y + radius);
    MoveToEx(device, center_x + radius, center_y - radius, nullptr);
    LineTo(device, center_x - radius, center_y + radius);
    SelectObject(device, old_pen);
    DeleteObject(pen);
}

using markdown::MarkdownLine;
using markdown::MarkdownRun;
using markdown::MarkdownStyle;

struct ChatFonts {
    HFONT text = nullptr;
    HFONT bold = nullptr;
    HFONT italic = nullptr;
    HFONT bold_italic = nullptr;
    HFONT mono = nullptr;
    HFONT heading = nullptr;
};

int DpiScale(int pixels) {
    return MulDiv(pixels, g_dpi, 96);
}

HFONT DeriveFont(HFONT base, LONG weight, bool italic, int scale_numerator = 1, int scale_denominator = 1) {
    if (base == nullptr) return nullptr;
    LOGFONTW font{};
    if (GetObjectW(base, sizeof(font), &font) == 0) return nullptr;
    font.lfWeight = weight;
    font.lfItalic = italic ? TRUE : FALSE;
    if (scale_numerator != scale_denominator) {
        font.lfHeight = MulDiv(font.lfHeight, scale_numerator, scale_denominator);
    }
    font.lfQuality = CLEARTYPE_QUALITY;
    return CreateFontIndirectW(&font);
}

HFONT MarkdownFontForStyle(const ChatFonts& fonts, MarkdownStyle style, bool code_block) {
    if (code_block) return fonts.mono != nullptr ? fonts.mono : fonts.text;
    switch (style) {
        case MarkdownStyle::Bold:
            return fonts.bold != nullptr ? fonts.bold : fonts.text;
        case MarkdownStyle::Italic:
            return fonts.italic != nullptr ? fonts.italic : fonts.text;
        case MarkdownStyle::BoldItalic:
            return fonts.bold_italic != nullptr ? fonts.bold_italic : fonts.text;
        case MarkdownStyle::Code:
            return fonts.mono != nullptr ? fonts.mono : fonts.text;
        case MarkdownStyle::Heading:
            return fonts.heading != nullptr ? fonts.heading : fonts.text;
        default:
            return fonts.text;
    }
}

COLORREF MarkdownColorForStyle(MarkdownStyle style, bool code_block) {
    if (code_block) return kMuted;
    switch (style) {
        case MarkdownStyle::Code:
        case MarkdownStyle::Link:
            return kAccent;
        case MarkdownStyle::Muted:
        case MarkdownStyle::ListMarker:
            return kMuted;
        default:
            return kText;
    }
}

int MeasureRunToken(HDC device, HFONT font, const std::wstring& text) {
    if (font == nullptr || text.empty()) return 0;
    HFONT previous = static_cast<HFONT>(SelectObject(device, font));
    SIZE size{};
    GetTextExtentPoint32W(device, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(device, previous);
    return size.cx;
}

int RunLineHeight(HDC device, HFONT font) {
    if (font == nullptr) return DpiScale(18);
    HFONT previous = static_cast<HFONT>(SelectObject(device, font));
    TEXTMETRICW metrics{};
    GetTextMetricsW(device, &metrics);
    SelectObject(device, previous);
    return std::max(DpiScale(14), static_cast<int>(metrics.tmHeight + metrics.tmExternalLeading));
}

int RenderStyledRuns(HDC device, const std::vector<MarkdownRun>& runs, const RECT& bounds, const ChatFonts& fonts,
                     bool code_block, bool draw) {
    if (runs.empty()) return DpiScale(8);
    int x = bounds.left;
    int y = bounds.top;
    int line_height = DpiScale(18);
    const int right = bounds.right;
    HFONT previous_font = nullptr;
    const auto new_line = [&]() {
        y += line_height + DpiScale(2);
        x = bounds.left;
        line_height = DpiScale(18);
    };
    const auto draw_token = [&](const std::wstring& token, MarkdownStyle style, HFONT font, COLORREF color) {
        const int token_width = MeasureRunToken(device, font, token);
        if (draw) {
            if (style == MarkdownStyle::Code && !code_block) {
                RECT background{x - DpiScale(2), y, x + token_width + DpiScale(2), y + line_height};
                HBRUSH brush = CreateSolidBrush(kToolPanel);
                FillRect(device, &background, brush);
                DeleteObject(brush);
            }
            previous_font = static_cast<HFONT>(SelectObject(device, font));
            SetTextColor(device, color);
            TextOutW(device, x, y, token.c_str(), static_cast<int>(token.size()));
            SelectObject(device, previous_font);
        }
        x += token_width;
        line_height = std::max(line_height, RunLineHeight(device, font));
    };

    SetBkMode(device, TRANSPARENT);
    for (const MarkdownRun& run : runs) {
        const HFONT font = MarkdownFontForStyle(fonts, run.style, code_block);
        const COLORREF color = MarkdownColorForStyle(run.style, code_block);
        size_t start = 0;
        while (start < run.text.size()) {
            const bool whitespace = std::iswspace(run.text[start]) != 0;
            size_t end = start + 1;
            while (end < run.text.size() && (std::iswspace(run.text[end]) != 0) == whitespace) ++end;
            const std::wstring token = run.text.substr(start, end - start);
            const int token_width = MeasureRunToken(device, font, token);
            if (!whitespace && x > bounds.left && x + token_width > right) new_line();
            if (whitespace && x == bounds.left) {
                start = end;
                continue;
            }
            if (!whitespace && token_width > std::max(1, static_cast<int>(right - bounds.left)) && token.size() > 1) {
                for (size_t index = 0; index < token.size();) {
                    size_t next = index + 1;
                    if (token[index] >= 0xD800 && token[index] <= 0xDBFF && next < token.size() &&
                        token[next] >= 0xDC00 && token[next] <= 0xDFFF) {
                        ++next;
                    }
                    const std::wstring piece = token.substr(index, next - index);
                    const int piece_width = MeasureRunToken(device, font, piece);
                    if (x > bounds.left && x + piece_width > right) new_line();
                    draw_token(piece, run.style, font, color);
                    index = next;
                }
            } else {
                draw_token(token, run.style, font, color);
            }
            start = end;
        }
    }
    return y + line_height - bounds.top;
}

int MeasureMarkdownLines(HDC device, const std::vector<MarkdownLine>& lines, int width, const ChatFonts& fonts) {
    int height = 0;
    for (const MarkdownLine& line : lines) {
        if (line.rule) {
            height += DpiScale(12);
            continue;
        }
        if (line.runs.empty()) {
            height += DpiScale(8);
            continue;
        }
        RECT bounds{0, 0, std::max(1, width - line.indent * DpiScale(14)), 0};
        height += RenderStyledRuns(device, line.runs, bounds, fonts, line.code, false);
        if (line.code) height += DpiScale(4);
    }
    return height;
}

int MeasureMarkdownHeight(HDC device, const std::wstring& source, int width, const ChatFonts& fonts) {
    const std::shared_ptr<const std::vector<MarkdownLine>> lines = markdown::ParseMarkdownCached(source);
    return std::max(DpiScale(20), MeasureMarkdownLines(device, *lines, width, fonts));
}

int MeasureStreamingMarkdownHeight(HDC device, const markdown::StreamingMarkdown& stream, int width,
                                   const ChatFonts& fonts) {
    const std::vector<MarkdownLine>& stable = stream.StableLines();
    int height = MeasureMarkdownLines(device, stable, width, fonts);
    if (!stream.TailSource().empty()) {
        if (!stable.empty()) height += DpiScale(8);
        const std::shared_ptr<const std::vector<MarkdownLine>> tail = stream.TailLines();
        height += MeasureMarkdownLines(device, *tail, width, fonts);
    }
    return std::max(DpiScale(20), height);
}

void DrawMarkdownLines(HDC device, const std::vector<MarkdownLine>& lines, const RECT& bounds, const ChatFonts& fonts,
                       int* y_position) {
    int y = *y_position;
    for (const MarkdownLine& line : lines) {
        const int left = bounds.left + line.indent * DpiScale(14);
        if (line.rule) {
            HPEN pen = CreatePen(PS_SOLID, 1, kToolBorder);
            HGDIOBJ previous = SelectObject(device, pen);
            MoveToEx(device, left, y + DpiScale(5), nullptr);
            LineTo(device, bounds.right, y + DpiScale(5));
            SelectObject(device, previous);
            DeleteObject(pen);
            y += DpiScale(12);
            continue;
        }
        if (line.runs.empty()) {
            y += DpiScale(8);
            continue;
        }
        RECT line_bounds{left, y, bounds.right, bounds.bottom};
        const int line_height = RenderStyledRuns(device, line.runs, line_bounds, fonts, line.code, false);
        if (line.code) {
            RECT background{bounds.left, y - DpiScale(2), bounds.right, y + line_height + DpiScale(2)};
            HBRUSH brush = CreateSolidBrush(kToolPanel);
            FillRect(device, &background, brush);
            DeleteObject(brush);
            HPEN accent = CreatePen(PS_SOLID, DpiScale(2), kAccent);
            HGDIOBJ previous = SelectObject(device, accent);
            MoveToEx(device, bounds.left, y, nullptr);
            LineTo(device, bounds.left, y + line_height);
            SelectObject(device, previous);
            DeleteObject(accent);
        }
        RenderStyledRuns(device, line.runs, line_bounds, fonts, line.code, true);
        y += line_height + (line.code ? DpiScale(4) : 0);
    }
    *y_position = y;
}

void DrawMarkdown(HDC device, const std::wstring& source, const RECT& bounds, const ChatFonts& fonts) {
    const std::shared_ptr<const std::vector<MarkdownLine>> lines = markdown::ParseMarkdownCached(source);
    int y = bounds.top;
    DrawMarkdownLines(device, *lines, bounds, fonts, &y);
}

void DrawStreamingMarkdown(HDC device, const markdown::StreamingMarkdown& stream, const RECT& bounds,
                           const ChatFonts& fonts) {
    int y = bounds.top;
    const std::vector<MarkdownLine>& stable = stream.StableLines();
    DrawMarkdownLines(device, stable, bounds, fonts, &y);
    if (!stream.TailSource().empty()) {
        if (!stable.empty()) y += DpiScale(8);
        const std::shared_ptr<const std::vector<MarkdownLine>> tail = stream.TailLines();
        DrawMarkdownLines(device, *tail, bounds, fonts, &y);
    }
}

void DrawCopyIcon(HDC device, const RECT& rect, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_pen = SelectObject(device, pen);
    HGDIOBJ old_brush = SelectObject(device, GetStockObject(NULL_BRUSH));
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int box_w = std::max(7, width - 5);
    const int box_h = std::max(7, height - 5);
    Rectangle(device, rect.left + 1, rect.top + 4, rect.left + 1 + box_w, rect.top + 4 + box_h);
    Rectangle(device, rect.left + 4, rect.top + 1, rect.left + 4 + box_w, rect.top + 1 + box_h);
    SelectObject(device, old_brush);
    SelectObject(device, old_pen);
    DeleteObject(pen);
}

void DrawEditIcon(HDC device, const RECT& rect, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_pen = SelectObject(device, pen);
    const int center_y = (rect.top + rect.bottom) / 2;
    MoveToEx(device, rect.left + 3, center_y + 5, nullptr);
    LineTo(device, rect.left + 5, center_y + 1);
    LineTo(device, rect.right - 2, center_y - 6);
    LineTo(device, rect.right - 5, center_y - 2);
    LineTo(device, rect.left + 3, center_y + 5);
    MoveToEx(device, rect.left + 5, center_y + 1, nullptr);
    LineTo(device, rect.left + 7, center_y + 5);
    SelectObject(device, old_pen);
    DeleteObject(pen);
}

void DrawTerminalIcon(HDC device, HFONT font, const RECT& rect, COLORREF color) {
    HFONT previous = static_cast<HFONT>(SelectObject(device, font));
    SetBkMode(device, TRANSPARENT);
    SetTextColor(device, color);
    RECT text_rect = rect;
    DrawTextW(device, L">_", -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(device, previous);
}

void DrawChevron(HDC device, const RECT& rect, bool expanded, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_pen = SelectObject(device, pen);
    const int center_x = (rect.left + rect.right) / 2;
    const int center_y = (rect.top + rect.bottom) / 2;
    if (expanded) {
        MoveToEx(device, center_x - 4, center_y - 1, nullptr);
        LineTo(device, center_x, center_y + 3);
        LineTo(device, center_x + 4, center_y - 1);
    } else {
        MoveToEx(device, center_x - 1, center_y - 4, nullptr);
        LineTo(device, center_x + 3, center_y);
        LineTo(device, center_x - 1, center_y + 4);
    }
    SelectObject(device, old_pen);
    DeleteObject(pen);
}

void DrawCompactionIcon(HDC device, int center_x, int center_y, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_pen = SelectObject(device, pen);
    const int horizontal = DpiScale(6);
    const int vertical = DpiScale(5);
    MoveToEx(device, center_x - horizontal, center_y - vertical, nullptr);
    LineTo(device, center_x + horizontal, center_y - vertical);
    MoveToEx(device, center_x - horizontal, center_y + vertical, nullptr);
    LineTo(device, center_x + horizontal, center_y + vertical);
    MoveToEx(device, center_x, center_y - vertical - DpiScale(3), nullptr);
    LineTo(device, center_x, center_y + vertical + DpiScale(3));
    SelectObject(device, old_pen);
    DeleteObject(pen);
}

std::wstring ToolDetails(const ChatItem& item) {
    std::string details;
    if (item.tool_name == "run_command") {
        try {
            const nlohmann::json arguments = nlohmann::json::parse(item.arguments.empty() ? "{}" : item.arguments);
            details = "$ " + arguments.value("command", std::string{});
        } catch (const nlohmann::json::exception&) {
            details = item.arguments;
        }
    } else {
        details = "[" + item.tool_name + "]\n" + item.arguments;
    }
    if (!item.result.empty()) details += details.empty() ? item.result : "\n\n" + item.result;
    return Utf8ToWide(details);
}

ItemLayout LayoutItem(HDC device, const std::vector<ChatItem>& items, const std::vector<GroupInfo>& groups,
                      const std::vector<markdown::StreamingMarkdown>& streams, size_t index, size_t last_user, int top,
                      int client_width, const ChatFonts& fonts, HFONT time_font, int known_markdown_height = -1) {
    const ChatItem& item = items[index];
    const GroupInfo& group = groups[index];
    ItemLayout layout;
    layout.top = top;
    layout.bottom = top;
    if (!item.visible) return layout;
    const ContentBounds content = CalculateContentBounds(client_width);
    const int content_left = content.left;
    const int content_right = content.right;
    const int content_width = content.width;
    const int max_text_width = std::max(120, content_width);

    if (item.kind == ChatItemKind::User) {
        const int bubble_max = std::max(180, std::min(max_text_width, static_cast<int>(content_width * 0.78)));
        const int text_width = std::max(80, bubble_max - kUserPaddingX * 2);
        const std::wstring text = Utf8ToWide(item.text);
        const int text_height = std::max(20, MeasureTextHeight(device, fonts.text, text, text_width));
        const int measured_width = std::max(80, std::min(bubble_max, MeasureTextWidth(device, fonts.text, text) + 2));
        const int bubble_width = std::min(bubble_max, std::max(120, measured_width + kUserPaddingX * 2));
        const int bubble_height = text_height + kUserPaddingY * 2;
        layout.bubble = RECT{content_right - bubble_width, top, content_right, top + bubble_height};
        layout.text = RECT{layout.bubble.left + kUserPaddingX, layout.bubble.top + kUserPaddingY,
                           layout.bubble.right - kUserPaddingX, layout.bubble.bottom - kUserPaddingY};
        layout.meta = RECT{layout.bubble.left, layout.bubble.bottom + 3, content_right, layout.bubble.bottom + 3 + kMetaHeight};
        const std::wstring time = FormatTime(item.timestamp);
        const int time_width = MeasureTextWidth(device, time_font, time);
        if (index == last_user) {
            const int edit_right = layout.meta.right;
            layout.edit = RECT{edit_right - 18, layout.meta.top + 1, edit_right, layout.meta.bottom - 1};
            const int copy_right = layout.edit.left - 6;
            layout.copy = RECT{copy_right - 18, layout.meta.top + 1, copy_right, layout.meta.bottom - 1};
            const int time_right = layout.copy.left - 8;
            layout.time = RECT{time_right - time_width, layout.meta.top, time_right, layout.meta.bottom};
            layout.message = RECT{std::min(layout.edit.left, layout.bubble.left), layout.bubble.top, content_right,
                                  layout.meta.bottom};
        } else {
            const int copy_right = layout.meta.right;
            layout.copy = RECT{copy_right - 18, layout.meta.top + 1, copy_right, layout.meta.bottom - 1};
            const int time_right = layout.copy.left - 8;
            layout.time = RECT{time_right - time_width, layout.meta.top, time_right, layout.meta.bottom};
            layout.message = RECT{std::min(layout.copy.left, layout.bubble.left), layout.bubble.top, content_right,
                                  layout.meta.bottom};
        }
        layout.bottom = layout.meta.bottom + kItemGap;
        return layout;
    }

    if (item.kind == ChatItemKind::UsageStats) {
        const int padding_x = DpiScale(10);
        const int padding_y = DpiScale(7);
        const int text_width = std::max(80, content_width - padding_x * 2);
        const int text_height =
            std::max(DpiScale(18), MeasureTextHeight(device, fonts.text, FormatUsageStats(item), text_width));
        layout.bubble = RECT{content_left, top, content_right, top + text_height + padding_y * 2};
        layout.text = RECT{layout.bubble.left + padding_x, layout.bubble.top + padding_y,
                           layout.bubble.right - padding_x, layout.bubble.bottom - padding_y};
        layout.message = layout.bubble;
        layout.bottom = layout.bubble.bottom + DpiScale(10);
        return layout;
    }

    if (item.kind == ChatItemKind::TurnStatus) {
        const int width = max_text_width;
        const int text_height = std::max(22, MeasureTextHeight(device, fonts.text, L"Ag", width));
        layout.text = RECT{content_left, top, content_left + width - 24, top + text_height};
        layout.message = RECT{content_left, top, content_left + width, top + text_height};
        layout.bottom = layout.text.bottom + 10;
        return layout;
    }

    if (item.kind == ChatItemKind::Reasoning) {
        const int width = max_text_width;
        const int text_height = std::max(20, MeasureTextHeight(device, fonts.text, L"Ag", width));
        layout.text = RECT{content_left, top, content_left + width, top + text_height};
        layout.message = layout.text;
        layout.bottom = layout.text.bottom + kItemGap - 4;
        return layout;
    }

    if (item.kind == ChatItemKind::Compaction) {
        const int width = max_text_width;
        const int text_height = std::max(22, MeasureTextHeight(device, fonts.text, L"Ag", width));
        layout.text = RECT{content_left + DpiScale(22), top, content_left + width, top + text_height};
        layout.message = RECT{content_left, top, content_left + width, top + text_height};
        layout.bottom = layout.message.bottom + DpiScale(6);
        return layout;
    }

    if (item.kind == ChatItemKind::Tool) {
        if (group.hidden && !group.leader) {
            layout.bottom = top;
            return layout;
        }
        const int width = std::min(max_text_width, content_width);
        int cursor = top;
        if (group.leader) {
            layout.group_header = RECT{content_left, cursor, content_left + width, cursor + kToolHeaderHeight};
            if (item.group_collapsed) {
                layout.message = layout.group_header;
                layout.bottom = layout.group_header.bottom + kToolGap;
                return layout;
            }
            cursor = layout.group_header.bottom + 4;
        }
        const int indent = (group.leader || group.indented) ? 18 : 0;
        layout.tool_header = RECT{content_left + indent, cursor, content_left + width, cursor + kToolHeaderHeight};
        int bottom = layout.tool_header.bottom;
        if (item.expanded) {
            const std::wstring details = ToolDetails(item);
            const int details_width = std::max(120, width - indent - 28);
            const int details_height = std::max(24, MeasureTextHeight(device, fonts.mono, details, details_width));
            layout.tool_body = RECT{content_left + indent + 12, layout.tool_header.bottom + 3, content_left + width - 12,
                                    layout.tool_header.bottom + 3 + details_height};
            bottom = layout.tool_body.bottom + 8;
        }
        layout.message = RECT{content_left, top, content_left + width, bottom};
        layout.bottom = bottom + kToolGap;
        return layout;
    }

    const int width = max_text_width;
    int text_height = known_markdown_height;
    if (text_height < 0) {
        if (item.kind == ChatItemKind::Assistant && index < streams.size()) {
            text_height = MeasureStreamingMarkdownHeight(device, streams[index], width, fonts);
        } else {
            text_height = MeasureMarkdownHeight(device, Utf8ToWide(item.text), width, fonts);
        }
    }
    text_height = std::max(20, text_height);
    layout.text = RECT{content_left, top, content_left + width, top + text_height};
    if (item.kind == ChatItemKind::Assistant && IsCopyableAssistant(items, index)) {
        layout.meta = RECT{content_left, layout.text.bottom + 3, content_left + width, layout.text.bottom + 3 + kMetaHeight};
        layout.copy = RECT{layout.meta.left, layout.meta.top + 1, layout.meta.left + 18, layout.meta.bottom - 1};
        const std::wstring time = FormatTime(item.timestamp);
        const int time_width = MeasureTextWidth(device, time_font, time);
        layout.time = RECT{layout.meta.right - time_width, layout.meta.top, layout.meta.right, layout.meta.bottom};
        layout.message = RECT{layout.text.left, layout.text.top, layout.meta.right, layout.meta.bottom};
        layout.bottom = layout.meta.bottom + kItemGap;
    } else {
        layout.message = layout.text;
        layout.bottom = layout.text.bottom + kItemGap;
    }
    return layout;
}

}  // namespace

struct ChatView::LayoutCache {
    enum class Stage { Groups, Heights, Rails, Complete };

    std::vector<GroupInfo> groups;
    bool groups_dirty = true;
    bool restart_heights = false;
    bool remeasure_all = false;
    Stage stage = Stage::Groups;
    size_t cursor = 0;
    size_t group_first = static_cast<size_t>(-1);
    size_t group_previous = static_cast<size_t>(-1);
    int content_y = 18;
    int viewport = 0;
    int rail_top = 0;
    int rail_bottom = 0;
    int rail_gap = 0;
    int rail_previous = std::numeric_limits<int>::min();
    std::vector<std::pair<size_t, int>> turns;
};

ChatView::ChatView() = default;

ChatView::~ChatView() {
    CancelRestore();
    // A side panel may outlive its ChatView owner. Destroy the HWND while its
    // window procedure can still safely access this object.
    if (IsWindow(window_)) DestroyWindow(window_);
    ReleaseBackBuffer();
    if (bold_font_ != nullptr) DeleteObject(bold_font_);
    if (italic_font_ != nullptr) DeleteObject(italic_font_);
    if (bold_italic_font_ != nullptr) DeleteObject(bold_italic_font_);
    if (heading_font_ != nullptr) DeleteObject(heading_font_);
    if (time_font_ != nullptr) DeleteObject(time_font_);
}

bool ChatView::Create(HWND parent, HINSTANCE instance, int control_id, HFONT text_font, HFONT mono_font) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = WindowProc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = nullptr;
        window_class.lpszClassName = kChatViewClass;
        if (RegisterClassExW(&window_class) == 0) return false;
        registered = true;
    }
    text_font_ = text_font;
    bold_font_ = DeriveFont(text_font_, FW_BOLD, false);
    italic_font_ = DeriveFont(text_font_, FW_NORMAL, true);
    bold_italic_font_ = DeriveFont(text_font_, FW_BOLD, true);
    heading_font_ = DeriveFont(text_font_, FW_BOLD, false, 5, 4);
    mono_font_ = mono_font;
    HDC screen = GetDC(nullptr);
    dpi_ = screen != nullptr ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
    g_dpi = dpi_;
    const int time_height = -MulDiv(8, dpi_, 72);
    ReleaseDC(nullptr, screen);
    time_font_ = CreateFontW(time_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    window_ = CreateWindowExW(0, kChatViewClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                              0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)), instance,
                              this);
    if (window_ != nullptr && loading_) {
        restore_spinner_timer_ = next_restore_timer_++;
        SetTimer(window_, restore_spinner_timer_, kRestoreSpinnerDelayMs, nullptr);
        if (!restoring_) {
            restore_prepare_timer_ = next_restore_timer_++;
            SetTimer(window_, restore_prepare_timer_, kRestorePrepareDelayMs, nullptr);
        }
    }
    return window_ != nullptr;
}

void ChatView::SetTheme(bool dark) {
    const theme::Palette palette = dark ? theme::DarkPalette() : theme::LightPalette();
    kBackground = palette.background;
    kUserBubble = dark ? RGB(52, 53, 57) : RGB(236, 237, 239);
    kToolPanel = dark ? RGB(31, 32, 35) : RGB(247, 247, 248);
    kToolBorder = palette.border;
    kText = palette.text;
    kMuted = palette.muted;
    kSubtle = palette.subtle;
    kError = palette.error;
    kWarning = palette.warning;
    kRailIdle = palette.rail_idle;
    kRailActive = dark ? RGB(150, 153, 160) : RGB(112, 115, 120);
    kRailHover = palette.rail_hover;
    kRailTip = dark ? RGB(46, 47, 51) : RGB(255, 255, 255);
    kRailTipBorder = dark ? RGB(78, 79, 85) : RGB(196, 198, 201);
    kAccent = dark ? RGB(142, 151, 168) : RGB(70, 78, 92);
    Rebuild();
}

void ChatView::RefreshLocalization() {
    for (size_t index = 0; index < items_.size(); ++index) {
        ChatItem& item = items_[index];
        if (item.kind == ChatItemKind::Quota) {
            const std::string text = QuotaNoticeText();
            if (item.text != text) {
                item.text = text;
                InvalidateItemHeight(index);
                rail_dirty_ = true;
            }
        }
        if (item.kind == ChatItemKind::Compaction) {
            const std::string text = WideToUtf8(item.success ? i18n::Text(L"Context automatically compacted")
                                                            : i18n::Text(L"Context manually compacted"));
            if (item.text != text) {
                item.text = text;
                InvalidateItemHeight(index);
                rail_dirty_ = true;
            }
        }
        if (item.kind == ChatItemKind::UsageStats) {
            // Preserve transcript and Markdown caches; only translated wrapping changes.
            InvalidateItemHeight(index);
            rail_dirty_ = true;
        }
    }
    Rebuild();
    if (window_ != nullptr) InvalidateRect(window_, nullptr, FALSE);
}

void ChatView::SetActivityFrame(int frame) {
    if (activity_frame_ == frame) return;
    activity_frame_ = frame;
    if (loading_) return;
    // Active group elapsed times are refreshed on the existing live animation.
    if (layout_cache_ != nullptr) layout_cache_->groups_dirty = true;
    if (activity_visible_) {
        if (window_ != nullptr) InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    for (const ChatItem& item : items_) {
        if (item.kind == ChatItemKind::Tool && !item.finished) {
            if (window_ != nullptr) InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        if (item.kind == ChatItemKind::Reasoning && !item.finished) {
            if (window_ != nullptr) InvalidateRect(window_, nullptr, FALSE);
            return;
        }
        if (item.kind == ChatItemKind::TurnStatus && !item.finished) {
            if (window_ != nullptr) InvalidateRect(window_, nullptr, FALSE);
            return;
        }
    }
}

void ChatView::SetActivityState(bool active, const std::wstring& label, uint64_t start_tick_ms) {
    const bool has_start = start_tick_ms != 0;
    const uint64_t resolved_start =
        active ? (has_start ? start_tick_ms : (activity_visible_ ? activity_start_tick_ms_ : MonotonicMillis())) : 0;
    if (activity_visible_ == active && activity_label_ == label &&
        (!active || !has_start || activity_start_tick_ms_ == start_tick_ms)) {
        return;
    }
    const bool becoming_visible = active && !activity_visible_;
    if (active) {
        activity_start_tick_ms_ = resolved_start;
    } else {
        activity_start_tick_ms_ = 0;
    }
    activity_visible_ = active;
    activity_label_ = label;
    if (window_ == nullptr || loading_) return;
    // Recomputing the full transcript height on every animation tick is what made long
    // sessions starve the UI thread. Only a newly visible status row needs auto-scroll;
    // label changes just repaint the row in place.
    if (becoming_visible) ScrollToBottom();
    else InvalidateRect(window_, nullptr, FALSE);
}

void ChatView::SetActions(Action toggle_tool, Action copy_item, Action edit_item) {
    toggle_tool_ = std::move(toggle_tool);
    copy_item_ = std::move(copy_item);
    edit_item_ = std::move(edit_item);
}

void ChatView::SetGroupAction(Action toggle_group) { toggle_group_ = std::move(toggle_group); }

void ChatView::Clear() {
    CancelRestore();
    if (window_ != nullptr) KillTimer(window_, kStreamRenderTimer);
    stream_render_pending_ = false;
    items_.clear();
    item_heights_.clear();
    item_text_heights_.clear();
    streaming_markdown_.clear();
    markdown_ready_.clear();
    layout_cache_.reset();
    rail_dashes_.clear();
    heights_dirty_ = true;
    content_height_ = 18;
    last_user_index_ = static_cast<size_t>(-1);
    layout_client_width_ = -1;
    scroll_y_ = 0;
    auto_scroll_enabled_ = true;
    activity_visible_ = false;
    activity_label_.clear();
    activity_start_tick_ms_ = 0;
    rail_dirty_ = true;
    hover_item_ = static_cast<size_t>(-1);
    hover_type_ = HitArea::Type::None;
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void ChatView::CancelRestore() {
    if (window_ != nullptr) {
        if (restore_spinner_timer_ != 0) KillTimer(window_, restore_spinner_timer_);
        if (restore_prepare_timer_ != 0) KillTimer(window_, restore_prepare_timer_);
    }
    restore_spinner_timer_ = 0;
    restore_prepare_timer_ = 0;
    restoring_ = false;
    loading_ = false;
    restore_started_tick_ = 0;
}

void ChatView::BeginRestore(uint64_t started_tick_ms) {
    Clear();
    restoring_ = true;
    loading_ = true;
    restore_started_tick_ = started_tick_ms == 0 ? MonotonicMillis() : started_tick_ms;
    restore_frame_ = 0;
    layout_cache_ = std::make_unique<LayoutCache>();
    if (window_ != nullptr) {
        restore_spinner_timer_ = next_restore_timer_++;
        SetTimer(window_, restore_spinner_timer_, kRestoreSpinnerDelayMs, nullptr);
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void ChatView::EndRestore() {
    if (!loading_ || !restoring_) return;
    restoring_ = false;
    if (window_ == nullptr) return;
    restore_prepare_timer_ = next_restore_timer_++;
    SetTimer(window_, restore_prepare_timer_, kRestorePrepareDelayMs, nullptr);
}

void ChatView::PushItem(ChatItem item) {
    const bool assistant = item.kind == ChatItemKind::Assistant;
    items_.push_back(std::move(item));
    item_heights_.push_back(-1);
    item_text_heights_.push_back(-1);
    streaming_markdown_.emplace_back();
    markdown_ready_.push_back(!assistant || !loading_);
    if (assistant && !loading_ && !items_.back().text.empty()) streaming_markdown_.back().Append(items_.back().text);
    if (items_.back().kind == ChatItemKind::User) last_user_index_ = items_.size() - 1;
    heights_dirty_ = true;
    if (layout_cache_ != nullptr) {
        layout_cache_->groups.emplace_back();
        layout_cache_->groups_dirty = true;
    }
}

size_t ChatView::AddUser(const std::string& text, uint64_t timestamp) {
    ChatItem item;
    item.kind = ChatItemKind::User;
    item.text = SanitizeDisplayText(text);
    item.timestamp = timestamp == 0 ? UnixTimeSeconds() : timestamp;
    PushItem(std::move(item));
    rail_dirty_ = true;
    // Sending a message always starts a new live turn, even when the user was
    // previously reading an older part of the conversation.
    auto_scroll_enabled_ = true;
    ScrollToBottom();
    return items_.size() - 1;
}

size_t ChatView::AddTurnStatus(uint64_t timestamp, uint64_t start_tick_ms) {
    ChatItem item;
    item.kind = ChatItemKind::TurnStatus;
    item.finished = false;
    item.expanded = true;
    item.timestamp = timestamp == 0 ? UnixTimeSeconds() : timestamp;
    item.start_tick_ms = start_tick_ms == 0 ? MonotonicMillis() : start_tick_ms;
    PushItem(std::move(item));
    rail_dirty_ = true;
    ScrollToBottom();
    return items_.size() - 1;
}

size_t ChatView::AddAssistant(const std::string& text, uint64_t timestamp) {
    ChatItem item;
    item.kind = ChatItemKind::Assistant;
    item.text = SanitizeDisplayText(text);
    item.finished = false;
    item.timestamp = timestamp == 0 ? UnixTimeSeconds() : timestamp;
    PushItem(std::move(item));
    rail_dirty_ = true;
    ScrollToBottom();
    return items_.size() - 1;
}

size_t ChatView::AddReasoning(const std::string& text, uint64_t timestamp) {
    ChatItem item;
    item.kind = ChatItemKind::Reasoning;
    item.text = NormalizeReasoningBuffer(text);
    item.finished = false;
    item.timestamp = timestamp;
    PushItem(std::move(item));
    rail_dirty_ = true;
    ScrollToBottom();
    return items_.size() - 1;
}

size_t ChatView::AddCompactionNotice(bool automatic, uint64_t timestamp) {
    ChatItem item;
    item.kind = ChatItemKind::Compaction;
    item.success = automatic;
    item.text = WideToUtf8(i18n::Text(automatic ? L"Context automatically compacted" : L"Context manually compacted"));
    item.timestamp = timestamp == 0 ? UnixTimeSeconds() : timestamp;
    PushItem(std::move(item));
    rail_dirty_ = true;
    ScrollToBottom();
    return items_.size() - 1;
}

size_t ChatView::AddError(const std::string& text, uint64_t timestamp) {
    ChatItem item;
    item.kind = ChatItemKind::Error;
    item.text = SanitizeDisplayText(text);
    item.success = false;
    item.timestamp = timestamp == 0 ? UnixTimeSeconds() : timestamp;
    for (size_t index = items_.size(); index > 0; --index) {
        const auto kind = items_[index - 1].kind;
        if (kind == ChatItemKind::User) break;
        if (kind != ChatItemKind::Error && kind != ChatItemKind::Quota) continue;
        items_[index - 1] = std::move(item);
        InvalidateItemHeight(index - 1);
        rail_dirty_ = true;
        ScrollToBottom();
        return index - 1;
    }
    PushItem(std::move(item));
    rail_dirty_ = true;
    ScrollToBottom();
    return items_.size() - 1;
}

size_t ChatView::AddQuotaNotice(uint64_t timestamp) {
    ChatItem item;
    item.kind = ChatItemKind::Quota;
    item.text = QuotaNoticeText();
    item.success = false;
    item.timestamp = timestamp == 0 ? UnixTimeSeconds() : timestamp;
    PushItem(std::move(item));
    rail_dirty_ = true;
    ScrollToBottom();
    return items_.size() - 1;
}

size_t ChatView::AddUsageStats(uint64_t input_tokens, uint64_t output_tokens, uint64_t cached_input_tokens,
                               uint64_t model_calls, uint64_t context_tokens, uint64_t context_limit_tokens,
                               uint64_t elapsed_ms, uint64_t timestamp) {
    ChatItem item;
    item.kind = ChatItemKind::UsageStats;
    item.finished = true;
    item.timestamp = timestamp == 0 ? UnixTimeSeconds() : timestamp;
    item.input_tokens = input_tokens;
    item.output_tokens = output_tokens;
    item.cached_input_tokens = cached_input_tokens;
    item.model_calls = model_calls;
    item.context_tokens = context_tokens;
    item.context_limit_tokens = context_limit_tokens;
    item.elapsed_ms = elapsed_ms;
    PushItem(std::move(item));
    rail_dirty_ = true;
    ScrollToBottom();
    return items_.size() - 1;
}

size_t ChatView::AddTool(const std::string& name, const std::string& arguments, uint64_t timestamp) {
    ChatItem item;
    item.kind = ChatItemKind::Tool;
    item.tool_name = SanitizeDisplayText(name);
    item.arguments = SanitizeDisplayText(arguments);
    item.finished = false;
    item.start_tick_ms = MonotonicMillis();
    item.timestamp = timestamp == 0 ? UnixTimeSeconds() : timestamp;
    PushItem(std::move(item));
    rail_dirty_ = true;
    ScrollToBottom();
    return items_.size() - 1;
}

void ChatView::AppendAssistant(size_t index, const std::string& text) {
    if (index >= items_.size() || items_[index].kind != ChatItemKind::Assistant) return;
    const std::string sanitized = SanitizeDisplayText(text);
    items_[index].text += sanitized;
    if (loading_) {
        markdown_ready_[index] = false;
    } else if (markdown_ready_[index]) {
        streaming_markdown_[index].Append(sanitized);
    } else {
        PrepareAssistantMarkdown(index);
    }
    InvalidateItemHeight(index);
    if (loading_) return;
    if (window_ != nullptr && !stream_render_pending_) {
        stream_render_pending_ = true;
        const size_t pending_bytes = index < streaming_markdown_.size()
                                         ? streaming_markdown_[index].TailSource().size()
                                         : items_[index].text.size();
        SetTimer(window_, kStreamRenderTimer, StreamRenderDelay(pending_bytes), nullptr);
    }
}

void ChatView::FinishAssistant(size_t index, uint64_t timestamp) {
    if (index >= items_.size() || items_[index].kind != ChatItemKind::Assistant) return;
    items_[index].finished = true;
    if (timestamp != 0) items_[index].timestamp = timestamp;
    if (items_[index].timestamp == 0) items_[index].timestamp = UnixTimeSeconds();
    if (loading_) {
        markdown_ready_[index] = false;
    } else if (markdown_ready_[index]) {
        streaming_markdown_[index].Finish();
    } else {
        PrepareAssistantMarkdown(index);
    }
    InvalidateItemHeight(index);
    if (window_ != nullptr && stream_render_pending_) {
        KillTimer(window_, kStreamRenderTimer);
        stream_render_pending_ = false;
    }
    rail_dirty_ = true;
    ScrollToBottom();
}

void ChatView::FinishTurnStatus(size_t index, uint64_t elapsed_ms, uint64_t timestamp) {
    if (index >= items_.size() || items_[index].kind != ChatItemKind::TurnStatus) return;
    items_[index].finished = true;
    items_[index].paused = false;
    items_[index].expanded = false;
    items_[index].elapsed_ms = elapsed_ms;
    items_[index].timestamp = timestamp == 0 ? UnixTimeSeconds() : timestamp;
    size_t end = items_.size();
    for (size_t next = index + 1; next < items_.size(); ++next) {
        if (items_[next].kind == ChatItemKind::TurnStatus || items_[next].kind == ChatItemKind::User) {
            end = next;
            break;
        }
    }
    size_t last_assistant = static_cast<size_t>(-1);
    for (size_t item = index + 1; item < end; ++item) {
        if (items_[item].kind == ChatItemKind::Assistant) last_assistant = item;
    }
    for (size_t item = index + 1; item < end; ++item) {
        if (items_[item].kind == ChatItemKind::Reasoning || items_[item].kind == ChatItemKind::Tool) {
            items_[item].visible = false;
        } else if (items_[item].kind == ChatItemKind::Assistant) {
            items_[item].visible = item == last_assistant;
            items_[item].final_assistant = item == last_assistant;
        }
        InvalidateItemHeight(item);
    }
    InvalidateItemHeight(index);
    rail_dirty_ = true;
    Rebuild();
}

void ChatView::MarkTurnPaused(size_t index, uint64_t elapsed_ms, uint64_t timestamp) {
    if (index >= items_.size() || items_[index].kind != ChatItemKind::TurnStatus) return;
    items_[index].finished = true;
    items_[index].paused = true;
    items_[index].expanded = true;
    items_[index].elapsed_ms = elapsed_ms;
    items_[index].timestamp = timestamp == 0 ? UnixTimeSeconds() : timestamp;
    InvalidateItemHeight(index);
    rail_dirty_ = true;
    Rebuild();
}

void ChatView::AppendReasoning(size_t index, const std::string& text) {
    if (index >= items_.size() || items_[index].kind != ChatItemKind::Reasoning) return;
    items_[index].text = NormalizeReasoningBuffer(items_[index].text + text);
    InvalidateItemHeight(index);
    if (loading_) return;
    if (window_ != nullptr && !stream_render_pending_) {
        stream_render_pending_ = true;
        SetTimer(window_, kStreamRenderTimer, StreamRenderDelay(items_[index].text.size()), nullptr);
    }
}

void ChatView::FinishReasoning(size_t index, uint64_t timestamp) {
    if (index >= items_.size() || items_[index].kind != ChatItemKind::Reasoning) return;
    items_[index].finished = true;
    items_[index].timestamp = timestamp == 0 ? UnixTimeSeconds() : timestamp;
    InvalidateItemHeight(index);
    if (window_ != nullptr && stream_render_pending_) {
        KillTimer(window_, kStreamRenderTimer);
        stream_render_pending_ = false;
    }
    rail_dirty_ = true;
    ScrollToBottom();
}

void ChatView::HideReasoning(size_t index) {
    if (index >= items_.size() || items_[index].kind != ChatItemKind::Reasoning || !items_[index].visible) return;
    items_[index].visible = false;
    InvalidateItemHeight(index);
    rail_dirty_ = true;
    Rebuild();
}

void ChatView::FinishTool(size_t index, bool success, const std::string& result, uint64_t timestamp,
                          uint64_t elapsed_ms) {
    if (index >= items_.size() || items_[index].kind != ChatItemKind::Tool) return;
    items_[index].success = success;
    items_[index].result = SanitizeDisplayText(result);
    items_[index].finished = true;
    items_[index].elapsed_ms =
        elapsed_ms > 0 ? elapsed_ms
                       : (items_[index].start_tick_ms == 0 ? 0 : MonotonicMillis() - items_[index].start_tick_ms);
    items_[index].timestamp = timestamp == 0 ? UnixTimeSeconds() : timestamp;
    InvalidateItemHeight(index);
    rail_dirty_ = true;
    ScrollToBottom();
}

void ChatView::ToggleTool(size_t index) {
    if (index >= items_.size() || items_[index].kind != ChatItemKind::Tool) return;
    items_[index].expanded = !items_[index].expanded;
    InvalidateItemHeight(index);
    rail_dirty_ = true;
    Rebuild();
}

void ChatView::ToggleToolGroup(size_t index) {
    if (index >= items_.size() || items_[index].kind != ChatItemKind::Tool) return;
    items_[index].group_collapsed = !items_[index].group_collapsed;
    InvalidateItemHeight();
    rail_dirty_ = true;
    Rebuild();
}

void ChatView::ToggleTurnStatus(size_t index) {
    if (index >= items_.size() || items_[index].kind != ChatItemKind::TurnStatus || !items_[index].finished) return;
    const bool expanded = !items_[index].expanded;
    items_[index].expanded = expanded;

    size_t end = items_.size();
    for (size_t next = index + 1; next < items_.size(); ++next) {
        if (items_[next].kind == ChatItemKind::TurnStatus || items_[next].kind == ChatItemKind::User) {
            end = next;
            break;
        }
    }
    size_t last_assistant = static_cast<size_t>(-1);
    for (size_t item = index + 1; item < end; ++item) {
        if (items_[item].kind == ChatItemKind::Assistant) last_assistant = item;
    }
    for (size_t item = index + 1; item < end; ++item) {
        if (items_[item].kind == ChatItemKind::Tool) {
            items_[item].visible = expanded;
        } else if (items_[item].kind == ChatItemKind::Assistant) {
            items_[item].visible = expanded || item == last_assistant;
        }
        InvalidateItemHeight(item);
    }
    InvalidateItemHeight(index);
    rail_dirty_ = true;
    Rebuild();
}

void ChatView::EraseFrom(size_t index) {
    if (index >= items_.size()) return;
    if (window_ != nullptr && stream_render_pending_) KillTimer(window_, kStreamRenderTimer);
    stream_render_pending_ = false;
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(index), items_.end());
    item_heights_.resize(items_.size());
    item_text_heights_.resize(items_.size());
    streaming_markdown_.resize(items_.size());
    markdown_ready_.resize(items_.size());
    if (layout_cache_ != nullptr) {
        layout_cache_->groups.resize(items_.size());
        layout_cache_->groups_dirty = true;
    }
    last_user_index_ = static_cast<size_t>(-1);
    for (size_t next = items_.size(); next > 0; --next) {
        if (items_[next - 1].kind == ChatItemKind::User) {
            last_user_index_ = next - 1;
            break;
        }
    }
    InvalidateItemHeight();
    hover_item_ = static_cast<size_t>(-1);
    rail_dirty_ = true;
    Rebuild();
}

const ChatItem* ChatView::Item(size_t index) const {
    return index < items_.size() ? &items_[index] : nullptr;
}

size_t ChatView::LastUserIndex() const {
    return last_user_index_;
}

void ChatView::ScrollToBottom() {
    if (window_ == nullptr || loading_) return;
    HDC device = GetDC(window_);
    if (!auto_scroll_enabled_) {
        // Streaming while the user is reading an older turn must not force a full height
        // recomputation. Only refresh the jump rail when its geometry changed.
        if (rail_dirty_) {
            UpdateRail(device);
            rail_dirty_ = false;
        }
        ReleaseDC(window_, device);
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    const int content_height = CalculateContentHeight(device);
    RECT client{};
    GetClientRect(window_, &client);
    scroll_y_ = std::max(0, content_height - static_cast<int>(client.bottom - client.top));
    if (rail_dirty_) {
        UpdateRail(device);
        rail_dirty_ = false;
    }
    ReleaseDC(window_, device);
    InvalidateRect(window_, nullptr, FALSE);
}

void ChatView::Rebuild() {
    if (window_ == nullptr || loading_) return;
    HDC device = GetDC(window_);
    const int content_height = CalculateContentHeight(device);
    RECT client{};
    GetClientRect(window_, &client);
    ClampScroll(content_height, client.bottom - client.top);
    if (rail_dirty_) {
        UpdateRail(device);
        rail_dirty_ = false;
    }
    ReleaseDC(window_, device);
    InvalidateRect(window_, nullptr, FALSE);
}

LRESULT CALLBACK ChatView::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    ChatView* self = reinterpret_cast<ChatView*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<ChatView*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) self->window_ = window;
    }
    return self != nullptr ? self->HandleMessage(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

int ChatView::Scale(int pixels) const {
    return MulDiv(pixels, dpi_, 96);
}

HDC ChatView::EnsureBackBuffer(HDC target, int width, int height) {
    if (target == nullptr || width <= 0 || height <= 0) return nullptr;
    if (back_dc_ != nullptr && back_width_ == width && back_height_ == height) return back_dc_;
    ReleaseBackBuffer();
    HDC dc = CreateCompatibleDC(target);
    if (dc == nullptr) return nullptr;
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    if (bitmap == nullptr) {
        DeleteDC(dc);
        return nullptr;
    }
    back_bitmap_old_ = SelectObject(dc, bitmap);
    back_dc_ = dc;
    back_bitmap_ = bitmap;
    back_width_ = width;
    back_height_ = height;
    return back_dc_;
}

void ChatView::ReleaseBackBuffer() {
    if (back_dc_ != nullptr) {
        if (back_bitmap_old_ != nullptr) SelectObject(back_dc_, back_bitmap_old_);
        DeleteDC(back_dc_);
        back_dc_ = nullptr;
        back_bitmap_old_ = nullptr;
    }
    if (back_bitmap_ != nullptr) {
        DeleteObject(back_bitmap_);
        back_bitmap_ = nullptr;
    }
    back_width_ = 0;
    back_height_ = 0;
}

void ChatView::InvalidateItemHeight(size_t index) {
    heights_dirty_ = true;
    if (layout_cache_ != nullptr) {
        if (index == static_cast<size_t>(-1) ||
            (index < items_.size() && items_[index].kind == ChatItemKind::Tool)) {
            layout_cache_->groups_dirty = true;
        }
        if (loading_ && !restoring_) layout_cache_->restart_heights = true;
    }
    if (item_heights_.size() != items_.size() || item_text_heights_.size() != items_.size()) {
        item_heights_.resize(items_.size(), -1);
        item_text_heights_.resize(items_.size(), -1);
    }
    if (index == static_cast<size_t>(-1)) {
        if (loading_) {
            layout_cache_->remeasure_all = true;
            return;
        }
        std::fill(item_heights_.begin(), item_heights_.end(), -1);
        std::fill(item_text_heights_.begin(), item_text_heights_.end(), -1);
        return;
    }
    if (index < item_heights_.size()) {
        item_heights_[index] = -1;
        item_text_heights_[index] = -1;
    }
}

ChatView::LayoutCache& ChatView::EnsureGroups() {
    if (layout_cache_ == nullptr) layout_cache_ = std::make_unique<LayoutCache>();
    if (layout_cache_->groups_dirty || layout_cache_->groups.size() != items_.size()) {
        layout_cache_->groups = ComputeGroups(items_);
        layout_cache_->groups_dirty = false;
    }
    return *layout_cache_;
}

void ChatView::PrepareAssistantMarkdown(size_t index) {
    if (restoring_ || index >= items_.size() || items_[index].kind != ChatItemKind::Assistant ||
        !items_[index].visible || markdown_ready_[index]) return;
    streaming_markdown_[index].Reset();
    streaming_markdown_[index].Append(items_[index].text);
    if (items_[index].finished) streaming_markdown_[index].Finish();
    markdown_ready_[index] = true;
}

void ChatView::PrepareItemHeight(HDC device, size_t index) {
    if (item_heights_[index] >= 0) return;
    PrepareAssistantMarkdown(index);
    const int known_text_height =
        items_[index].kind == ChatItemKind::Assistant ? item_text_heights_[index] : -1;
    const ChatFonts fonts{text_font_, bold_font_, italic_font_, bold_italic_font_, mono_font_, heading_font_};
    const ItemLayout layout = LayoutItem(device, items_, layout_cache_->groups, streaming_markdown_, index,
                                         LastUserIndex(), 0, layout_client_width_, fonts, time_font_,
                                         known_text_height);
    item_heights_[index] = std::max(0, layout.bottom - layout.top);
    if (items_[index].kind == ChatItemKind::Assistant && items_[index].visible) {
        item_text_heights_[index] = std::max(0, static_cast<int>(layout.text.bottom - layout.text.top));
    }
}

void ChatView::PrepareRestoreRows() {
    if (!loading_ || restoring_ || window_ == nullptr || layout_cache_ == nullptr) return;
    const uint64_t started = MonotonicMillis();
    LayoutCache& cache = *layout_cache_;
    RECT client{};
    GetClientRect(window_, &client);
    const int width = std::max(1, static_cast<int>(client.right - client.left));
    const int viewport = static_cast<int>(client.bottom - client.top);
    const auto begin_heights = [&] {
        cache.stage = LayoutCache::Stage::Heights;
        cache.cursor = 0;
        cache.content_y = 18;
        cache.turns.clear();
        cache.restart_heights = false;
    };
    const auto begin_rails = [&] {
        cache.stage = LayoutCache::Stage::Rails;
        cache.cursor = 0;
        cache.viewport = viewport;
        cache.rail_previous = std::numeric_limits<int>::min();
        rail_dashes_.clear();
        const int margin = Scale(10);
        const int available_span = std::max(1, viewport - margin * 2);
        const int compact_span = cache.turns.empty() ? 0 : static_cast<int>((cache.turns.size() - 1) * Scale(14));
        cache.rail_top = compact_span <= available_span ? std::max(margin, viewport / 2 - compact_span / 2) : margin;
        cache.rail_bottom = compact_span <= available_span
                                ? std::min(viewport - margin, cache.rail_top + compact_span) : viewport - margin;
        cache.rail_gap = std::max(Scale(2), cache.turns.size() > 1
                                              ? (cache.rail_bottom - cache.rail_top) /
                                                    static_cast<int>(cache.turns.size() - 1) : 0);
    };
    if (cache.groups_dirty || cache.groups.size() != items_.size()) {
        cache.groups.resize(items_.size());
        cache.groups_dirty = false;
        cache.stage = LayoutCache::Stage::Groups;
        cache.cursor = 0;
        cache.group_first = static_cast<size_t>(-1);
        cache.group_previous = static_cast<size_t>(-1);
    }
    if (layout_client_width_ != width) {
        layout_client_width_ = width;
        cache.remeasure_all = true;
        cache.restart_heights = true;
    }
    if (cache.restart_heights && cache.stage != LayoutCache::Stage::Groups) begin_heights();
    if (cache.stage == LayoutCache::Stage::Rails && cache.viewport != viewport) begin_rails();
    HDC device = GetDC(window_);
    if (device == nullptr) return;
    size_t prepared = 0;
    while (prepared < kRestoreRowsPerTick && MonotonicMillis() - started < kRestoreBudgetMs) {
        if (cache.stage == LayoutCache::Stage::Groups) {
            if (cache.cursor == items_.size()) {
                if (cache.group_first != static_cast<size_t>(-1) && !cache.groups[cache.group_first].leader) {
                    cache.groups[cache.group_first] = {};
                }
                begin_heights();
                continue;
            }
            const size_t index = cache.cursor++;
            const ChatItem& item = items_[index];
            cache.groups[index] = {};
            if (item.kind == ChatItemKind::Tool) {
                if (cache.group_first == static_cast<size_t>(-1) ||
                    !CanJoinActivityGroup(items_[cache.group_previous], item)) {
                    if (cache.group_first != static_cast<size_t>(-1) && !cache.groups[cache.group_first].leader) {
                        cache.groups[cache.group_first] = {};
                    }
                    cache.group_first = index;
                } else {
                    cache.groups[index].hidden = items_[cache.group_first].group_collapsed;
                    cache.groups[index].indented = true;
                }
                GroupInfo& group = cache.groups[cache.group_first];
                ++group.size;
                group.leader = group.size > 1;
                group.last_member = index;
                group.finished += item.finished ? 1 : 0;
                group.active = group.active || !item.finished;
                group.failed = group.failed || (item.finished && !item.success);
                group.elapsed_ms += ToolElapsed(item);
                switch (CategoryOf(item.tool_name)) {
                    case ToolCategory::Read: ++group.read_count; break;
                    case ToolCategory::Search: ++group.search_count; break;
                    case ToolCategory::Command: ++group.command_count; break;
                    case ToolCategory::Image:
                    case ToolCategory::Write:
                    case ToolCategory::Other: break;
                }
                cache.group_previous = index;
                item_heights_[index] = -1;
            } else if (item.kind != ChatItemKind::Reasoning) {
                if (cache.group_first != static_cast<size_t>(-1) && !cache.groups[cache.group_first].leader) {
                    cache.groups[cache.group_first] = {};
                }
                cache.group_first = static_cast<size_t>(-1);
                cache.group_previous = static_cast<size_t>(-1);
            }
        } else if (cache.stage == LayoutCache::Stage::Heights) {
            if (cache.cursor == items_.size()) {
                content_height_ = cache.content_y;
                cache.remeasure_all = false;
                begin_rails();
                continue;
            }
            const size_t index = cache.cursor++;
            if (cache.remeasure_all) {
                item_heights_[index] = -1;
                item_text_heights_[index] = -1;
            }
            PrepareItemHeight(device, index);
            if (items_[index].kind == ChatItemKind::User) cache.turns.emplace_back(index, cache.content_y);
            cache.content_y += item_heights_[index];
        } else if (cache.stage == LayoutCache::Stage::Rails) {
            if (cache.cursor == cache.turns.size() || viewport <= 0 || client.right <= client.left ||
                cache.content_y + 4 <= viewport) {
                cache.stage = LayoutCache::Stage::Complete;
                heights_dirty_ = false;
                rail_dirty_ = false;
                const int content_height = content_height_ + Scale(kContentBottomPadding) +
                                           (activity_visible_ ? Scale(30) : 0);
                if (auto_scroll_enabled_) scroll_y_ = std::max(0, content_height - viewport);
                else ClampScroll(content_height, viewport);
                CancelRestore();
                InvalidateRect(window_, nullptr, FALSE);
                break;
            }
            const auto& turn = cache.turns[cache.cursor++];
            int position = cache.rail_top + MulDiv(turn.second, cache.rail_bottom - cache.rail_top,
                                                   std::max(1, cache.content_y + 4));
            if (cache.rail_previous != std::numeric_limits<int>::min()) {
                position = std::max(position, cache.rail_previous + cache.rail_gap);
            }
            position = std::min(position, cache.rail_bottom);
            cache.rail_previous = position;
            rail_dashes_.push_back(RailDash{turn.first, position, turn.second});
        } else {
            break;
        }
        ++prepared;
    }
    ReleaseDC(window_, device);
}

void ChatView::EnsureItemHeights(HDC device) {
    if (window_ == nullptr || device == nullptr || loading_) return;
    RECT client{};
    GetClientRect(window_, &client);
    const int client_width = std::max(1, static_cast<int>(client.right - client.left));
    if (item_heights_.size() != items_.size() || item_text_heights_.size() != items_.size() ||
        streaming_markdown_.size() != items_.size() || layout_client_width_ != client_width) {
        item_heights_.assign(items_.size(), -1);
        item_text_heights_.assign(items_.size(), -1);
        layout_client_width_ = client_width;
        heights_dirty_ = true;
    }
    if (!heights_dirty_) return;
    EnsureGroups();
    content_height_ = 18;
    for (size_t index = 0; index < items_.size(); ++index) {
        PrepareItemHeight(device, index);
        content_height_ += item_heights_[index];
    }
    heights_dirty_ = false;
}

int ChatView::ContentWidth() const {
    RECT client{};
    GetClientRect(window_, &client);
    return CalculateContentBounds(static_cast<int>(client.right - client.left)).width;
}

int ChatView::CalculateContentHeight(HDC device) {
    EnsureItemHeights(device);
    int content_height = content_height_;
    if (activity_visible_) content_height += Scale(30);
    return std::max(0, content_height + Scale(kContentBottomPadding));
}

std::vector<ChatView::HitArea> ChatView::CalculateHitAreas(HDC device) {
    std::vector<HitArea> areas;
    if (loading_) return areas;
    RECT client{};
    GetClientRect(window_, &client);
    const size_t last_user = LastUserIndex();
    const ChatFonts fonts{text_font_, bold_font_, italic_font_, bold_italic_font_, mono_font_, heading_font_};
    EnsureItemHeights(device);
    const std::vector<GroupInfo>& groups = EnsureGroups().groups;
    int y = 18 - scroll_y_;
    for (size_t index = 0; index < items_.size(); ++index) {
        const int item_height = item_heights_[index];
        if (!items_[index].visible || y + item_height < 0 || y > client.bottom ||
            (groups[index].hidden && !groups[index].leader)) {
            y += item_height;
            continue;
        }
        const int known_text_height =
            items_[index].kind == ChatItemKind::Assistant ? item_text_heights_[index] : -1;
        const ItemLayout layout = LayoutItem(device, items_, groups, streaming_markdown_, index, last_user, y,
                                             client.right - client.left, fonts, time_font_, known_text_height);
        if (layout.bottom >= 0 && layout.top <= client.bottom) {
            areas.push_back(HitArea{HitArea::Type::Message, index, layout.message});
            if (items_[index].kind == ChatItemKind::TurnStatus) {
                areas.push_back(HitArea{HitArea::Type::TurnStatus, index, layout.message});
            } else if (items_[index].kind == ChatItemKind::Tool) {
                if (groups[index].leader) {
                    areas.push_back(HitArea{HitArea::Type::ToolGroup, index, layout.group_header});
                    if (items_[index].group_collapsed) {
                        y = layout.bottom;
                        continue;
                    }
                }
                areas.push_back(HitArea{HitArea::Type::ToolHeader, index, layout.tool_header});
            } else if (items_[index].kind == ChatItemKind::User) {
                areas.push_back(HitArea{HitArea::Type::Copy, index, layout.copy});
                if (index == last_user) areas.push_back(HitArea{HitArea::Type::Edit, index, layout.edit});
            } else if (items_[index].kind == ChatItemKind::Assistant && IsCopyableAssistant(items_, index)) {
                areas.push_back(HitArea{HitArea::Type::Copy, index, layout.copy});
            }
        }
        y = layout.bottom;
    }
    return areas;
}

ChatView::HitArea ChatView::HitTest(POINT point) {
    if (loading_) return {};
    RECT client{};
    GetClientRect(window_, &client);
    const int gutter = Scale(22);
    if (!rail_dashes_.empty() && point.x <= gutter) {
        const int reach = Scale(8);
        for (const RailDash& dash : rail_dashes_) {
            if (point.y >= dash.rail_y - reach && point.y <= dash.rail_y + reach) {
                HitArea area;
                area.type = HitArea::Type::Rail;
                area.item = dash.item;
                area.rect = RECT{0, dash.rail_y - reach, gutter, dash.rail_y + reach};
                return area;
            }
        }
        return {};
    }
    HDC device = GetDC(window_);
    std::vector<HitArea> areas = CalculateHitAreas(device);
    ReleaseDC(window_, device);
    for (auto iterator = areas.rbegin(); iterator != areas.rend(); ++iterator) {
        if (iterator->Contains(point)) return *iterator;
    }
    return {};
}

// The rail replaces the native scrollbar: one dash per user turn, positioned by that turn's
// place in the conversation, so clicking a dash jumps straight to the turn.
void ChatView::UpdateRail(HDC device) {
    if (loading_) return;
    rail_dashes_.clear();
    if (window_ == nullptr || items_.empty()) return;
    RECT client{};
    GetClientRect(window_, &client);
    const int width = static_cast<int>(client.right - client.left);
    const int viewport = static_cast<int>(client.bottom - client.top);
    if (width <= 0 || viewport <= 0) return;

    std::vector<std::pair<size_t, int>> turns;
    EnsureItemHeights(device);
    int y = 18;
    for (size_t index = 0; index < items_.size(); ++index) {
        if (items_[index].kind == ChatItemKind::User) turns.emplace_back(index, y);
        y += item_heights_[index];
    }
    const int content_height = std::max(1, y + 4);
    if (content_height <= viewport) return;

    const int margin = Scale(10);
    const int available_span = std::max(1, viewport - margin * 2);
    const int compact_gap = Scale(14);
    const int compact_span = turns.empty() ? 0 : static_cast<int>((turns.size() - 1) * compact_gap);
    const int rail_top =
        compact_span <= available_span ? std::max(margin, viewport / 2 - compact_span / 2) : margin;
    const int rail_bottom =
        compact_span <= available_span ? std::min(viewport - margin, rail_top + compact_span) : viewport - margin;
    const int span = rail_bottom - rail_top;
    const int gap = std::max(Scale(2), turns.size() > 1 ? span / static_cast<int>(turns.size() - 1) : 0);
    int previous = std::numeric_limits<int>::min();
    for (const auto& turn : turns) {
        int position = rail_top + MulDiv(turn.second, span, content_height);
        if (previous != std::numeric_limits<int>::min()) position = std::max(position, previous + gap);
        position = std::min(position, rail_bottom);
        previous = position;
        rail_dashes_.push_back(RailDash{turn.first, position, turn.second});
    }
}

void ChatView::DrawScrollRail(HDC device, const RECT& client) {
    if (rail_dashes_.empty()) return;
    const int left = Scale(6);
    const int thickness = std::max(1, Scale(2));
    const int half = thickness / 2;
    const int idle_length = Scale(8);
    const int active_length = Scale(13);
    const int hover_length = Scale(18);

    size_t current = rail_dashes_.front().item;
    for (const RailDash& dash : rail_dashes_) {
        if (dash.content_top <= scroll_y_ + Scale(24)) current = dash.item;
    }

    int hovered_y = -1;
    for (const RailDash& dash : rail_dashes_) {
        const bool hovered = hover_type_ == HitArea::Type::Rail && hover_item_ == dash.item;
        const bool active = !hovered && dash.item == current;
        const int length = hovered ? hover_length : (active ? active_length : idle_length);
        const COLORREF color = hovered ? kRailHover : (active ? kRailActive : kRailIdle);
        HBRUSH brush = CreateSolidBrush(color);
        RECT bar{left, dash.rail_y - half, left + length, dash.rail_y - half + thickness};
        FillRect(device, &bar, brush);
        DeleteObject(brush);
        if (hovered) hovered_y = dash.rail_y;
    }

    if (hovered_y < 0 || hover_item_ >= items_.size()) return;
    const std::wstring preview = RailPreview(items_[hover_item_].text);
    HFONT previous_font = static_cast<HFONT>(SelectObject(device, text_font_));
    const int max_text_width = Scale(320);
    RECT measured{0, 0, max_text_width, 0};
    DrawTextW(device, preview.c_str(), -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    const int padding_x = Scale(12);
    const int padding_y = Scale(8);
    const int box_width = std::min(max_text_width, static_cast<int>(measured.right - measured.left)) + padding_x * 2;
    const int box_height =
        std::max(Scale(30), static_cast<int>(measured.bottom - measured.top) + padding_y * 2);
    int box_left = left + hover_length + Scale(12);
    box_left = std::min(box_left, static_cast<int>(client.right) - box_width - Scale(12));
    box_left = std::max(Scale(12), box_left);
    int top = hovered_y - box_height / 2;
    top = std::max(Scale(8), std::min(top, static_cast<int>(client.bottom) - box_height - Scale(8)));
    const RECT box{box_left, top, box_left + box_width, top + box_height};
    DrawRoundPanel(device, box, kRailTip, kRailTipBorder, 10);
    SetBkMode(device, TRANSPARENT);
    SetTextColor(device, kText);
    RECT text_rect{box.left + padding_x, box.top + padding_y, box.right - padding_x, box.bottom - padding_y};
    DrawTextW(device, preview.c_str(), -1, &text_rect, DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(device, previous_font);
}

void ChatView::ClampScroll(int content_height, int viewport_height) {
    const int max_scroll = std::max(0, content_height - viewport_height);
    scroll_y_ = std::max(0, std::min(scroll_y_, max_scroll));
}

void ChatView::SetHover(const HitArea& area) {
    const bool changed = hover_item_ != area.item || hover_type_ != area.type;
    hover_item_ = area.item;
    hover_type_ = area.type;
    if (changed) InvalidateRect(window_, nullptr, FALSE);
}

LRESULT ChatView::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            rail_dirty_ = true;
            if (loading_) InvalidateRect(window_, nullptr, FALSE);
            Rebuild();
            return 0;
        case WM_TIMER:
            if (restore_spinner_timer_ != 0 && wparam == restore_spinner_timer_) {
                if (loading_ && MonotonicMillis() - restore_started_tick_ >= kRestoreSpinnerRevealDelayMs) {
                    restore_frame_ = (restore_frame_ + 1) % 24;
                    RECT client{};
                    GetClientRect(window_, &client);
                    const int extent = Scale(22);
                    const int center_x = (client.left + client.right) / 2;
                    const int center_y = (client.top + client.bottom) / 2;
                    const RECT spinner{center_x - extent, center_y - extent, center_x + extent, center_y + extent};
                    InvalidateRect(window_, &spinner, FALSE);
                }
                return 0;
            }
            if (restore_prepare_timer_ != 0 && wparam == restore_prepare_timer_) {
                PrepareRestoreRows();
                return 0;
            }
            if (wparam == kStreamRenderTimer) {
                KillTimer(window_, kStreamRenderTimer);
                if (loading_ || !stream_render_pending_) return 0;
                stream_render_pending_ = false;
                ScrollToBottom();
                return 0;
            }
            break;
        case WM_PRINTCLIENT:
        case WM_PAINT: {
            const bool printing = message == WM_PRINTCLIENT;
            PAINTSTRUCT paint{};
            HDC target = printing ? reinterpret_cast<HDC>(wparam) : BeginPaint(window_, &paint);
            RECT client{};
            GetClientRect(window_, &client);
            HDC buffer = EnsureBackBuffer(target, std::max(1, static_cast<int>(client.right)),
                                          std::max(1, static_cast<int>(client.bottom)));
            if (buffer == nullptr) {
                if (!printing) EndPaint(window_, &paint);
                return 0;
            }
            HBRUSH background = CreateSolidBrush(kBackground);
            const RECT redraw = loading_ && !printing ? paint.rcPaint : client;
            FillRect(buffer, &redraw, background);
            DeleteObject(background);
            SetBkMode(buffer, TRANSPARENT);

            if (loading_) {
                if (MonotonicMillis() - restore_started_tick_ >= kRestoreSpinnerRevealDelayMs) {
                    DrawRestoreLoader(buffer, client, restore_frame_);
                }
                BitBlt(target, redraw.left, redraw.top, redraw.right - redraw.left, redraw.bottom - redraw.top,
                       buffer, redraw.left, redraw.top, SRCCOPY);
                if (!printing) EndPaint(window_, &paint);
                return 0;
            }
            const size_t last_user = LastUserIndex();
            const ChatFonts fonts{text_font_, bold_font_, italic_font_, bold_italic_font_, mono_font_, heading_font_};
            EnsureItemHeights(buffer);
            const std::vector<GroupInfo>& groups = EnsureGroups().groups;
            const wchar_t* toolbar_tooltip = nullptr;
            RECT toolbar_anchor{};
            int y = 18 - scroll_y_;
            for (size_t index = 0; index < items_.size(); ++index) {
                const ChatItem& item = items_[index];
                const int item_height = item_heights_[index];
                const bool hovered = hover_item_ == index;
                if (!item.visible || y + item_height < -40 || y > client.bottom + 40 ||
                    (groups[index].hidden && !groups[index].leader)) {
                    y += item_height;
                    continue;
                }
                const int known_text_height =
                    item.kind == ChatItemKind::Assistant ? item_text_heights_[index] : -1;
                const ItemLayout layout = LayoutItem(buffer, items_, groups, streaming_markdown_, index, last_user, y,
                                                     client.right - client.left, fonts, time_font_, known_text_height);

                if (item.kind == ChatItemKind::User) {
                    DrawRoundPanel(buffer, layout.bubble, kUserBubble, kUserBubble, 12);
                    DrawWrappedText(buffer, text_font_, Utf8ToWide(item.text), layout.text, kText);
                } else if (item.kind == ChatItemKind::UsageStats) {
                    DrawRoundPanel(buffer, layout.bubble, kBackground, kSuccess, 8);
                    DrawWrappedText(buffer, text_font_, FormatUsageStats(item), layout.text, kMuted);
                } else if (item.kind == ChatItemKind::TurnStatus) {
                    const COLORREF status_color = hovered ? kText : kMuted;
                    HFONT status_font = static_cast<HFONT>(SelectObject(buffer, text_font_));
                    SetTextColor(buffer, status_color);
                    const uint64_t elapsed = item.finished
                                                 ? item.elapsed_ms
                                                 : (item.start_tick_ms == 0 ? 0 : MonotonicMillis() - item.start_tick_ms);
                    const wchar_t* msgid = item.paused ? L"Interrupted" : (item.finished ? L"Processed" : L"Processing");
                    std::wstring label = i18n::Text(msgid);
                    if (elapsed > 0) {
                        msgid = item.paused ? L"Interrupted {elapsed}"
                                           : (item.finished ? L"Processed {elapsed}" : L"Processing {elapsed}");
                        label = i18n::Format(msgid, {{L"elapsed", FormatTurnDuration(elapsed)}});
                    }
                    RECT status_rect = layout.text;
                    DrawTextW(buffer, label.c_str(), -1, &status_rect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                    SelectObject(buffer, status_font);
                    if (item.finished) {
                        RECT chevron{layout.message.right - Scale(24), layout.message.top,
                                     layout.message.right - Scale(4), layout.message.bottom};
                        DrawChevron(buffer, chevron, item.expanded, hovered ? kText : kMuted);
                    }
                    HPEN divider = CreatePen(PS_SOLID, 1, kToolBorder);
                    HGDIOBJ old_pen = SelectObject(buffer, divider);
                    MoveToEx(buffer, layout.message.left, layout.message.bottom - 1, nullptr);
                    LineTo(buffer, layout.message.right, layout.message.bottom - 1);
                    SelectObject(buffer, old_pen);
                    DeleteObject(divider);
                } else if (item.kind == ChatItemKind::Tool) {
                    const bool group_leader = groups[index].leader;
                    const bool group_open = group_leader && !item.group_collapsed;
                    if (group_leader) {
                        const RECT header = layout.group_header;
                        const int glyph_x = header.left + Scale(18);
                        const int glyph_y = (header.top + header.bottom) / 2;
                        const bool command_group = GroupKey(item.tool_name) == "command";
                        const COLORREF group_color =
                            hovered ? kText : (groups[index].failed ? kError : kMuted);
                        if (command_group) {
                            const RECT icon_rect{header.left + Scale(9), header.top, header.left + Scale(30), header.bottom};
                            DrawTerminalIcon(buffer, text_font_, icon_rect, group_color);
                        } else if (groups[index].active) {
                            DrawActivityGlyph(buffer, glyph_x, glyph_y, Scale(5), activity_frame_, kText);
                        } else if (groups[index].failed) {
                            DrawFailureGlyph(buffer, glyph_x, glyph_y, kError);
                        } else {
                            DrawSuccessGlyph(buffer, glyph_x, glyph_y, kSuccess);
                        }
                        HFONT group_font = static_cast<HFONT>(SelectObject(buffer, text_font_));
                        SetTextColor(buffer, group_color);
                        RECT group_title{header.left + Scale(34), header.top, header.right - Scale(94), header.bottom};
                        std::wstring group_text = GroupLabel(groups[index], GroupKey(item.tool_name));
                        if (groups[index].active) {
                            const std::wstring active_summary =
                                ActiveToolSummary(items_, groups[index], static_cast<size_t>(index));
                            if (!active_summary.empty()) group_text += L"  ·  " + active_summary;
                        }
                        if (groups[index].elapsed_ms > 0) group_text += L"  ·  " + FormatDuration(groups[index].elapsed_ms);
                        if (groups[index].active) {
                            // The collapsed header of a still-running batch carries the sweep.
                            const std::wstring live = FitText(buffer, group_text, group_title.right - group_title.left, false);
                            DrawShimmerText(buffer, live, group_title, group_color, RGB(255, 255, 255));
                        } else {
                            DrawTextW(buffer, group_text.c_str(), -1, &group_title,
                                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                        }
                        SelectObject(buffer, group_font);
                        RECT group_chevron{header.right - Scale(28), header.top, header.right - Scale(6), header.bottom};
                        DrawChevron(buffer, group_chevron, group_open, hovered ? kText : kMuted);
                    }
                    if (!group_leader || group_open) {
                        const int glyph_x = layout.tool_header.left + Scale(18);
                        const int glyph_y = (layout.tool_header.top + layout.tool_header.bottom) / 2;
                        const COLORREF tool_color = hovered ? kText : (item.finished && !item.success ? kError : kMuted);
                        if (item.tool_name == "run_command") {
                            const RECT icon_rect{layout.tool_header.left + Scale(9), layout.tool_header.top,
                                                 layout.tool_header.left + Scale(30), layout.tool_header.bottom};
                            DrawTerminalIcon(buffer, text_font_, icon_rect, tool_color);
                        } else if (!item.finished) {
                            DrawActivityGlyph(buffer, glyph_x, glyph_y, Scale(5), activity_frame_, kText);
                        } else if (item.success) {
                            DrawSuccessGlyph(buffer, glyph_x, glyph_y, hovered ? kText : kSuccess);
                        } else {
                            DrawFailureGlyph(buffer, glyph_x, glyph_y, kError);
                        }
                        HFONT previous = static_cast<HFONT>(SelectObject(buffer, text_font_));
                        SetTextColor(buffer, tool_color);
                        RECT title_rect{layout.tool_header.left + Scale(34), layout.tool_header.top,
                                        layout.tool_header.right - Scale(92),
                                        layout.tool_header.bottom};
                        std::wstring title =
                            item.finished ? ToolLabel(item.tool_name)
                                          : i18n::Text(item.tool_name == "run_command" ? L"Running command" : L"Running tool");
                        const std::wstring summary = ToolSummary(item);
                        if (!summary.empty()) title += L"  ·  " + summary;
                        if (!item.finished) {
                            // A tool that is still running keeps the left-to-right sweep on
                            // its own row, which is where Codex shows live command output.
                            const std::wstring live = FitText(buffer, title, title_rect.right - title_rect.left, false);
                            DrawShimmerText(buffer, live, title_rect, tool_color, RGB(255, 255, 255));
                        } else {
                            DrawTextW(buffer, title.c_str(), -1, &title_rect,
                                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                        }
                        SelectObject(buffer, previous);
                        const std::wstring elapsed = FormatDuration(ToolElapsed(item));
                        if (!elapsed.empty()) {
                            HFONT old_time = static_cast<HFONT>(SelectObject(buffer, time_font_));
                            SetTextColor(buffer, kSubtle);
                            RECT elapsed_rect{layout.tool_header.right - Scale(88), layout.tool_header.top,
                                              layout.tool_header.right - Scale(30), layout.tool_header.bottom};
                            DrawTextW(buffer, elapsed.c_str(), -1, &elapsed_rect,
                                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                            SelectObject(buffer, old_time);
                        }
                        RECT chevron_rect{layout.tool_header.right - Scale(28), layout.tool_header.top,
                                          layout.tool_header.right - Scale(6),
                                          layout.tool_header.bottom};
                        DrawChevron(buffer, chevron_rect, item.expanded, hovered ? kText : kMuted);
                        if (item.expanded) {
                            DrawWrappedText(buffer, mono_font_, ToolDetails(item), layout.tool_body,
                                            item.success ? kMuted : kError);
                        }
                    }
                } else if (item.kind == ChatItemKind::Reasoning) {
                    HFONT reasoning_font = static_cast<HFONT>(SelectObject(buffer, text_font_));
                    const std::wstring label = FitText(buffer, CurrentReasoningSentence(item.text),
                                                       layout.text.right - layout.text.left, false);
                    if (item.finished) {
                        RECT text_rect = layout.text;
                        DrawTextW(buffer, label.c_str(), -1, &text_rect,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    } else {
                        DrawShimmerText(buffer, label, layout.text, kMuted, RGB(255, 255, 255));
                    }
                    SelectObject(buffer, reasoning_font);
                } else if (item.kind == ChatItemKind::Compaction) {
                    const COLORREF color = hovered ? kText : kMuted;
                    DrawCompactionIcon(buffer, layout.message.left + DpiScale(11),
                                       (layout.message.top + layout.message.bottom) / 2, color);
                    HFONT previous = static_cast<HFONT>(SelectObject(buffer, text_font_));
                    SetTextColor(buffer, color);
                    RECT text_rect = layout.text;
                    DrawTextW(buffer, Utf8ToWide(item.text).c_str(), -1, &text_rect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                    SelectObject(buffer, previous);
                } else {
                    if (item.kind == ChatItemKind::Assistant) {
                        if (index < streaming_markdown_.size()) {
                            DrawStreamingMarkdown(buffer, streaming_markdown_[index], layout.text, fonts);
                        } else {
                            DrawMarkdown(buffer, Utf8ToWide(item.text), layout.text, fonts);
                        }
                    } else if (item.kind == ChatItemKind::Quota) {
                        DrawWrappedText(buffer, text_font_, Utf8ToWide(item.text), layout.text, kWarning);
                    } else {
                        DrawWrappedText(buffer, text_font_, Utf8ToWide(item.text), layout.text, kError);
                    }
                }

                const auto draw_time = [&]() {
                    const std::wstring time = FormatTime(item.timestamp);
                    if (time.empty()) return;
                    HFONT previous = static_cast<HFONT>(SelectObject(buffer, time_font_));
                    SetTextColor(buffer, kSubtle);
                    RECT time_rect = layout.time;
                    DrawTextW(buffer, time.c_str(), -1, &time_rect,
                              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    SelectObject(buffer, previous);
                };
                if (item.kind == ChatItemKind::User) {
                    if (hovered) {
                        draw_time();
                        DrawCopyIcon(buffer, layout.copy, kMuted);
                        if (index == last_user) DrawEditIcon(buffer, layout.edit, kMuted);
                    }
                } else if (item.kind == ChatItemKind::Assistant) {
                    if (hovered && item.finished && IsCopyableAssistant(items_, index)) {
                        DrawCopyIcon(buffer, layout.copy, kMuted);
                        draw_time();
                    }
                }
                if (hovered && (item.kind == ChatItemKind::User ||
                                (item.finished && IsCopyableAssistant(items_, index)))) {
                    if (hover_type_ == HitArea::Type::Copy) {
                        toolbar_tooltip = item.kind == ChatItemKind::Assistant ? i18n::Text(L"Copy reply")
                                                                              : i18n::Text(L"Copy message");
                        toolbar_anchor = layout.copy;
                    } else if (hover_type_ == HitArea::Type::Edit && item.kind == ChatItemKind::User &&
                               index == last_user) {
                        toolbar_tooltip = i18n::Text(L"Edit message");
                        toolbar_anchor = layout.edit;
                    }
                }
                y = layout.bottom;
            }

            if (activity_visible_) {
                // The live status belongs to the transcript, so it is drawn as one dim
                // line that the answer and tool rows grow past instead of a floating panel.
                const int activity_top = y + Scale(6);
                const ContentBounds content = CalculateContentBounds(static_cast<int>(client.right - client.left));
                RECT activity_rect{content.left, activity_top, content.right, activity_top + Scale(24)};
                const int glyph_x = activity_rect.left + Scale(7);
                const int glyph_y = (activity_rect.top + activity_rect.bottom) / 2;
                DrawActivityGlyph(buffer, glyph_x, glyph_y, Scale(5), activity_frame_, kText);

                HFONT old_font = static_cast<HFONT>(SelectObject(buffer, time_font_));
                const std::wstring elapsed = FormatDuration(MonotonicMillis() - activity_start_tick_ms_);
                SIZE elapsed_extent{};
                GetTextExtentPoint32W(buffer, elapsed.c_str(), static_cast<int>(elapsed.size()), &elapsed_extent);
                SelectObject(buffer, old_font);
                int text_right = activity_rect.right;
                if (!elapsed.empty()) {
                    text_right = std::max(activity_rect.left + Scale(24), activity_rect.right - elapsed_extent.cx - Scale(14));
                    old_font = static_cast<HFONT>(SelectObject(buffer, time_font_));
                    SetTextColor(buffer, kSubtle);
                    RECT elapsed_rect{text_right + Scale(8), activity_rect.top, activity_rect.right, activity_rect.bottom};
                    DrawTextW(buffer, elapsed.c_str(), -1, &elapsed_rect,
                              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    SelectObject(buffer, old_font);
                }
                if (!activity_label_.empty()) {
                    old_font = static_cast<HFONT>(SelectObject(buffer, text_font_));
                    RECT activity_text{activity_rect.left + Scale(18), activity_rect.top, text_right, activity_rect.bottom};
                    const std::wstring label = FitText(buffer, activity_label_, activity_text.right - activity_text.left, true);
                    DrawShimmerText(buffer, label, activity_text, kMuted, RGB(255, 255, 255));
                    SelectObject(buffer, old_font);
                }
            }

            DrawScrollRail(buffer, client);
            if (toolbar_tooltip != nullptr) {
                DrawToolbarTooltip(buffer, text_font_, client, toolbar_anchor, toolbar_tooltip);
            }

            BitBlt(target, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
            if (!printing) EndPaint(window_, &paint);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (loading_) return 0;
            if (!tracking_mouse_) {
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window_, 0};
                TrackMouseEvent(&tracking);
                tracking_mouse_ = true;
            }
            SetHover(HitTest(POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}));
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_mouse_ = false;
            SetHover(HitArea{});
            return 0;
        case WM_SETCURSOR: {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(window_, &point);
            const HitArea area = HitTest(point);
            const bool interactive = area.type == HitArea::Type::TurnStatus ||
                                     area.type == HitArea::Type::ToolHeader || area.type == HitArea::Type::ToolGroup ||
                                     area.type == HitArea::Type::Copy || area.type == HitArea::Type::Edit ||
                                     area.type == HitArea::Type::Rail;
            SetCursor(LoadCursorW(nullptr, interactive ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        case WM_LBUTTONDOWN:
            return 0;
        case WM_LBUTTONUP: {
            const HitArea area = HitTest(POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            if (area.type == HitArea::Type::Rail) {
                for (size_t rail_index = 0; rail_index < rail_dashes_.size(); ++rail_index) {
                    const RailDash& dash = rail_dashes_[rail_index];
                    if (dash.item != area.item) continue;
                    auto_scroll_enabled_ = rail_index + 1 == rail_dashes_.size();
                    scroll_y_ = std::max(0, dash.content_top - Scale(12));
                    Rebuild();
                    break;
                }
            } else if (area.type == HitArea::Type::TurnStatus) ToggleTurnStatus(area.item);
            else if (area.type == HitArea::Type::ToolGroup && toggle_group_) toggle_group_(area.item);
            else if (area.type == HitArea::Type::ToolHeader && toggle_tool_) toggle_tool_(area.item);
            else if (area.type == HitArea::Type::Copy && copy_item_) copy_item_(area.item);
            else if (area.type == HitArea::Type::Edit && edit_item_) edit_item_(area.item);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (loading_) return 0;
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            if (delta > 0) auto_scroll_enabled_ = false;
            const int wheel_steps = delta == 0 ? 0 : std::max(1, std::abs(delta) / WHEEL_DELTA);
            scroll_y_ -= (delta > 0 ? wheel_steps : -wheel_steps) * Scale(54);
            Rebuild();
            if (delta < 0) {
                HDC device = GetDC(window_);
                const int content_height = CalculateContentHeight(device);
                RECT client{};
                GetClientRect(window_, &client);
                ReleaseDC(window_, device);
                auto_scroll_enabled_ =
                    scroll_y_ >= std::max(0, content_height - static_cast<int>(client.bottom - client.top)) - Scale(2);
            }
            return 0;
        }
        case WM_KEYDOWN: {
            if (loading_) return 0;
            RECT client{};
            GetClientRect(window_, &client);
            const int page = std::max(Scale(40), static_cast<int>(client.bottom - client.top) - Scale(32));
            if (wparam == VK_UP) {
                auto_scroll_enabled_ = false;
                scroll_y_ -= Scale(24);
            } else if (wparam == VK_DOWN) {
                scroll_y_ += Scale(24);
            } else if (wparam == VK_PRIOR) {
                auto_scroll_enabled_ = false;
                scroll_y_ -= page;
            } else if (wparam == VK_NEXT) {
                scroll_y_ += page;
            } else if (wparam == VK_HOME) {
                auto_scroll_enabled_ = false;
                scroll_y_ = 0;
            } else if (wparam == VK_END) {
                auto_scroll_enabled_ = true;
                scroll_y_ = std::numeric_limits<int>::max();
            }
            else return 0;
            Rebuild();
            if (wparam == VK_DOWN || wparam == VK_NEXT) {
                HDC device = GetDC(window_);
                const int content_height = CalculateContentHeight(device);
                RECT client{};
                GetClientRect(window_, &client);
                ReleaseDC(window_, device);
                auto_scroll_enabled_ =
                    scroll_y_ >= std::max(0, content_height - static_cast<int>(client.bottom - client.top)) - Scale(2);
            }
            return 0;
        }
        case WM_DESTROY:
            CancelRestore();
            KillTimer(window_, kStreamRenderTimer);
            stream_render_pending_ = false;
            ReleaseBackBuffer();
            window_ = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

}  // namespace spark
