#include "render/markdown.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

bool HasRun(const std::vector<spark::markdown::MarkdownLine>& lines, const std::wstring& text,
            spark::markdown::MarkdownStyle style) {
    for (const auto& line : lines) {
        for (const auto& run : line.runs) {
            if (run.text == text && run.style == style) return true;
        }
    }
    return false;
}

bool HasTextPart(const std::vector<spark::markdown::MarkdownLine>& lines, const std::wstring& text) {
    for (const auto& line : lines) {
        std::wstring combined;
        bool code = false;
        for (const auto& run : line.runs) {
            combined += run.text;
            code = code || run.style == spark::markdown::MarkdownStyle::Code;
        }
        if (combined.find(text) != std::wstring::npos || (code && text == L"code")) return true;
    }
    return false;
}

int EmptyLineCount(const std::vector<spark::markdown::MarkdownLine>& lines) {
    int count = 0;
    for (const auto& line : lines) {
        if (line.runs.empty() && !line.rule) ++count;
    }
    return count;
}

bool SameRendering(const std::vector<spark::markdown::MarkdownLine>& left,
                   const std::vector<spark::markdown::MarkdownLine>& right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](const auto& first, const auto& second) {
               return first.code == second.code && first.rule == second.rule && first.indent == second.indent &&
                      first.runs.size() == second.runs.size() &&
                      std::equal(first.runs.begin(), first.runs.end(), second.runs.begin(),
                                 [](const auto& a, const auto& b) { return a.text == b.text && a.style == b.style; });
           });
}

}  // namespace

int main() {
    using spark::markdown::MarkdownLine;
    using spark::markdown::MarkdownStyle;

    const auto headings = spark::markdown::ParseMarkdown(L"# Title\n\nBody");
    Check(HasRun(headings, L"Title", spark::markdown::MarkdownStyle::Heading), "heading style");
    Check(HasTextPart(headings, L"Body"), "paragraph body");

    const auto emphasis = spark::markdown::ParseMarkdown(L"**bold** *italic* `code` ~~old~~");
    Check(HasRun(emphasis, L"bold", spark::markdown::MarkdownStyle::Bold), "strong style");
    Check(HasRun(emphasis, L"italic", spark::markdown::MarkdownStyle::Italic), "emphasis style");
    Check(HasRun(emphasis, L"code", spark::markdown::MarkdownStyle::Code), "inline code style");
    Check(HasRun(emphasis, L"old", spark::markdown::MarkdownStyle::Muted), "strikethrough style");

    const auto table = spark::markdown::ParseMarkdown(L"| A | B |\n| --- | --- |\n| 1 | 2 |");
    Check(HasTextPart(table, L"A"), "table header");
    Check(HasTextPart(table, L"1"), "table body");

    const auto lists = spark::markdown::ParseMarkdown(L"- one\n\n  - two\n1. three");
    Check(HasTextPart(lists, L"•  one"), "unordered list marker");
    Check(HasTextPart(lists, L"•  two"), "nested list marker");
    Check(HasTextPart(lists, L"1. three"), "ordered list marker");

    const auto task = spark::markdown::ParseMarkdown(L"- [x] done");
    Check(HasTextPart(task, L"[x] done"), "task list marker");

    const auto quote = spark::markdown::ParseMarkdown(L"> quoted");
    Check(HasTextPart(quote, L"│ quoted"), "block quote marker");

    const auto code = spark::markdown::ParseMarkdown(L"```cpp\nint value = 1;\n```");
    Check(HasRun(code, L"int value = 1;", spark::markdown::MarkdownStyle::Code), "fenced code block");

    const auto link = spark::markdown::ParseMarkdown(L"[OpenAI](https://openai.com)");
    Check(HasRun(link, L"OpenAI", spark::markdown::MarkdownStyle::Link), "link style");

    const auto entity = spark::markdown::ParseMarkdown(L"&amp; &lt; &#65;");
    Check(HasTextPart(entity, L"& < A"), "entity decoding");

    const std::wstring unicode_source =
        L"# \u6807\u9898 \U0001F680\n\n"
        L"\u4F60\u597D **\u4E16\u754C \U0001F9EA** [caf\u00E9 \U0001F600](https://example.invalid/)\n\n"
        L"`\u53D8\u91CF \U0001F680`";
    const std::vector<MarkdownLine> unicode_expected = {
        {{{L"\u6807\u9898 \U0001F680", MarkdownStyle::Heading}}},
        {},
        {{{L"\u4F60\u597D ", MarkdownStyle::Body},
          {L"\u4E16\u754C \U0001F9EA", MarkdownStyle::Bold},
          {L" ", MarkdownStyle::Body},
          {L"caf\u00E9 \U0001F600", MarkdownStyle::Link}}},
        {},
        {{{L"\u53D8\u91CF \U0001F680", MarkdownStyle::Code}}},
    };
    Check(SameRendering(spark::markdown::ParseMarkdown(unicode_source), unicode_expected),
          "Unicode and non-BMP text preserve complete heading, emphasis, link and inline-code rendering");
    const auto unicode_cached = spark::markdown::ParseMarkdownCached(unicode_source);
    Check(unicode_cached == spark::markdown::ParseMarkdownCached(unicode_source) &&
              SameRendering(*unicode_cached, unicode_expected),
          "cached Unicode rendering matches every line, run, style and indent");

    const auto unicode_entities = spark::markdown::ParseMarkdown(
        L"\U0001F680 &#128640; &#x1F680; &#X1f600; &#1114111; &amp; &nbsp; &#0;");
    const std::vector<MarkdownLine> entities_expected = {
        {{{L"\U0001F680 \U0001F680 \U0001F680 \U0001F600 \U0010FFFF & \u00A0 \uFFFD",
           MarkdownStyle::Body}}},
    };
    Check(SameRendering(unicode_entities, entities_expected),
          "numeric entities use native wchar_t representation through the maximum Unicode scalar");

    const auto unicode_fence = spark::markdown::ParseMarkdown(
        L"```c\u8BED\U0001F680&amp;&#x1F680;\n\u53D8\u91CF = \"\U0001F600 &amp;\";\n```");
    const std::vector<MarkdownLine> fence_expected = {
        {{{L"c\u8BED\U0001F680&\U0001F680", MarkdownStyle::Muted}}, true},
        {{{L"\u53D8\u91CF = \"\U0001F600 &amp;\";", MarkdownStyle::Code}}, true},
    };
    Check(SameRendering(unicode_fence, fence_expected),
          "fence attributes decode Unicode and entity substrings while code contents remain literal");

    const auto unicode_table = spark::markdown::ParseMarkdown(
        L"| \u6807\u9898 | \U0001F680 |\n| --- | --- |\n| &#128640; | \u4E16\u754C |");
    const std::vector<MarkdownLine> table_expected = {
        {{{L"\u6807\u9898", MarkdownStyle::Body},
          {L"   ", MarkdownStyle::Muted},
          {L"\U0001F680", MarkdownStyle::Body}}},
        {{{L"\U0001F680", MarkdownStyle::Body},
          {L"   ", MarkdownStyle::Muted},
          {L"\u4E16\u754C", MarkdownStyle::Body}}},
    };
    Check(SameRendering(unicode_table, table_expected),
          "Unicode table cells and numeric entities preserve row and cell boundaries");

    const auto fenced_table = spark::markdown::ParseMarkdown(
        L"```markdown\n| A | B |\n| --- | --- |\n| 1 | 2 |\n```");
    Check(HasTextPart(fenced_table, L"A") && HasTextPart(fenced_table, L"1"),
          "markdown fence containing a table is unwrapped");

    const auto fenced_text = spark::markdown::ParseMarkdown(L"```markdown\nplain text\n```");
    Check(HasRun(fenced_text, L"plain text", spark::markdown::MarkdownStyle::Code),
          "markdown fence without a table remains a code block");

    const auto citation = spark::markdown::ParseMarkdown(L"See :codex-file-citation{path=\"src/main.cpp\"} now");
    Check(HasRun(citation, L"src/main.cpp", spark::markdown::MarkdownStyle::Code),
          "codex file citation is rendered as a readable path");

    const auto controls = spark::markdown::ParseMarkdown(L"before\x1b[31mafter");
    Check(HasTextPart(controls, L"beforeafter"), "markdown rendering strips ANSI controls");

    spark::markdown::StreamingMarkdown stream;
    stream.Append("# Title\n\n");
    Check(HasRun(stream.StableLines(), L"Title", spark::markdown::MarkdownStyle::Heading),
          "streaming markdown commits a completed top-level block");
    Check(stream.TailSource().empty(), "streaming markdown has no tail after a blank boundary");
    stream.Append("first line\n");
    Check(stream.TailSource() == "first line\n", "streaming markdown keeps an open paragraph mutable");
    stream.Append("\nsecond paragraph\n\n");
    Check(stream.TailSource().empty(), "streaming markdown commits after the paragraph closes");
    Check(HasTextPart(stream.StableLines(), L"first line") &&
              HasTextPart(stream.StableLines(), L"second paragraph"),
          "streaming markdown retains committed paragraphs");
    Check(EmptyLineCount(stream.StableLines()) >= 2,
          "streaming markdown preserves block spacing between committed segments");

    spark::markdown::StreamingMarkdown fence_stream;
    fence_stream.Append("```cpp\nint value = 1;\n\n```\n\n");
    Check(HasRun(fence_stream.StableLines(), L"int value = 1;", spark::markdown::MarkdownStyle::Code),
          "streaming markdown does not split a fenced block on an internal blank line");
    Check(fence_stream.TailSource().empty(), "streaming markdown commits a closed fenced block");

    spark::markdown::StreamingMarkdown list_stream;
    list_stream.Append("- one\n\n- two\n");
    Check(!list_stream.TailSource().empty(), "streaming markdown keeps a live list mutable");
    list_stream.Finish();
    Check(HasTextPart(list_stream.StableLines(), L"•  one") &&
              HasTextPart(list_stream.StableLines(), L"•  two"),
          "streaming markdown finalizes a list without losing markers");
    Check(list_stream.TailSource().empty(), "streaming markdown finalization clears the tail");

    const std::string unicode_utf8 =
        u8"# \u6807\u9898 \U0001F680\n\n"
        u8"\u4F60\u597D **\u4E16\u754C \U0001F9EA** [caf\u00E9 \U0001F600](https://example.invalid/)\n\n"
        u8"`\u53D8\u91CF \U0001F680`";
    spark::markdown::StreamingMarkdown unicode_stream;
    for (size_t index = 0; index < unicode_utf8.size(); ++index) {
        unicode_stream.Append(std::string_view(unicode_utf8.data() + index, 1));
        unicode_stream.TailLines();
    }
    Check(SameRendering(*unicode_stream.TailLines(), {unicode_expected.back()}),
          "live Unicode tail renders complete non-BMP characters before finalization");
    unicode_stream.Finish();
    Check(SameRendering(unicode_stream.StableLines(), unicode_expected),
          "one-byte UTF-8 chunks including split non-BMP characters match complete rendering");
    Check(unicode_stream.TailSource().empty() && unicode_stream.SourceBytes() == 0,
          "Unicode stream finalization releases source bytes and clears the tail");
    unicode_stream.Reset();
    Check(unicode_stream.StableLines().empty() && unicode_stream.TailLines()->empty(),
          "reset clears committed Unicode rendering and cached tail");

    spark::markdown::StreamingMarkdown entity_stream;
    entity_stream.Append("&#x1F6");
    entity_stream.TailLines();
    entity_stream.Append("80; &am");
    entity_stream.TailLines();
    entity_stream.Append("p;");
    const std::vector<MarkdownLine> entity_stream_expected = {
        {{{L"\U0001F680 &", MarkdownStyle::Body}}},
    };
    Check(SameRendering(*entity_stream.TailLines(), entity_stream_expected),
          "live tail reparses numeric and named entities after their split terminators arrive");
    entity_stream.Append("\n\n");
    Check(SameRendering(entity_stream.StableLines(), entity_stream_expected) && entity_stream.TailSource().empty(),
          "split numeric and named entities render correctly when their paragraph commits");

    if (failures != 0) {
        std::cerr << failures << " markdown test(s) failed\n";
        return 1;
    }
    std::cout << "all markdown tests passed\n";
    return 0;
}
