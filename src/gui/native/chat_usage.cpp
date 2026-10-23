#include "gui/native/chat_usage.h"
#include "gui/localization.h"

#include <algorithm>

namespace spark {
namespace {
std::wstring Tokens(uint64_t value) {
    auto text = std::to_wstring(value);
    for (int offset = static_cast<int>(text.size()) - 3; offset > 0; offset -= 3)
        text.insert(static_cast<size_t>(offset), 1, L',');
    return text;
}

std::wstring Elapsed(uint64_t milliseconds) {
    if (milliseconds < 60000)
        return i18n::Format(L"{seconds}s",
            {{L"seconds", wxString::Format("%.1f", static_cast<double>(milliseconds) / 1000.0).ToStdWstring()}});
    const uint64_t minutes = milliseconds / 60000;
    if (minutes < 60)
        return i18n::Format(L"{minutes}m {seconds}s",
            {{L"minutes", std::to_wstring(minutes)}, {L"seconds", std::to_wstring(milliseconds / 1000 % 60)}});
    return i18n::Format(L"{hours}h {minutes}m",
        {{L"hours", std::to_wstring(minutes / 60)}, {L"minutes", std::to_wstring(minutes % 60)}});
}

std::wstring Percent(uint64_t used, uint64_t total) {
    const double rate = total ? std::min(100.0, 100.0 * static_cast<double>(used) / static_cast<double>(total)) : 0.0;
    return wxString::Format("%.1f%%", rate).ToStdWstring();
}
}  // namespace

wxString NativeUsageText(const ChatItem& item) {
    return wxString(i18n::Format(
        L"Turn total {total} \u00b7 Input {input} \u00b7 Output {output} \u00b7 Cache read {cached} \u00b7 Cache hit {cache_rate} \u00b7 Context {context}/{limit} ({context_rate}) \u00b7 Calls {calls} \u00b7 Elapsed {elapsed}",
        {{L"total", Tokens(item.input_tokens + item.output_tokens)}, {L"input", Tokens(item.input_tokens)},
         {L"output", Tokens(item.output_tokens)}, {L"cached", Tokens(item.cached_input_tokens)},
         {L"cache_rate", Percent(item.cached_input_tokens, item.input_tokens)},
         {L"context", Tokens(item.context_tokens)}, {L"limit", Tokens(item.context_limit_tokens)},
         {L"context_rate", Percent(item.context_tokens, item.context_limit_tokens)},
         {L"calls", std::to_wstring(item.model_calls)}, {L"elapsed", Elapsed(item.elapsed_ms)}}));
}

}  // namespace spark
