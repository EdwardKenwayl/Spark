#include "render/markdown.h"
#include "gui/chat_ui_helpers.h"

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
                                 [](const auto& a, const auto& b) {
                                     return a.text == b.text && a.style == b.style && a.destination == b.destination;
                                 });
           });
}

}  // namespace

int main() {
    using spark::markdown::MarkdownLine;
    using spark::markdown::MarkdownStyle;

    using namespace spark::ui;
    const std::string original = u8"[附件引用]\n- C:\\work\\photo.png\n- /tmp/notes.md\n\n保留正文\nsecond line";
    std::string source = original;
    Check(UserMessageText(source) == u8"保留正文\nsecond line" && source == original,
          "display filters generated attachments without modifying source");
    Check(UserMessageText(u8"[附件引用]\r\n- \\\\server\\share\\photo.png\r\n\r\nBody\r\n") == "Body\r\n",
          "CRLF attachment block retains body newlines");
    Check(UserMessageText(u8"[智能快照]\nCaptured text\n\n[附件引用]\n- /tmp/a.png\n\nBody") ==
          u8"[智能快照]\nCaptured text\n\nBody", "snapshot prefix remains visible");
    for (const auto& literal : {std::string(u8"[附件引用]\nnot generated\n\nBody"),
             std::string(u8"[附件引用]\n- relative.txt\n\nBody"),
             std::string(u8"[附件引用]\n- /tmp/a.png\nno blank terminator"),
             std::string(u8"Intro\n[附件引用]\n- /tmp/a.png\n\nBody"),
             std::string(u8"[智能快照]literal\n\n[附件引用]\n- /tmp/a.png\n\nBody")})
        Check(UserMessageText(literal) == literal, "literal heading and incomplete blocks are preserved");
    Check(UserMessageText(u8"[附件引用]\n- /tmp/a.png\n\n").empty(), "attachment-only display has no path block");
    std::vector<std::string> edited_attachments{"C:\\work\\photo.png"};
    Check(UserMessageText(original, &edited_attachments) == u8"保留正文\nsecond line" &&
              edited_attachments == std::vector<std::string>({"C:\\work\\photo.png", "/tmp/notes.md"}),
          "editing restores generated file references without duplicating structured images");
    const std::string incomplete = u8"[附件引用]\n- /tmp/a.png\nno blank terminator";
    edited_attachments.clear();
    Check(UserMessageText(incomplete, &edited_attachments) == incomplete && edited_attachments.empty(),
          "incomplete literal reference blocks never become selected attachments");
    Check(FileLinkTarget(L"C:\\work\\source.cpp:17:4") == L"C:\\work\\source.cpp" &&
          FileLinkTarget(L"file:///tmp/My%20File.cpp#L17-L20") == L"file:///tmp/My%20File.cpp" &&
          FileLinkTarget(L"/tmp/file%3A17") == L"/tmp/file%3A17", "source locations strip before URI decoding");
    Check(FileUri(u8"/tmp/a b,#中文.txt") == "file:///tmp/a%20b%2C%23%E4%B8%AD%E6%96%87.txt",
          "file URI escapes spaces commas hashes and UTF-8 bytes");
    const std::string unusual = "/tmp/a b,'$;.md";
    Check(FileRevealCommand(FileRevealPlatform::Linux, unusual) == std::vector<std::string>{
          "dbus-send", "--session", "--type=method_call", "--dest=org.freedesktop.FileManager1",
          "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1.ShowItems",
          "array:string:file:///tmp/a%20b%2C%27%24%3B.md", "string:"},
          "Linux ShowItems uses one encoded URI argument without a shell");
    Check(FileRevealCommand(FileRevealPlatform::MacOS, unusual) ==
          std::vector<std::string>{"open", "-R", "--", unusual}, "macOS reveal preserves a separate path argument");
    Check(FileRevealCommand(FileRevealPlatform::Linux, std::string("/tmp/a\0b", 8)).empty() &&
          FileRevealCommand(FileRevealPlatform::MacOS, "").empty(), "invalid reveal inputs produce no command");

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
    bool has_link_destination = false;
    for (const MarkdownLine& line : link) {
        for (const auto& run : line.runs) {
            if (run.text == L"OpenAI" && run.style == MarkdownStyle::Link &&
                run.destination == L"https://openai.com") {
                has_link_destination = true;
            }
        }
    }
    Check(has_link_destination, "link destination is retained for interaction");

    const auto file_link = spark::markdown::ParseMarkdown(L"[source](file:///C:/workspace/My%20File.cpp)");
    bool has_file_destination = false;
    for (const MarkdownLine& line : file_link) {
        for (const auto& run : line.runs) {
            if (run.text == L"source" && run.destination == L"file:///C:/workspace/My%20File.cpp") {
                has_file_destination = true;
            }
        }
    }
    Check(has_file_destination, "file link destination is retained");

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
          {L"caf\u00E9 \U0001F600", MarkdownStyle::Link, L"https://example.invalid/"}}},
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
