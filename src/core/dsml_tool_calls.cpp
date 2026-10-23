#include "core/dsml_tool_calls.h"

#include "core/common.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <utility>

namespace spark {
namespace {

struct Tag {
    size_t begin = 0;
    size_t end = 0;
    bool closing = false;
    std::string body;
};

bool IsDsmlTag(const std::string& body) {
    return LowerAscii(body).find("dsml") != std::string::npos;
}

std::optional<Tag> ParseTagAt(const std::string& text, size_t begin) {
    if (begin >= text.size() || text[begin] != '<') return std::nullopt;
    const size_t end = text.find('>', begin + 1);
    if (end == std::string::npos) return std::nullopt;
    std::string body = text.substr(begin + 1, end - begin - 1);
    bool closing = false;
    size_t first = body.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && body[first] == '/') {
        closing = true;
        body.erase(first, 1);
    }
    if (!IsDsmlTag(body)) return std::nullopt;
    return Tag{begin, end + 1, closing, std::move(body)};
}

std::optional<Tag> FindTag(const std::string& text, size_t start, std::string_view keyword, bool closing) {
    const std::string lowered_keyword = LowerAscii(std::string(keyword));
    for (size_t position = text.find('<', start); position != std::string::npos;
         position = text.find('<', position + 1)) {
        const auto tag = ParseTagAt(text, position);
        if (!tag.has_value() || tag->closing != closing) continue;
        if (LowerAscii(tag->body).find(lowered_keyword) != std::string::npos) return tag;
    }
    return std::nullopt;
}

bool IsAttributeNameBoundary(char character) {
    return !std::isalnum(static_cast<unsigned char>(character)) && character != '_' && character != '-';
}

std::string Attribute(const std::string& tag, std::string_view name) {
    const std::string lowered = LowerAscii(tag);
    const std::string needle = LowerAscii(std::string(name)) + "=";
    size_t position = lowered.find(needle);
    while (position != std::string::npos) {
        if (position == 0 || IsAttributeNameBoundary(lowered[position - 1])) break;
        position = lowered.find(needle, position + 1);
    }
    if (position == std::string::npos) return {};
    size_t value = position + needle.size();
    while (value < tag.size() && std::isspace(static_cast<unsigned char>(tag[value]))) ++value;
    if (value >= tag.size()) return {};
    if (tag[value] == '"' || tag[value] == '\'') {
        const char quote = tag[value++];
        const size_t end = tag.find(quote, value);
        if (end == std::string::npos) return {};
        return tag.substr(value, end - value);
    }
    const size_t end = tag.find_first_of(" \t\r\n>", value);
    return tag.substr(value, end == std::string::npos ? std::string::npos : end - value);
}

std::string DecodeXmlEntities(std::string value) {
    std::string output;
    output.reserve(value.size());
    for (size_t index = 0; index < value.size();) {
        if (value[index] != '&') {
            output.push_back(value[index++]);
            continue;
        }
        const size_t end = value.find(';', index + 1);
        if (end == std::string::npos || end - index > 12) {
            output.push_back(value[index++]);
            continue;
        }
        const std::string entity = value.substr(index + 1, end - index - 1);
        if (entity == "amp") output.push_back('&');
        else if (entity == "lt") output.push_back('<');
        else if (entity == "gt") output.push_back('>');
        else if (entity == "quot") output.push_back('"');
        else if (entity == "apos") output.push_back('\'');
        else if (entity == "#34") output.push_back('"');
        else if (entity == "#39") output.push_back('\'');
        else {
            output.append(value, index, end - index + 1);
            index = end + 1;
            continue;
        }
        index = end + 1;
    }
    return output;
}

bool ParseInvoke(const std::string& text, size_t block_end, size_t* cursor, std::vector<ToolCall>* calls,
                 bool* malformed) {
    const auto invoke_open = FindTag(text, *cursor, "invoke", false);
    if (!invoke_open.has_value() || invoke_open->begin >= block_end) return false;
    const auto invoke_close = FindTag(text, invoke_open->end, "invoke", true);
    if (!invoke_close.has_value() || invoke_close->begin >= block_end) {
        *malformed = true;
        return false;
    }

    ToolCall call;
    call.name = Attribute(invoke_open->body, "name");
    nlohmann::json arguments = nlohmann::json::object();
    size_t parameter_cursor = invoke_open->end;
    while (parameter_cursor < invoke_close->begin) {
        const auto parameter_open = FindTag(text, parameter_cursor, "parameter", false);
        if (!parameter_open.has_value() || parameter_open->begin >= invoke_close->begin) break;
        const auto parameter_close = FindTag(text, parameter_open->end, "parameter", true);
        if (!parameter_close.has_value() || parameter_close->begin >= invoke_close->begin) {
            *malformed = true;
            break;
        }
        const std::string parameter_name = Attribute(parameter_open->body, "name");
        std::string parameter_value =
            DecodeXmlEntities(text.substr(parameter_open->end, parameter_close->begin - parameter_open->end));
        const bool string_value = LowerAscii(Attribute(parameter_open->body, "string")) == "true";
        if (!parameter_name.empty()) {
            if (string_value) {
                arguments[parameter_name] = parameter_value;
            } else {
                try {
                    arguments[parameter_name] = nlohmann::json::parse(Trim(parameter_value));
                } catch (const nlohmann::json::exception&) {
                    arguments[parameter_name] = parameter_value;
                }
            }
        }
        parameter_cursor = parameter_close->end;
    }

    if (call.name.empty()) {
        *malformed = true;
    } else {
        call.id = "dsml-" + std::to_string(calls->size() + 1);
        call.arguments_json = arguments.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        calls->push_back(std::move(call));
    }
    *cursor = invoke_close->end;
    return true;
}

bool LooksLikePartialDsml(const std::string& text) {
    const size_t open = text.rfind('<');
    if (open == std::string::npos || text.find('>', open) != std::string::npos) return false;
    return LowerAscii(text.substr(open)).find("dsml") != std::string::npos;
}

}  // namespace

DsmlParseResult ExtractDsmlToolCalls(const std::string& text) {
    DsmlParseResult result;
    size_t cursor = 0;
    while (cursor < text.size()) {
        const auto block_start = FindTag(text, cursor, "calls", false);
        if (!block_start.has_value()) {
            result.text += text.substr(cursor);
            break;
        }
        result.found = true;
        result.text += text.substr(cursor, block_start->begin - cursor);
        const auto block_end = FindTag(text, block_start->end, "calls", true);
        if (!block_end.has_value()) {
            result.incomplete = true;
            break;
        }

        size_t invoke_cursor = block_start->end;
        bool found_invoke = false;
        while (invoke_cursor < block_end->begin) {
            const auto next = FindTag(text, invoke_cursor, "invoke", false);
            if (!next.has_value() || next->begin >= block_end->begin) break;
            found_invoke = true;
            if (!ParseInvoke(text, block_end->begin, &invoke_cursor, &result.tool_calls, &result.malformed)) break;
        }
        if (!found_invoke) result.malformed = true;
        cursor = block_end->end;
    }
    result.text = Trim(std::move(result.text));
    return result;
}

std::string DsmlStreamFilter::Push(std::string_view chunk) {
    pending_.append(chunk.data(), chunk.size());
    std::string output;
    while (!pending_.empty()) {
        if (suppressing_) {
            const auto block_end = FindTag(pending_, 0, "calls", true);
            if (!block_end.has_value()) break;
            pending_.erase(0, block_end->end);
            suppressing_ = false;
            continue;
        }

        const auto block_start = FindTag(pending_, 0, "calls", false);
        if (block_start.has_value()) {
            output.append(pending_, 0, block_start->begin);
            pending_.erase(0, block_start->begin);
            suppressing_ = true;
            continue;
        }

        size_t safe = pending_.size();
        const size_t last_open = pending_.rfind('<');
        if (last_open != std::string::npos && pending_.find('>', last_open) == std::string::npos) safe = last_open;
        output.append(pending_, 0, safe);
        pending_.erase(0, safe);
        break;
    }
    return output;
}

std::string DsmlStreamFilter::Finish() {
    if (suppressing_ || LooksLikePartialDsml(pending_)) {
        incomplete_ = true;
        pending_.clear();
        return {};
    }
    std::string output = std::move(pending_);
    pending_.clear();
    return output;
}

}  // namespace spark
