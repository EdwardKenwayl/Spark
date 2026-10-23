#include "render/markdown.h"

#include "core/common.h"

#include <md4c.h>

#include <algorithm>
#include <cwctype>
#include <list>
#include <unordered_map>

namespace spark {
namespace markdown {
namespace {

constexpr size_t kMaxRenderedLines = 8192;
constexpr size_t kMaxMarkdownBytes = 4 * 1024 * 1024;

struct StyleSpec {
    bool bold = false;
    bool italic = false;
    bool code = false;
    bool link = false;
    bool muted = false;
    bool heading = false;
};

struct ListState {
    bool ordered = false;
    unsigned next = 1;
    wchar_t delimiter = L'.';
};

struct RenderState {
    std::vector<MarkdownLine> lines;
    MarkdownLine current;
    std::vector<StyleSpec> style_stack;
    std::vector<ListState> lists;
    StyleSpec style;
    std::wstring pending_marker;
    int quote_depth = 0;
    int list_depth = 0;
    int heading_indent = 0;
    bool line_open = false;
    bool in_code = false;
    bool in_table = false;
    bool in_table_cell = false;
    bool first_table_cell = true;
    bool limit_reached = false;
};

MarkdownStyle ToStyle(const StyleSpec& style) {
    if (style.code) return MarkdownStyle::Code;
    if (style.link) return MarkdownStyle::Link;
    if (style.muted) return MarkdownStyle::Muted;
    if (style.bold && style.italic) return MarkdownStyle::BoldItalic;
    if (style.bold) return MarkdownStyle::Bold;
    if (style.italic) return MarkdownStyle::Italic;
    if (style.heading) return MarkdownStyle::Heading;
    return MarkdownStyle::Body;
}

void AppendRun(std::vector<MarkdownRun>& runs, std::wstring text, MarkdownStyle style) {
    if (text.empty()) return;
    if (!runs.empty() && runs.back().style == style) {
        runs.back().text += text;
        return;
    }
    runs.push_back({std::move(text), style});
}

void AppendCodepoint(std::wstring& output, uint32_t codepoint) {
    if (sizeof(wchar_t) > 2 || codepoint <= 0xFFFF) {
        output.push_back(static_cast<wchar_t>(codepoint));
        return;
    }
    codepoint -= 0x10000;
    output.push_back(static_cast<wchar_t>(0xD800 + (codepoint >> 10)));
    output.push_back(static_cast<wchar_t>(0xDC00 + (codepoint & 0x3FF)));
}

std::wstring MdTextToWide(const MD_CHAR* text, MD_SIZE size) {
    if (text == nullptr || size == 0) return {};
#ifdef MD4C_USE_UTF16
    return std::wstring(text, size);
#else
    return Utf8ToWide(std::string_view(text, size));
#endif
}

std::wstring DecodeEntity(const std::wstring& entity) {
    if (entity.size() < 3 || entity.front() != L'&' || entity.back() != L';') return entity;
    const std::wstring body = entity.substr(1, entity.size() - 2);
    if (body.size() > 1 && body.front() == L'#') {
        unsigned base = 10;
        size_t start = 1;
        if (start < body.size() && (body[start] == L'x' || body[start] == L'X')) {
            base = 16;
            ++start;
        }
        if (start == body.size()) return entity;
        uint32_t codepoint = 0;
        for (size_t index = start; index < body.size(); ++index) {
            const wchar_t character = body[index];
            unsigned digit = 0;
            if (character >= L'0' && character <= L'9') digit = static_cast<unsigned>(character - L'0');
            else if (base == 16 && character >= L'a' && character <= L'f') digit = static_cast<unsigned>(character - L'a' + 10);
            else if (base == 16 && character >= L'A' && character <= L'F') digit = static_cast<unsigned>(character - L'A' + 10);
            else return entity;
            if (codepoint > 0x10FFFF / base) return entity;
            codepoint = codepoint * base + digit;
            if (codepoint > 0x10FFFF) return entity;
        }
        if (codepoint == 0) return L"\uFFFD";
        std::wstring decoded;
        AppendCodepoint(decoded, codepoint);
        return decoded;
    }
    if (body == L"amp") return L"&";
    if (body == L"lt") return L"<";
    if (body == L"gt") return L">";
    if (body == L"quot") return L"\"";
    if (body == L"apos") return L"'";
    if (body == L"nbsp") return L"\u00A0";
    return entity;
}

void PushLine(RenderState& state, MarkdownLine line) {
    if (state.lines.size() >= kMaxRenderedLines) {
        state.limit_reached = true;
        return;
    }
    state.lines.push_back(std::move(line));
}

void StartLine(RenderState& state, bool code_line = false) {
    if (state.line_open || state.limit_reached) return;
    MarkdownLine line;
    line.code = code_line || state.in_code;
    line.indent = std::min(3, state.quote_depth + state.list_depth + state.heading_indent);
    if (state.quote_depth > 0) AppendRun(line.runs, L"│ ", MarkdownStyle::ListMarker);
    if (!state.pending_marker.empty()) {
        AppendRun(line.runs, std::move(state.pending_marker), MarkdownStyle::ListMarker);
        state.pending_marker.clear();
    }
    state.current = std::move(line);
    state.line_open = true;
}

void FinishLine(RenderState& state) {
    if (!state.line_open) return;
    if (!state.current.runs.empty() || state.current.rule) PushLine(state, std::move(state.current));
    state.current = {};
    state.line_open = false;
}

void AddSpacer(RenderState& state) {
    FinishLine(state);
    if (state.lines.empty() || !state.lines.back().runs.empty()) PushLine(state, {});
}

void AppendText(RenderState& state, const std::wstring& text, MarkdownStyle style) {
    if (text.empty() || state.limit_reached) return;
    if (state.in_table_cell) {
        if (!state.line_open) StartLine(state);
    } else if (!state.line_open) {
        StartLine(state);
    }
    AppendRun(state.current.runs, text, style);
}

void AppendCodeText(RenderState& state, const std::wstring& text) {
    if (state.limit_reached) return;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t newline = text.find(L'\n', start);
        const size_t end = newline == std::wstring::npos ? text.size() : newline;
        if (end > start) {
            if (!state.line_open) StartLine(state, true);
            AppendRun(state.current.runs, text.substr(start, end - start), MarkdownStyle::Code);
        }
        if (newline == std::wstring::npos) break;
        FinishLine(state);
        StartLine(state, true);
        start = newline + 1;
    }
}

std::wstring AttributeText(const MD_ATTRIBUTE& attribute) {
    if (attribute.text == nullptr || attribute.size == 0) return {};
    if (attribute.substr_offsets == nullptr || attribute.substr_types == nullptr)
        return MdTextToWide(attribute.text, attribute.size);
    std::wstring result;
    // Substring arrays end at an offset equal to size, not after size entries.
    for (MD_OFFSET index = 0; attribute.substr_offsets[index] < attribute.size; ++index) {
        const MD_OFFSET start = attribute.substr_offsets[index];
        const MD_OFFSET end = attribute.substr_offsets[index + 1];
        if (end <= start || end > attribute.size) break;
        const std::wstring piece = MdTextToWide(attribute.text + start, end - start);
        if (attribute.substr_types[index] == MD_TEXT_ENTITY) {
            result += DecodeEntity(piece);
        } else if (attribute.substr_types[index] == MD_TEXT_NULLCHAR) {
            result += L"\uFFFD";
        } else {
            result += piece;
        }
    }
    return result;
}

void PushStyle(RenderState& state, StyleSpec style) {
    state.style_stack.push_back(state.style);
    state.style = style;
}

void PopStyle(RenderState& state) {
    if (state.style_stack.empty()) return;
    state.style = state.style_stack.back();
    state.style_stack.pop_back();
}

int EnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto& state = *static_cast<RenderState*>(userdata);
    switch (type) {
        case MD_BLOCK_DOC:
            break;
        case MD_BLOCK_QUOTE:
            ++state.quote_depth;
            break;
        case MD_BLOCK_UL:
            state.lists.push_back({});
            break;
        case MD_BLOCK_OL: {
            ListState list;
            list.ordered = true;
            if (detail != nullptr) {
                const auto* ordered = static_cast<const MD_BLOCK_OL_DETAIL*>(detail);
                list.next = ordered->start;
                list.delimiter = ordered->mark_delimiter;
            }
            state.lists.push_back(list);
            break;
        }
        case MD_BLOCK_LI: {
            if (!state.lists.empty()) {
                ListState& list = state.lists.back();
                if (list.ordered) {
                    state.pending_marker = std::to_wstring(list.next++) + std::wstring(1, list.delimiter) + L" ";
                } else {
                    state.pending_marker = L"•  ";
                }
            } else {
                state.pending_marker = L"•  ";
            }
            if (detail != nullptr) {
                const auto* item = static_cast<const MD_BLOCK_LI_DETAIL*>(detail);
                if (item->is_task != 0) {
                    state.pending_marker = item->task_mark == L' ' ? L"[ ] " : L"[x] ";
                }
            }
            ++state.list_depth;
            break;
        }
        case MD_BLOCK_H:
            FinishLine(state);
            PushStyle(state, [&] {
                StyleSpec style = state.style;
                style.heading = true;
                return style;
            }());
            if (detail != nullptr) {
                state.heading_indent = static_cast<int>(static_cast<const MD_BLOCK_H_DETAIL*>(detail)->level - 1);
            }
            StartLine(state);
            break;
        case MD_BLOCK_CODE: {
            state.in_code = true;
            StyleSpec style = state.style;
            style.code = true;
            PushStyle(state, style);
            StartLine(state, true);
            if (detail != nullptr) {
                const std::wstring language = AttributeText(static_cast<const MD_BLOCK_CODE_DETAIL*>(detail)->lang);
                if (!language.empty()) {
                    AppendRun(state.current.runs, language, MarkdownStyle::Muted);
                    FinishLine(state);
                    StartLine(state, true);
                }
            }
            break;
        }
        case MD_BLOCK_HR:
            FinishLine(state);
            PushLine(state, MarkdownLine{{}, false, true, 0});
            AddSpacer(state);
            break;
        case MD_BLOCK_P:
            if (state.in_table_cell) break;
            PushStyle(state, [&] {
                StyleSpec style = state.style;
                style.heading = false;
                return style;
            }());
            StartLine(state);
            break;
        case MD_BLOCK_TABLE:
            state.in_table = true;
            break;
        case MD_BLOCK_TR:
            if (state.in_table) {
                FinishLine(state);
                state.first_table_cell = true;
                StartLine(state);
            }
            break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            if (state.in_table) {
                if (!state.first_table_cell) AppendRun(state.current.runs, L"   ", MarkdownStyle::Muted);
                state.first_table_cell = false;
                state.in_table_cell = true;
            }
            break;
        case MD_BLOCK_THEAD:
        case MD_BLOCK_TBODY:
        case MD_BLOCK_HTML:
        case MD_BLOCK_FOOTNOTE_DEF_SECTION:
        case MD_BLOCK_FOOTNOTE_DEF:
        case MD_BLOCK_ADMONITION:
            break;
        case MD_BLOCK_BLANK:
            AddSpacer(state);
            break;
    }
    return 0;
}

int LeaveBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    static_cast<void>(detail);
    auto& state = *static_cast<RenderState*>(userdata);
    switch (type) {
        case MD_BLOCK_QUOTE:
            FinishLine(state);
            state.quote_depth = std::max(0, state.quote_depth - 1);
            break;
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            FinishLine(state);
            if (!state.lists.empty()) state.lists.pop_back();
            break;
        case MD_BLOCK_LI:
            FinishLine(state);
            state.list_depth = std::max(0, state.list_depth - 1);
            state.pending_marker.clear();
            break;
        case MD_BLOCK_H:
            FinishLine(state);
            state.heading_indent = 0;
            PopStyle(state);
            AddSpacer(state);
            break;
        case MD_BLOCK_CODE:
            FinishLine(state);
            state.in_code = false;
            PopStyle(state);
            AddSpacer(state);
            break;
        case MD_BLOCK_P:
            if (!state.in_table_cell) {
                FinishLine(state);
                PopStyle(state);
                AddSpacer(state);
            }
            break;
        case MD_BLOCK_TABLE:
            FinishLine(state);
            state.in_table = false;
            AddSpacer(state);
            break;
        case MD_BLOCK_TR:
            FinishLine(state);
            break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            state.in_table_cell = false;
            break;
        case MD_BLOCK_DOC:
        case MD_BLOCK_HR:
        case MD_BLOCK_HTML:
        case MD_BLOCK_THEAD:
        case MD_BLOCK_TBODY:
        case MD_BLOCK_FOOTNOTE_DEF_SECTION:
        case MD_BLOCK_FOOTNOTE_DEF:
        case MD_BLOCK_ADMONITION:
        case MD_BLOCK_BLANK:
            break;
    }
    return 0;
}

int EnterSpan(MD_SPANTYPE type, void* detail, void* userdata) {
    static_cast<void>(detail);
    auto& state = *static_cast<RenderState*>(userdata);
    StyleSpec style = state.style;
    switch (type) {
        case MD_SPAN_STRONG:
            style.bold = true;
            break;
        case MD_SPAN_EM:
            style.italic = true;
            break;
        case MD_SPAN_CODE:
            style.code = true;
            break;
        case MD_SPAN_A:
        case MD_SPAN_WIKILINK:
            style.link = true;
            break;
        case MD_SPAN_DEL:
        case MD_SPAN_IMG:
        case MD_SPAN_SPOILER:
            style.muted = true;
            break;
        case MD_SPAN_INS:
        case MD_SPAN_LATEXMATH:
        case MD_SPAN_LATEXMATH_DISPLAY:
        case MD_SPAN_U:
        case MD_SPAN_SUPERSCRIPT:
        case MD_SPAN_SUBSCRIPT:
        case MD_SPAN_FOOTNOTE_REF:
        case MD_SPAN_MARK:
            break;
    }
    PushStyle(state, style);
    return 0;
}

int LeaveSpan(MD_SPANTYPE type, void* detail, void* userdata) {
    static_cast<void>(type);
    static_cast<void>(detail);
    PopStyle(*static_cast<RenderState*>(userdata));
    return 0;
}

int TextCallback(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto& state = *static_cast<RenderState*>(userdata);
    const std::wstring value = MdTextToWide(text, size);
    switch (type) {
        case MD_TEXT_NULLCHAR:
            AppendText(state, L"\uFFFD", ToStyle(state.style));
            break;
        case MD_TEXT_BR:
            FinishLine(state);
            StartLine(state);
            break;
        case MD_TEXT_SOFTBR:
            AppendText(state, L" ", ToStyle(state.style));
            break;
        case MD_TEXT_ENTITY:
            AppendText(state, DecodeEntity(value), ToStyle(state.style));
            break;
        case MD_TEXT_CODE:
            AppendCodeText(state, value);
            break;
        case MD_TEXT_HTML:
            AppendText(state, value, MarkdownStyle::Muted);
            break;
        case MD_TEXT_LATEXMATH:
            AppendText(state, value, MarkdownStyle::Code);
            break;
        case MD_TEXT_NORMAL:
            AppendText(state, value, ToStyle(state.style));
            break;
    }
    return 0;
}

void DebugLog(const char* message, void* userdata) {
    static_cast<void>(message);
    static_cast<void>(userdata);
}

std::wstring TrimMarkdownLine(std::wstring line) {
    if (!line.empty() && line.back() == L'\r') line.pop_back();
    line = Trim(std::move(line));
    return line;
}

std::wstring StripBlockquotePrefix(std::wstring line) {
    line = Trim(std::move(line));
    while (!line.empty() && line.front() == L'>') {
        line.erase(line.begin());
        line = Trim(std::move(line));
    }
    return line;
}

bool ParseFenceMarker(const std::wstring& line, wchar_t* marker, size_t* marker_length) {
    std::wstring trimmed = TrimMarkdownLine(line);
    size_t leading = 0;
    while (leading < trimmed.size() && trimmed[leading] == L' ') ++leading;
    if (leading > 3) return false;
    trimmed = StripBlockquotePrefix(trimmed.substr(leading));
    if (trimmed.empty() || (trimmed.front() != L'`' && trimmed.front() != L'~')) return false;
    const wchar_t candidate = trimmed.front();
    size_t length = 0;
    while (length < trimmed.size() && trimmed[length] == candidate) ++length;
    if (length < 3) return false;
    *marker = candidate;
    *marker_length = length;
    return true;
}

bool IsMarkdownFence(const std::wstring& line, wchar_t* marker, size_t* marker_length) {
    wchar_t parsed_marker = 0;
    size_t parsed_length = 0;
    if (!ParseFenceMarker(line, &parsed_marker, &parsed_length)) return false;
    std::wstring trimmed = StripBlockquotePrefix(TrimMarkdownLine(line));
    const size_t leading = trimmed.find_first_not_of(L' ');
    if (leading != std::wstring::npos && leading <= 3) trimmed = trimmed.substr(leading);
    if (trimmed.size() < parsed_length) return false;
    std::wstring info = TrimMarkdownLine(trimmed.substr(parsed_length));
    const size_t space = info.find_first_of(L" \t");
    if (space != std::wstring::npos) info.resize(space);
    if (!EqualsIgnoreCaseAscii(WideToUtf8(info), "md") && !EqualsIgnoreCaseAscii(WideToUtf8(info), "markdown")) {
        return false;
    }
    *marker = parsed_marker;
    *marker_length = parsed_length;
    return true;
}

bool IsFenceClose(const std::wstring& line, wchar_t marker, size_t marker_length) {
    wchar_t parsed_marker = 0;
    size_t parsed_length = 0;
    if (!ParseFenceMarker(line, &parsed_marker, &parsed_length)) return false;
    if (parsed_marker != marker || parsed_length < marker_length) return false;
    std::wstring trimmed = StripBlockquotePrefix(TrimMarkdownLine(line));
    const size_t leading = trimmed.find_first_not_of(L' ');
    if (leading != std::wstring::npos && leading <= 3) trimmed = trimmed.substr(leading);
    return trimmed.size() >= parsed_length && TrimMarkdownLine(trimmed.substr(parsed_length)).empty();
}

bool IsTableDelimiterSegment(const std::wstring& raw_segment) {
    std::wstring segment = TrimMarkdownLine(raw_segment);
    if (segment.empty()) return false;
    if (segment.front() == L':') segment.erase(segment.begin());
    if (!segment.empty() && segment.back() == L':') segment.pop_back();
    if (segment.size() < 3) return false;
    for (wchar_t character : segment) {
        if (character != L'-') return false;
    }
    return true;
}

bool IsTableDelimiterLine(const std::wstring& line) {
    std::wstring trimmed = StripBlockquotePrefix(TrimMarkdownLine(line));
    if (trimmed.empty() || trimmed.find(L'|') == std::wstring::npos) return false;
    size_t start = 0;
    if (!trimmed.empty() && trimmed.front() == L'|') start = 1;
    size_t end = trimmed.size();
    if (end > start && trimmed[end - 1] == L'|') --end;
    if (end <= start) return false;
    size_t cell_start = start;
    for (size_t index = start; index <= end; ++index) {
        if (index == end || trimmed[index] == L'|') {
            if (!IsTableDelimiterSegment(trimmed.substr(cell_start, index - cell_start))) return false;
            cell_start = index + 1;
        }
    }
    return true;
}

bool ContainsMarkdownTable(const std::vector<std::wstring>& lines) {
    std::wstring previous;
    for (const std::wstring& raw : lines) {
        const std::wstring current = StripBlockquotePrefix(TrimMarkdownLine(raw));
        if (current.empty()) {
            previous.clear();
            continue;
        }
        if (!previous.empty() && previous.find(L'|') != std::wstring::npos && IsTableDelimiterLine(current)) return true;
        previous = current;
    }
    return false;
}

std::wstring UnwrapMarkdownTableFences(const std::wstring& source) {
    if (source.find(L"```") == std::wstring::npos && source.find(L"~~~") == std::wstring::npos) return source;

    std::vector<std::wstring> lines;
    std::vector<bool> has_newline;
    size_t start = 0;
    while (start <= source.size()) {
        const size_t end = source.find(L'\n', start);
        lines.push_back(source.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        has_newline.push_back(end != std::wstring::npos);
        if (end == std::wstring::npos) break;
        start = end + 1;
    }

    std::wstring output;
    output.reserve(source.size());
    const auto append_line = [&](size_t index) {
        output += lines[index];
        if (has_newline[index]) output.push_back(L'\n');
    };

    bool candidate = false;
    wchar_t marker = 0;
    size_t marker_length = 0;
    size_t opening_index = 0;
    std::vector<std::wstring> body;

    for (size_t index = 0; index < lines.size(); ++index) {
        if (candidate) {
            if (IsFenceClose(lines[index], marker, marker_length)) {
                if (ContainsMarkdownTable(body)) {
                    for (const std::wstring& line : body) output += line + L"\n";
                } else {
                    append_line(opening_index);
                    for (const std::wstring& line : body) output += line + L"\n";
                    append_line(index);
                }
                candidate = false;
                body.clear();
                continue;
            }
            body.push_back(lines[index]);
            continue;
        }

        wchar_t parsed_marker = 0;
        size_t parsed_length = 0;
        if (IsMarkdownFence(lines[index], &parsed_marker, &parsed_length)) {
            candidate = true;
            marker = parsed_marker;
            marker_length = parsed_length;
            opening_index = index;
            body.clear();
            continue;
        }
        append_line(index);
    }

    if (candidate) {
        append_line(opening_index);
        for (const std::wstring& line : body) output += line + L"\n";
    }
    return output;
}

std::wstring RewriteFileCitations(const std::wstring& source) {
    if (source.find(L":codex-file-citation{") == std::wstring::npos) return source;
    std::wstring output;
    output.reserve(source.size());
    size_t index = 0;
    while (index < source.size()) {
        const size_t start = source.find(L":codex-file-citation{", index);
        if (start == std::wstring::npos) {
            output += source.substr(index);
            break;
        }
        output += source.substr(index, start - index);
        size_t cursor = start + std::wstring(L":codex-file-citation{").size();
        size_t end = cursor;
        bool quoted = false;
        while (end < source.size()) {
            if (source[end] == L'\\' && end + 1 < source.size()) {
                end += 2;
                continue;
            }
            if (source[end] == L'"') quoted = !quoted;
            if (source[end] == L'}' && !quoted) break;
            ++end;
        }
        if (end >= source.size()) {
            output += source.substr(start);
            break;
        }
        const std::wstring directive = source.substr(cursor, end - cursor);
        const size_t path_start = directive.find(L"path=\"");
        if (path_start == std::wstring::npos) {
            output += source.substr(start, end - start + 1);
            index = end + 1;
            continue;
        }
        const size_t value_start = path_start + 6;
        size_t value_end = value_start;
        while (value_end < directive.size()) {
            if (directive[value_end] == L'\\' && value_end + 1 < directive.size()) {
                value_end += 2;
                continue;
            }
            if (directive[value_end] == L'"') break;
            ++value_end;
        }
        if (value_end >= directive.size()) {
            output += source.substr(start, end - start + 1);
            index = end + 1;
            continue;
        }
        output += L"`" + directive.substr(value_start, value_end - value_start) + L"`";
        index = end + 1;
    }
    return output;
}

std::string_view TrimAsciiLine(std::string_view value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    return value;
}

std::string_view StripBlockquoteView(std::string_view line) {
    line = TrimAsciiLine(line);
    while (!line.empty() && line.front() == '>') {
        line.remove_prefix(1);
        line = TrimAsciiLine(line);
    }
    return line;
}

bool TryOpenStreamingFence(std::string_view line, char* marker, size_t* marker_length) {
    size_t leading = 0;
    while (leading < line.size() && line[leading] == ' ') ++leading;
    if (leading > 3) return false;
    line = StripBlockquoteView(line.substr(leading));
    if (line.empty() || (line.front() != '`' && line.front() != '~')) return false;
    const char candidate = line.front();
    size_t length = 0;
    while (length < line.size() && line[length] == candidate) ++length;
    if (length < 3) return false;
    *marker = candidate;
    *marker_length = length;
    return true;
}

bool IsStreamingFenceClose(std::string_view line, char marker, size_t marker_length) {
    size_t leading = 0;
    while (leading < line.size() && line[leading] == ' ') ++leading;
    if (leading > 3) return false;
    line = StripBlockquoteView(line.substr(leading));
    if (line.empty() || line.front() != marker) return false;
    size_t length = 0;
    while (length < line.size() && line[length] == marker) ++length;
    return length >= marker_length && TrimAsciiLine(line.substr(length)).empty();
}

bool IsMarkdownContainerLine(std::string_view line) {
    const std::string_view trimmed = StripBlockquoteView(line);
    if (trimmed.empty()) return false;
    const char first = trimmed.front();
    if (first == '>' || first == '-' || first == '*' || first == '+') return true;
    if (first == '|' || trimmed.find('|') != std::string_view::npos) return true;
    if (first == '[' && trimmed.find("]:") != std::string_view::npos) return true;
    if (first >= '0' && first <= '9') {
        size_t index = 0;
        while (index < trimmed.size() && trimmed[index] >= '0' && trimmed[index] <= '9') ++index;
        if (index > 0 && index + 1 < trimmed.size() && (trimmed[index] == '.' || trimmed[index] == ')') &&
            (trimmed[index + 1] == ' ' || trimmed[index + 1] == '\t')) {
            return true;
        }
    }
    size_t leading = 0;
    while (leading < line.size() && (line[leading] == ' ' || line[leading] == '\t')) ++leading;
    return leading >= 2;
}

std::vector<MarkdownLine> FallbackParse(const std::wstring& source) {
    std::vector<MarkdownLine> lines;
    size_t start = 0;
    while (start <= source.size()) {
        const size_t end = source.find(L'\n', start);
        std::wstring line = source.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        MarkdownLine parsed;
        AppendRun(parsed.runs, std::move(line), MarkdownStyle::Body);
        lines.push_back(std::move(parsed));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return lines;
}

}  // namespace

std::vector<MarkdownLine> ParseMarkdown(const std::wstring& source) {
    if (source.empty()) return {};
    std::wstring sanitized = Utf8ToWide(SanitizeDisplayText(WideToUtf8(source)));
    if (sanitized.empty() && !source.empty()) sanitized = source;
    const std::wstring prepared = RewriteFileCitations(UnwrapMarkdownTableFences(sanitized));
    if (prepared.size() > kMaxMarkdownBytes) {
        std::vector<MarkdownLine> fallback = FallbackParse(prepared.substr(0, kMaxMarkdownBytes));
        MarkdownLine notice;
        AppendRun(notice.runs, L"[Markdown rendering truncated: message is too large]", MarkdownStyle::Muted);
        fallback.push_back(std::move(notice));
        return fallback;
    }

    RenderState state;
    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_NOHTML | MD_FLAG_HARD_SOFT_BREAKS;
    parser.enter_block = EnterBlock;
    parser.leave_block = LeaveBlock;
    parser.enter_span = EnterSpan;
    parser.leave_span = LeaveSpan;
    parser.text = TextCallback;
    parser.debug_log = DebugLog;
    parser.syntax = nullptr;

#ifdef MD4C_USE_UTF16
    const int parsed = md_parse(prepared.data(), static_cast<MD_SIZE>(prepared.size()), &parser, &state);
#else
    const std::string utf8 = WideToUtf8(prepared);
    if (utf8.empty()) return FallbackParse(prepared);
    const int parsed = md_parse(utf8.data(), static_cast<MD_SIZE>(utf8.size()), &parser, &state);
#endif
    FinishLine(state);
    while (!state.lines.empty() && state.lines.back().runs.empty() && !state.lines.back().rule) state.lines.pop_back();
    if (parsed != 0 && state.lines.empty()) return FallbackParse(prepared);
    if (state.limit_reached) {
        MarkdownLine notice;
        AppendRun(notice.runs, L"[Markdown rendering truncated: too many blocks]", MarkdownStyle::Muted);
        PushLine(state, std::move(notice));
    }
    return std::move(state.lines);
}

std::shared_ptr<const std::vector<MarkdownLine>> ParseMarkdownCached(const std::wstring& source) {
    using MarkdownLines = std::vector<MarkdownLine>;
    using CacheEntry = std::pair<std::wstring, std::shared_ptr<const MarkdownLines>>;
    struct Cache {
        std::list<CacheEntry> entries;
        std::unordered_map<std::wstring, std::list<CacheEntry>::iterator> index;
    };
    static Cache cache;

    const auto found = cache.index.find(source);
    if (found != cache.index.end()) {
        cache.entries.splice(cache.entries.begin(), cache.entries, found->second);
        return found->second->second;
    }

    auto parsed = std::make_shared<const MarkdownLines>(ParseMarkdown(source));
    cache.entries.emplace_front(source, parsed);
    cache.index[source] = cache.entries.begin();
    constexpr size_t kCacheLimit = 16;
    if (cache.entries.size() > kCacheLimit) {
        cache.index.erase(cache.entries.back().first);
        cache.entries.pop_back();
    }
    return parsed;
}

void StreamingMarkdown::Append(std::string_view utf8) {
    if (utf8.empty()) return;
    source_.append(utf8.data(), utf8.size());
    ++revision_;
    tail_cache_revision_ = 0;
    ScanCompleteLines();
}

void StreamingMarkdown::Finish() {
    ScanCompleteLines();
    CommitThrough(source_.size());
    // The rendered line model is now authoritative. Releasing the source copy keeps
    // long restored transcripts from storing every assistant reply twice.
    source_.clear();
    source_.shrink_to_fit();
    scanned_bytes_ = 0;
    committed_bytes_ = 0;
    tail_cache_revision_ = 0;
    tail_lines_.reset();
    ++revision_;
}

void StreamingMarkdown::Reset() {
    source_.clear();
    scanned_bytes_ = 0;
    committed_bytes_ = 0;
    stable_lines_.clear();
    in_fence_ = false;
    fence_marker_ = 0;
    fence_length_ = 0;
    container_active_ = false;
    after_blank_line_ = false;
    ++revision_;
    tail_cache_revision_ = 0;
    tail_lines_.reset();
}

std::shared_ptr<const std::vector<MarkdownLine>> StreamingMarkdown::TailLines() const {
    if (tail_cache_revision_ != revision_ || !tail_lines_) {
        tail_lines_ = ParseMarkdownCached(Utf8ToWide(TailSource()));
        tail_cache_revision_ = revision_;
    }
    return tail_lines_;
}

std::string_view StreamingMarkdown::TailSource() const {
    return committed_bytes_ < source_.size() ? std::string_view(source_).substr(committed_bytes_) : std::string_view{};
}

void StreamingMarkdown::ScanCompleteLines() {
    size_t line_start = scanned_bytes_;
    while (line_start < source_.size()) {
        const size_t newline = source_.find('\n', line_start);
        if (newline == std::string::npos) break;
        const size_t line_end = newline + 1;
        std::string_view line(source_.data() + line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\n') line.remove_suffix(1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        if (in_fence_) {
            if (IsStreamingFenceClose(line, fence_marker_, fence_length_)) {
                in_fence_ = false;
                container_active_ = false;
                after_blank_line_ = false;
            }
        } else {
            char marker = 0;
            size_t marker_length = 0;
            if (TryOpenStreamingFence(line, &marker, &marker_length)) {
                in_fence_ = true;
                fence_marker_ = marker;
                fence_length_ = marker_length;
                container_active_ = true;
                after_blank_line_ = false;
            } else if (TrimAsciiLine(line).empty()) {
                if (!container_active_) CommitThrough(line_end);
                after_blank_line_ = true;
            } else {
                const bool container = IsMarkdownContainerLine(line);
                container_active_ = after_blank_line_ ? container : (container_active_ || container);
                after_blank_line_ = false;
            }
        }
        line_start = line_end;
    }
    scanned_bytes_ = line_start;
}

void StreamingMarkdown::CommitThrough(size_t end) {
    if (end <= committed_bytes_) return;
    const std::wstring segment = Utf8ToWide(source_.substr(committed_bytes_, end - committed_bytes_));
    const std::shared_ptr<const std::vector<MarkdownLine>> parsed = ParseMarkdownCached(segment);
    if (!stable_lines_.empty() && parsed != nullptr && !parsed->empty()) stable_lines_.push_back({});
    stable_lines_.insert(stable_lines_.end(), parsed->begin(), parsed->end());
    committed_bytes_ = end;
    ++revision_;
    tail_cache_revision_ = 0;
}

}  // namespace markdown
}  // namespace spark
