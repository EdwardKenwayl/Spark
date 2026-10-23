#include "core/provider.h"

#include "platform/binary_io.h"
#include "platform/http.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <vector>

namespace spark {
namespace {

class SseDecoder {
public:
    using EventCallback = std::function<void(const std::string&, const std::string&)>;

    void Feed(const char* data, size_t size, const EventCallback& callback) {
        callback_ = callback;
        buffer_.append(data, size);
        size_t newline = 0;
        while ((newline = buffer_.find('\n')) != std::string::npos) {
            std::string line = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            ProcessLine(line, callback);
        }
    }

    void Finish() {
        if (!data_.empty()) {
            if (callback_) callback_(event_, data_);
            data_.clear();
            event_.clear();
        }
    }

private:
    void ProcessLine(const std::string& line, const EventCallback& callback) {
        if (line.empty()) {
            if (!data_.empty()) {
                callback(event_, data_);
                data_.clear();
                event_.clear();
            }
            return;
        }
        if (line[0] == ':') return;
        const size_t separator = line.find(':');
        const std::string field = separator == std::string::npos ? line : line.substr(0, separator);
        std::string value = separator == std::string::npos ? std::string{} : line.substr(separator + 1);
        if (!value.empty() && value.front() == ' ') value.erase(0, 1);
        if (field == "event") event_ = value;
        if (field == "data") {
            if (!data_.empty()) data_.push_back('\n');
            data_ += value;
        }
    }

    std::string buffer_;
    std::string event_;
    std::string data_;
    EventCallback callback_;
};

std::string JoinUrl(std::string base, const std::string& suffix) {
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.size() >= suffix.size() && base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return base;
    }
    return base + suffix;
}

std::string ErrorFromResponse(const HttpResponse& response) {
    std::string body = Trim(response.body);
    if (body.size() > 4096) body.resize(4096);
    if (body.empty()) body = "(empty response body)";
    return "provider returned HTTP " + std::to_string(response.status) + ": " + body;
}

std::string ExtractErrorMessage(const HttpResponse& response) {
    try {
        const nlohmann::json json = nlohmann::json::parse(response.body);
        if (json.contains("error")) {
            const auto& error = json["error"];
            if (error.is_string()) return error.get<std::string>();
            if (error.is_object()) {
                const std::string message = error.value("message", std::string{});
                if (!message.empty()) return message;
            }
        }
    } catch (const nlohmann::json::exception&) {
    }
    return ErrorFromResponse(response);
}

std::string JsonErrorMessage(const nlohmann::json& value, const std::string& fallback) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_object()) {
        const std::string message = value.value("message", std::string{});
        if (!message.empty()) return message;
        if (value.contains("error")) return JsonErrorMessage(value["error"], fallback);
        const std::string detail = value.value("detail", std::string{});
        if (!detail.empty()) return detail;
    }
    const std::string dumped = value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    return dumped.empty() || dumped == "null" ? fallback : dumped;
}

std::string EventErrorMessage(const nlohmann::json& event, const std::string& fallback) {
    if (event.contains("error")) return JsonErrorMessage(event["error"], fallback);
    if (event.contains("response") && event["response"].is_object() &&
        event["response"].contains("error")) {
        return JsonErrorMessage(event["response"]["error"], fallback);
    }
    const std::string message = event.value("message", std::string{});
    return message.empty() ? fallback : message;
}

int UsageInteger(const nlohmann::json& object, const char* key) {
    if (!object.is_object()) return 0;
    const auto value = object.find(key);
    if (value == object.end() || !value->is_number()) return 0;
    return std::max(0, value->get<int>());
}

int UsageDetailInteger(const nlohmann::json& usage, const char* details_key, const char* token_key) {
    if (!usage.is_object()) return 0;
    const auto details = usage.find(details_key);
    return details == usage.end() ? 0 : UsageInteger(*details, token_key);
}

void AddUsage(const nlohmann::json& usage, ModelReply& reply) {
    if (!usage.is_object()) return;
    const int input_tokens = UsageInteger(usage, "prompt_tokens");
    const int response_input_tokens = UsageInteger(usage, "input_tokens");
    const int output_tokens = UsageInteger(usage, "completion_tokens");
    const int response_output_tokens = UsageInteger(usage, "output_tokens");
    if (input_tokens > 0 || response_input_tokens > 0) {
        reply.input_tokens = std::max(input_tokens, response_input_tokens);
    }
    if (output_tokens > 0 || response_output_tokens > 0) {
        reply.output_tokens = std::max(output_tokens, response_output_tokens);
    }

    int cached_tokens = std::max(UsageInteger(usage, "cache_read_input_tokens"),
                                 UsageInteger(usage, "prompt_cache_hit_tokens"));
    cached_tokens = std::max(cached_tokens, UsageDetailInteger(usage, "prompt_tokens_details", "cached_tokens"));
    cached_tokens = std::max(cached_tokens, UsageDetailInteger(usage, "input_tokens_details", "cached_tokens"));
    if (cached_tokens > 0) reply.cached_input_tokens = cached_tokens;

    int cache_write_tokens = UsageInteger(usage, "cache_creation_input_tokens");
    cache_write_tokens = std::max(cache_write_tokens,
                                  UsageDetailInteger(usage, "input_tokens_details", "cache_write_tokens"));
    if (cache_write_tokens > 0) reply.cache_write_input_tokens = cache_write_tokens;
}

std::string SourceString(const nlohmann::json& object, const char* key) {
    if (!object.is_object()) return {};
    const auto value = object.find(key);
    if (value == object.end() || !value->is_string()) return {};
    // Only copy a bounded prefix before trimming an incomplete UTF-8 sequence.
    return TruncateUtf8(value->get_ref<const std::string&>().substr(0, ModelReply::kMaxSourceStringBytes + 4),
                        ModelReply::kMaxSourceStringBytes);
}

void AddSource(SourceReference source, ModelReply& reply) {
    source.kind = TruncateUtf8(std::move(source.kind), ModelReply::kMaxSourceStringBytes);
    source.label = TruncateUtf8(std::move(source.label), ModelReply::kMaxSourceStringBytes);
    source.detail = TruncateUtf8(std::move(source.detail), ModelReply::kMaxSourceStringBytes);
    if (source.detail.empty()) return;
    for (auto& existing : reply.sources) {
        if (existing.kind != source.kind || existing.detail != source.detail) continue;
        const bool url_source = source.kind == "url_citation" || source.kind == "web_source";
        const bool partial_find = source.kind == "web_find" &&
            (source.label == "Find in page" || existing.label == "Find in page");
        if (existing.label != source.label && !url_source && !partial_find) continue;
        if ((url_source && existing.label == existing.detail && source.label != source.detail) ||
            (partial_find && existing.label == "Find in page")) {
            existing.label = std::move(source.label);
        }
        return;
    }
    if (reply.sources.size() < ModelReply::kMaxSources) reply.sources.push_back(std::move(source));
}

void CollectUrlSource(const nlohmann::json& value, const char* kind, ModelReply& reply) {
    const std::string url = SourceString(value, "url");
    std::string title = SourceString(value, "title");
    if (title.empty()) title = url;
    AddSource({kind, std::move(title), url}, reply);
}

void CollectCitation(const nlohmann::json& annotation, ModelReply& reply) {
    if (SourceString(annotation, "type") != "url_citation") return;
    const auto nested = annotation.find("url_citation");
    CollectUrlSource(nested != annotation.end() && nested->is_object() ? *nested : annotation,
                     "url_citation", reply);
}

void CollectMessageSources(const nlohmann::json& message, ModelReply& reply) {
    const auto collect_annotations = [&](const nlohmann::json& block) {
        if (!block.is_object()) return;
        const auto annotations = block.find("annotations");
        if (annotations == block.end() || !annotations->is_array()) return;
        for (const auto& annotation : *annotations) CollectCitation(annotation, reply);
    };
    collect_annotations(message);
    if (!message.is_object()) return;
    const auto content = message.find("content");
    if (content != message.end() && content->is_array()) {
        for (const auto& block : *content) collect_annotations(block);
    }
}

void CollectOutputSources(const nlohmann::json& item, ModelReply& reply) {
    const std::string type = SourceString(item, "type");
    if (type == "message") {
        CollectMessageSources(item, reply);
        return;
    }
    if (type != "web_search_call") return;
    const auto action = item.find("action");
    if (action == item.end() || !action->is_object()) return;
    const std::string action_type = SourceString(*action, "type");
    if (action_type == "search") {
        AddSource({"web_search", "Web search", SourceString(*action, "query")}, reply);
        const auto queries = action->find("queries");
        if (queries != action->end() && queries->is_array()) {
            for (const auto& query : *queries) {
                if (!query.is_string()) continue;
                AddSource({"web_search", "Web search",
                           TruncateUtf8(query.get_ref<const std::string&>().substr(0, ModelReply::kMaxSourceStringBytes + 4),
                                        ModelReply::kMaxSourceStringBytes)}, reply);
            }
        }
        const auto sources = action->find("sources");
        if (sources != action->end() && sources->is_array()) {
            for (const auto& source : *sources) {
                if (SourceString(source, "type") == "url") CollectUrlSource(source, "web_source", reply);
            }
        }
    } else if (action_type == "open_page") {
        AddSource({"web_open", "Open page", SourceString(*action, "url")}, reply);
    } else if (action_type == "find" || action_type == "find_in_page") {
        const std::string pattern = SourceString(*action, "pattern");
        AddSource({"web_find", pattern.empty() ? "Find in page" : "Find: " + pattern,
                   SourceString(*action, "url")}, reply);
    }
}

void ApplyProviderHeaders(HttpRequest& request, const ProviderConfig& provider) {
    for (const auto& [name, value] : provider.headers) request.headers[name] = value;
}

std::string ContentText(const nlohmann::json& content) {
    if (content.is_string()) return content.get<std::string>();
    if (!content.is_array()) return {};
    std::string output;
    for (const auto& block : content) {
        if (block.is_object()) {
            const std::string type = block.value("type", std::string{});
            if (type == "text" || type == "output_text") {
                output += block.value("text", std::string{});
            }
        }
    }
    return output;
}

std::string Base64Encode(std::string_view bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t index = 0; index < bytes.size(); index += 3) {
        const unsigned int first = static_cast<unsigned char>(bytes[index]);
        const unsigned int second = index + 1 < bytes.size() ? static_cast<unsigned char>(bytes[index + 1]) : 0;
        const unsigned int third = index + 2 < bytes.size() ? static_cast<unsigned char>(bytes[index + 2]) : 0;
        const unsigned int value = (first << 16) | (second << 8) | third;
        encoded.push_back(alphabet[(value >> 18) & 0x3f]);
        encoded.push_back(alphabet[(value >> 12) & 0x3f]);
        encoded.push_back(index + 1 < bytes.size() ? alphabet[(value >> 6) & 0x3f] : '=');
        encoded.push_back(index + 2 < bytes.size() ? alphabet[value & 0x3f] : '=');
    }
    return encoded;
}

std::string ImageMimeType(std::string_view path) {
    std::string extension = LowerAscii(PathToUtf8(NativePath(path).extension()));
    if (extension == ".jpg") extension = ".jpeg";
    if (extension == ".jpeg") return "image/jpeg";
    if (extension == ".png") return "image/png";
    if (extension == ".gif") return "image/gif";
    if (extension == ".webp") return "image/webp";
    if (extension == ".bmp") return "image/bmp";
    return "application/octet-stream";
}

std::optional<std::string> ImageDataUrl(std::string_view path) {
    const auto bytes = ReadAttachmentBytes(path);
    if (!bytes.has_value()) return std::nullopt;
    return "data:" + ImageMimeType(path) + ";base64," + Base64Encode(*bytes);
}

nlohmann::json OpenAiContent(const ChatMessage& message) {
    if (message.image_paths.empty()) return message.content;
    nlohmann::json content = nlohmann::json::array();
    if (!message.content.empty()) content.push_back({{"type", "text"}, {"text", message.content}});
    for (const std::string& path : message.image_paths) {
        const auto data_url = ImageDataUrl(path);
        if (data_url.has_value()) {
            content.push_back({{"type", "image_url"}, {"image_url", {{"url", *data_url}}}});
        } else {
            content.push_back({{"type", "text"}, {"text", "[image unavailable: " + path + "]"}});
        }
    }
    return content;
}

nlohmann::json AnthropicContent(const ChatMessage& message) {
    nlohmann::json content = nlohmann::json::array();
    if (!message.content.empty()) content.push_back({{"type", "text"}, {"text", message.content}});
    for (const std::string& path : message.image_paths) {
        const auto bytes = ReadAttachmentBytes(path);
        if (bytes.has_value()) {
            content.push_back({{"type", "image"},
                               {"source",
                                {{"type", "base64"},
                                 {"media_type", ImageMimeType(path)},
                                 {"data", Base64Encode(*bytes)}}}});
        } else {
            content.push_back({{"type", "text"}, {"text", "[image unavailable: " + path + "]"}});
        }
    }
    return content;
}

nlohmann::json ResponseImageContent(const ChatMessage& message) {
    nlohmann::json content = nlohmann::json::array();
    if (!message.content.empty()) content.push_back({{"type", "input_text"}, {"text", message.content}});
    for (const std::string& path : message.image_paths) {
        const auto data_url = ImageDataUrl(path);
        if (data_url.has_value()) {
            content.push_back({{"type", "input_image"}, {"image_url", *data_url}});
        } else {
            content.push_back({{"type", "input_text"}, {"text", "[image unavailable: " + path + "]"}});
        }
    }
    return content;
}

int AnthropicThinkingBudget(const std::string& effort) {
    if (effort == "low") return 2048;
    if (effort == "high") return 8192;
    if (effort == "xhigh") return 16384;
    if (effort == "ultra") return 32768;
    return 4096;
}

class OpenAiChatAdapter final : public ProviderAdapter {
public:
    Result<ModelReply> Complete(const ProviderRequest& request, const ProviderCallbacks& callbacks,
                                CancellationToken* cancellation) override {
        if (request.provider == nullptr || request.model == nullptr) {
            return Result<ModelReply>::Failure("provider and model are required");
        }
        nlohmann::json payload = {
            {"model", request.model->id},
            {"stream", request.model->capabilities.streaming},
            {"messages", nlohmann::json::array()},
        };
        if (!request.reasoning_effort.empty()) payload["reasoning_effort"] = request.reasoning_effort;
        for (const ChatMessage& message : request.messages) {
            nlohmann::json entry = {
                {"role", MessageRoleToString(message.role)},
            };
            if (!message.content.empty() || !message.image_paths.empty()) entry["content"] = OpenAiContent(message);
            if (!message.name.empty()) entry["name"] = message.name;
            if (!message.tool_call_id.empty()) entry["tool_call_id"] = message.tool_call_id;
            if (!message.tool_calls.empty()) {
                entry["tool_calls"] = nlohmann::json::array();
                for (const ToolCall& call : message.tool_calls) {
                    entry["tool_calls"].push_back({
                        {"id", call.id},
                        {"type", "function"},
                        {"function", {{"name", call.name}, {"arguments", call.arguments_json}}},
                    });
                }
            }
            if (message.role == MessageRole::Assistant && !message.reasoning_text.empty()) {
                // Thinking mode with tool calls rejects the follow-up request unless the
                // previous reasoning is echoed back with the assistant message.
                entry["reasoning_content"] = message.reasoning_text;
            }
            payload["messages"].push_back(std::move(entry));
            if (message.role == MessageRole::Tool && !message.image_paths.empty()) {
                ChatMessage image_message;
                image_message.role = MessageRole::User;
                image_message.content = "请查看工具返回的图像，并结合图像内容继续处理。";
                image_message.image_paths = message.image_paths;
                payload["messages"].push_back({
                    {"role", "user"},
                    {"content", OpenAiContent(image_message)},
                });
            }
        }
        if (request.model->capabilities.tool_calls && !request.tools.empty()) {
            payload["tools"] = nlohmann::json::array();
            for (const ToolDefinition& tool : request.tools) {
                payload["tools"].push_back({
                    {"type", "function"},
                    {"function", {
                        {"name", tool.name},
                        {"description", tool.description},
                        {"parameters", tool.parameters},
                    }},
                });
            }
            payload["tool_choice"] = "auto";
        }
        HttpRequest http;
        http.url = JoinUrl(request.provider->base_url, "/chat/completions");
        http.headers["Authorization"] = "Bearer " + request.provider->api_key;
        http.headers["Content-Type"] = "application/json";
        http.headers["Accept"] = request.model->capabilities.streaming ? "text/event-stream" : "application/json";
        ApplyProviderHeaders(http, *request.provider);
        http.body = payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        http.timeout_seconds = request.request_timeout_seconds;
        http.header_timeout_seconds = request.response_header_timeout_seconds;
        http.stream = request.model->capabilities.streaming;
        http.on_status = callbacks.on_http_status;

        ModelReply reply;
        std::map<int, ToolCall> calls;
        SseDecoder decoder;
        bool stream_complete = false;
        std::string stream_error;
        // Thinking mode with tool calls requires the previous reasoning to be echoed
        // back, so accumulate whatever the provider streams and keep it on the reply.
        std::string reasoning_accum;
        const auto chunk_callback = [&](const char* data, size_t size) {
            if (!http.stream) return StreamAction::Continue;
            decoder.Feed(data, size, [&](const std::string&, const std::string& event_data) {
                if (event_data == "[DONE]") {
                    stream_complete = true;
                    return;
                }
                try {
                    const nlohmann::json event = nlohmann::json::parse(event_data);
                    if (event.contains("error")) {
                        stream_error = EventErrorMessage(event, "provider returned an error");
                        reply.finish_reason = "error";
                        stream_complete = true;
                        return;
                    }
                    if (event.contains("usage")) AddUsage(event["usage"], reply);
                    if (event.contains("id") && reply.provider_request_id.empty()) reply.provider_request_id = event.value("id", "");
                    const auto choices = event.find("choices");
                    if (choices == event.end() || !choices->is_array() || choices->empty()) return;
                    const auto& choice = choices->front();
                    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                        reply.finish_reason = choice["finish_reason"].get<std::string>();
                        if (reply.finish_reason == "error") {
                            stream_error = "provider returned an error finish reason";
                        }
                    }
                    const auto delta = choice.find("delta");
                    if (delta == choice.end() || !delta->is_object()) return;
                    CollectMessageSources(*delta, reply);
                    if (delta->contains("content") && !(*delta)["content"].is_null()) {
                        const std::string text = ContentText((*delta)["content"]);
                        if (!text.empty()) {
                            reply.text += text;
                            if (callbacks.on_text_delta) callbacks.on_text_delta(text);
                        }
                    }
                    const auto reasoning = delta->find("reasoning_content");
                    if (reasoning != delta->end() && reasoning->is_string()) {
                        const std::string text = reasoning->get<std::string>();
                        reasoning_accum += text;
                        if (callbacks.on_reasoning_delta && !text.empty()) callbacks.on_reasoning_delta(text);
                    } else {
                        const auto reasoning_text = delta->find("reasoning");
                        if (reasoning_text != delta->end() && reasoning_text->is_string()) {
                            const std::string text = reasoning_text->get<std::string>();
                            reasoning_accum += text;
                            if (callbacks.on_reasoning_delta && !text.empty()) callbacks.on_reasoning_delta(text);
                        }
                    }
                    const auto tool_calls = delta->find("tool_calls");
                    if (tool_calls != delta->end() && tool_calls->is_array()) {
                        for (const auto& item : *tool_calls) {
                            const int index = item.value("index", 0);
                            ToolCall& call = calls[index];
                            call.id += item.value("id", std::string{});
                            const auto function = item.find("function");
                            if (function != item.end() && function->is_object()) {
                                call.name += function->value("name", std::string{});
                                call.arguments_json += function->value("arguments", std::string{});
                            }
                        }
                    }
                } catch (const nlohmann::json::exception&) {
                    reply.finish_reason = "malformed_sse";
                }
            });
            if (cancellation != nullptr && cancellation->IsCancelled()) return StreamAction::Cancel;
            return stream_complete ? StreamAction::Stop : StreamAction::Continue;
        };

        const auto response = HttpRequestStreaming(http, chunk_callback, cancellation);
        if (!response.ok) return Result<ModelReply>::Failure(response.error);
        if (response.value.status < 200 || response.value.status >= 300) {
            return Result<ModelReply>::Failure(ExtractErrorMessage(response.value));
        }
        if (!http.stream) {
            try {
                const nlohmann::json json = nlohmann::json::parse(response.value.body);
                if (json.contains("error")) {
                    return Result<ModelReply>::Failure(EventErrorMessage(json, "provider returned an error"));
                }
                if (json.contains("id")) reply.provider_request_id = json.value("id", "");
                if (json.contains("usage")) AddUsage(json["usage"], reply);
                if (json.contains("choices") && json["choices"].is_array() && !json["choices"].empty()) {
                    const auto& choice = json["choices"].front();
                    reply.finish_reason = choice.value("finish_reason", std::string{});
                    if (choice.contains("message")) {
                        const auto& message = choice["message"];
                        CollectMessageSources(message, reply);
                        reply.text = ContentText(message.value("content", nlohmann::json::string_t{}));
                        const auto message_reasoning = message.find("reasoning_content");
                        if (message_reasoning != message.end() && message_reasoning->is_string()) {
                            reasoning_accum += message_reasoning->get<std::string>();
                        }
                        if (callbacks.on_text_delta && !reply.text.empty()) callbacks.on_text_delta(reply.text);
                        if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
                            for (const auto& item : message["tool_calls"]) {
                                ToolCall call;
                                call.id = item.value("id", std::string{});
                                call.name = item.value("function", nlohmann::json::object()).value("name", std::string{});
                                call.arguments_json = item.value("function", nlohmann::json::object()).value("arguments", std::string{});
                                reply.tool_calls.push_back(std::move(call));
                            }
                        }
                    }
                }
            } catch (const nlohmann::json::exception& exception) {
                return Result<ModelReply>::Failure(std::string("invalid provider JSON: ") + exception.what());
            }
        }
        if (http.stream) decoder.Finish();
        reply.reasoning_text = reasoning_accum;
        if (!stream_error.empty()) return Result<ModelReply>::Failure(stream_error);
        if (reply.finish_reason == "error") {
            return Result<ModelReply>::Failure("provider returned an error");
        }
        if (reply.finish_reason == "malformed_sse") {
            return Result<ModelReply>::Failure("provider sent malformed SSE data");
        }
        for (auto& [index, call] : calls) {
            (void)index;
            if (call.name.empty()) continue;
            if (call.id.empty()) call.id = "call-" + std::to_string(reply.tool_calls.size());
            if (call.arguments_json.empty()) call.arguments_json = "{}";
            reply.tool_calls.push_back(std::move(call));
        }
        return Result<ModelReply>::Success(std::move(reply));
    }
};

class AnthropicMessagesAdapter final : public ProviderAdapter {
public:
    Result<ModelReply> Complete(const ProviderRequest& request, const ProviderCallbacks& callbacks,
                                CancellationToken* cancellation) override {
        if (request.provider == nullptr || request.model == nullptr) {
            return Result<ModelReply>::Failure("provider and model are required");
        }
        const bool use_thinking = request.model->capabilities.reasoning && !request.reasoning_effort.empty();
        const int thinking_budget = use_thinking ? AnthropicThinkingBudget(request.reasoning_effort) : 0;
        nlohmann::json payload = {
            {"model", request.model->id},
            {"max_tokens", use_thinking ? thinking_budget + 4096 : 8192},
            {"stream", request.model->capabilities.streaming},
            {"messages", nlohmann::json::array()},
        };
        if (use_thinking) payload["thinking"] = {{"type", "enabled"}, {"budget_tokens", thinking_budget}};
        std::string system_prompt;
        for (const ChatMessage& message : request.messages) {
            if (message.role == MessageRole::System) {
                if (!system_prompt.empty()) system_prompt += "\n\n";
                system_prompt += message.content;
                continue;
            }
            if (message.role == MessageRole::Tool) {
                nlohmann::json tool_result = {
                    {"type", "tool_result"},
                    {"tool_use_id", message.tool_call_id},
                    {"content", message.image_paths.empty() ? nlohmann::json(message.content)
                                                             : AnthropicContent(message)},
                };
                if (!payload["messages"].empty() && payload["messages"].back().value("role", std::string{}) == "user" &&
                    payload["messages"].back().contains("content") && payload["messages"].back()["content"].is_array() &&
                    !payload["messages"].back()["content"].empty() &&
                    payload["messages"].back()["content"].back().value("type", std::string{}) == "tool_result") {
                    payload["messages"].back()["content"].push_back(std::move(tool_result));
                } else {
                    payload["messages"].push_back({{"role", "user"}, {"content", nlohmann::json::array({tool_result})}});
                }
                continue;
            }
            const std::string role = message.role == MessageRole::Assistant ? "assistant" : "user";
            nlohmann::json content = AnthropicContent(message);
            for (const ToolCall& call : message.tool_calls) {
                nlohmann::json input = nlohmann::json::object();
                try {
                    input = nlohmann::json::parse(call.arguments_json);
                } catch (const nlohmann::json::exception&) {
                }
                content.push_back({{"type", "tool_use"}, {"id", call.id}, {"name", call.name}, {"input", input}});
            }
            payload["messages"].push_back({{"role", role}, {"content", std::move(content)}});
        }
        if (!system_prompt.empty()) payload["system"] = system_prompt;
        if (request.model->capabilities.tool_calls && !request.tools.empty()) {
            payload["tools"] = nlohmann::json::array();
            for (const ToolDefinition& tool : request.tools) {
                payload["tools"].push_back({{"name", tool.name}, {"description", tool.description}, {"input_schema", tool.parameters}});
            }
        }

        HttpRequest http;
        http.url = JoinUrl(request.provider->base_url, "/messages");
        http.headers["x-api-key"] = request.provider->api_key;
        http.headers["anthropic-version"] = "2023-06-01";
        http.headers["Content-Type"] = "application/json";
        http.headers["Accept"] = request.model->capabilities.streaming ? "text/event-stream" : "application/json";
        ApplyProviderHeaders(http, *request.provider);
        http.body = payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        http.timeout_seconds = request.request_timeout_seconds;
        http.header_timeout_seconds = request.response_header_timeout_seconds;
        http.stream = request.model->capabilities.streaming;
        http.on_status = callbacks.on_http_status;

        ModelReply reply;
        std::map<int, ToolCall> tool_calls;
        std::map<int, std::string> tool_arguments;
        SseDecoder decoder;
        bool stream_complete = false;
        std::string stream_error;
        const auto chunk_callback = [&](const char* data, size_t size) {
            if (!http.stream) return StreamAction::Continue;
            decoder.Feed(data, size, [&](const std::string& event_name, const std::string& event_data) {
                try {
                    const nlohmann::json event = nlohmann::json::parse(event_data);
                    if (event_name == "error" || event.value("type", std::string{}) == "error") {
                        stream_error = EventErrorMessage(event, "Anthropic returned an error");
                        reply.finish_reason = "error";
                        stream_complete = true;
                        return;
                    }
                    const std::string type = event.value("type", event_name);
                    if (type == "message_stop") {
                        stream_complete = true;
                        return;
                    }
                    if (type == "message_start" && event.contains("message")) {
                        reply.provider_request_id = event["message"].value("id", std::string{});
                        if (event["message"].contains("usage")) AddUsage(event["message"]["usage"], reply);
                    } else if (type == "content_block_start") {
                        const auto block = event.value("content_block", nlohmann::json::object());
                        if (block.value("type", std::string{}) == "tool_use") {
                            const int index = event.value("index", 0);
                            tool_calls[index].id = block.value("id", std::string{});
                            tool_calls[index].name = block.value("name", std::string{});
                            tool_arguments[index] = "{}";
                        }
                    } else if (type == "content_block_delta") {
                        const auto delta = event.value("delta", nlohmann::json::object());
                        const std::string delta_type = delta.value("type", std::string{});
                        if (delta_type == "text_delta") {
                            const std::string text = delta.value("text", std::string{});
                            reply.text += text;
                            if (callbacks.on_text_delta && !text.empty()) callbacks.on_text_delta(text);
                        } else if (delta_type == "thinking_delta") {
                            const std::string text = delta.value("thinking", std::string{});
                            if (callbacks.on_reasoning_delta && !text.empty()) callbacks.on_reasoning_delta(text);
                        } else if (delta_type == "input_json_delta") {
                            tool_arguments[event.value("index", 0)] += delta.value("partial_json", std::string{});
                        }
                    } else if (type == "message_delta") {
                        reply.finish_reason = event.value("delta", nlohmann::json::object()).value("stop_reason", reply.finish_reason);
                        if (event.contains("usage")) AddUsage(event["usage"], reply);
                    }
                } catch (const nlohmann::json::exception&) {
                    reply.finish_reason = "malformed_sse";
                }
            });
            if (cancellation != nullptr && cancellation->IsCancelled()) return StreamAction::Cancel;
            return stream_complete ? StreamAction::Stop : StreamAction::Continue;
        };
        const auto response = HttpRequestStreaming(http, chunk_callback, cancellation);
        if (!response.ok) return Result<ModelReply>::Failure(response.error);
        if (response.value.status < 200 || response.value.status >= 300) {
            return Result<ModelReply>::Failure(ExtractErrorMessage(response.value));
        }
        if (!http.stream) {
            try {
                const nlohmann::json json = nlohmann::json::parse(response.value.body);
                if (json.contains("error")) {
                    return Result<ModelReply>::Failure(EventErrorMessage(json, "Anthropic returned an error"));
                }
                reply.provider_request_id = json.value("id", std::string{});
                reply.finish_reason = json.value("stop_reason", std::string{});
                AddUsage(json.value("usage", nlohmann::json::object()), reply);
                if (json.contains("content") && json["content"].is_array()) {
                    for (const auto& block : json["content"]) {
                        if (block.value("type", std::string{}) == "text") {
                            reply.text += block.value("text", std::string{});
                        } else if (block.value("type", std::string{}) == "tool_use") {
                            ToolCall call;
                            call.id = block.value("id", std::string{});
                            call.name = block.value("name", std::string{});
                            call.arguments_json = block.value("input", nlohmann::json::object())
                                                      .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
                            reply.tool_calls.push_back(std::move(call));
                        }
                    }
                }
                if (callbacks.on_text_delta && !reply.text.empty()) callbacks.on_text_delta(reply.text);
            } catch (const nlohmann::json::exception& exception) {
                return Result<ModelReply>::Failure(std::string("invalid provider JSON: ") + exception.what());
            }
        }
        if (http.stream) decoder.Finish();
        if (!stream_error.empty()) return Result<ModelReply>::Failure(stream_error);
        if (reply.finish_reason == "error") {
            return Result<ModelReply>::Failure("Anthropic returned an error");
        }
        if (reply.finish_reason == "malformed_sse") {
            return Result<ModelReply>::Failure("provider sent malformed SSE data");
        }
        for (auto& [index, call] : tool_calls) {
            call.arguments_json = tool_arguments[index].empty() ? "{}" : tool_arguments[index];
            reply.tool_calls.push_back(std::move(call));
        }
        return Result<ModelReply>::Success(std::move(reply));
    }
};

class OpenAiResponsesAdapter final : public ProviderAdapter {
public:
    Result<ModelReply> Complete(const ProviderRequest& request, const ProviderCallbacks& callbacks,
                                CancellationToken* cancellation) override {
        if (request.provider == nullptr || request.model == nullptr) {
            return Result<ModelReply>::Failure("provider and model are required");
        }
        nlohmann::json payload = {
            {"model", request.model->id},
            {"input", nlohmann::json::array()},
            {"stream", request.model->capabilities.streaming},
        };
        if (!request.reasoning_effort.empty()) payload["reasoning"] = {{"effort", request.reasoning_effort}};
        if (!request.prompt_cache_key.empty()) payload["prompt_cache_key"] = request.prompt_cache_key;
        std::string instructions;
        // Thinking models pair every assistant turn with the reasoning that produced it and
        // reject the request when the pair is broken ("The reasoning_text in the thinking mode
        // must be passed back to the API"). Echo reasoning back for every assistant turn that
        // carries it instead of only for the newest ones; Codex replays its full history the
        // same way, and compaction is what keeps the request from growing without bound.
        size_t replayed_reasoning = 0;
        size_t replayed_reasoning_bytes = 0;
        for (size_t message_index = 0; message_index < request.messages.size(); ++message_index) {
            const ChatMessage& message = request.messages[message_index];
            if (message.role == MessageRole::System) {
                if (!instructions.empty()) instructions += "\n\n";
                instructions += message.content;
            } else if (message.role == MessageRole::Tool) {
                payload["input"].push_back({{"type", "function_call_output"}, {"call_id", message.tool_call_id}, {"output", message.content}});
                if (!message.image_paths.empty()) {
                    payload["input"].push_back({{"type", "message"},
                                                {"role", "user"},
                                                {"content", ResponseImageContent(message)}});
                }
            } else if (message.role == MessageRole::Assistant) {
                const bool replay_reasoning = !message.reasoning_items.empty() || !message.reasoning_text.empty();
                if (replay_reasoning) {
                    for (const std::string& item : message.reasoning_items) {
                        try {
                            payload["input"].push_back(
                                nlohmann::json::parse(item, nullptr, true, true));
                            ++replayed_reasoning;
                            replayed_reasoning_bytes += item.size();
                        } catch (const nlohmann::json::exception&) {
                        }
                    }
                    if (message.reasoning_items.empty() && !message.reasoning_text.empty()) {
                        payload["input"].push_back({
                            {"type", "reasoning"},
                            {"content", nlohmann::json::array(
                                            {{{"type", "reasoning_text"}, {"text", message.reasoning_text}}})},
                        });
                        ++replayed_reasoning;
                        replayed_reasoning_bytes += message.reasoning_text.size();
                    }
                }
                if (!message.content.empty()) {
                    payload["input"].push_back({
                        {"type", "message"},
                        {"role", "assistant"},
                        {"content", nlohmann::json::array({{{"type", "output_text"}, {"text", message.content}}})},
                    });
                }
                for (const ToolCall& call : message.tool_calls) {
                    payload["input"].push_back({
                        {"type", "function_call"},
                        {"call_id", call.id},
                        {"name", call.name},
                        {"arguments", call.arguments_json},
                    });
                }
            } else {
                std::string content = message.content;
                if (!message.name.empty()) content = message.name + ": " + content;
                payload["input"].push_back({
                    {"type", "message"},
                    {"role", "user"},
                    {"content", message.image_paths.empty()
                                     ? nlohmann::json::array({{{"type", "input_text"}, {"text", content}}})
                                     : ResponseImageContent(ChatMessage{message.role, content, message.image_paths, {}, {}, {}})},
                });
            }
        }
        if (!instructions.empty()) payload["instructions"] = instructions;
        if (request.model->capabilities.tool_calls && !request.tools.empty()) {
            payload["tools"] = nlohmann::json::array();
            for (const ToolDefinition& tool : request.tools) {
                payload["tools"].push_back({
                    {"type", "function"},
                    {"name", tool.name},
                    {"description", tool.description},
                    {"parameters", tool.parameters},
                });
            }
        }
        HttpRequest http;
        http.url = JoinUrl(request.provider->base_url, "/responses");
        http.headers["Authorization"] = "Bearer " + request.provider->api_key;
        http.headers["Content-Type"] = "application/json";
        http.headers["Accept"] = request.model->capabilities.streaming ? "text/event-stream" : "application/json";
        ApplyProviderHeaders(http, *request.provider);
        http.body = payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        http.timeout_seconds = request.request_timeout_seconds;
        http.header_timeout_seconds = request.response_header_timeout_seconds;
        http.stream = request.model->capabilities.streaming;
        http.on_status = callbacks.on_http_status;
        ModelReply reply;
        std::map<std::string, ToolCall> calls_by_id;
        std::map<std::string, std::string> call_id_by_item_id;
        SseDecoder decoder;
        bool stream_complete = false;
        std::string stream_error;
        // Diagnostics for the silent-failure case: a 2xx answer that carries no
        // usable payload used to end the turn with an empty assistant message and
        // no explanation at all.
        size_t received_bytes = 0;
        size_t received_events = 0;
        size_t reasoning_events = 0;
        std::map<std::string, int> event_type_counts;
        std::string reasoning_buffer;
        std::string reasoning_done;
        std::vector<std::string> reasoning_items;
        std::string raw_prefix;
        const auto absorb_output_item = [&](const nlohmann::json& item, bool final) {
            if (!item.is_object()) return;
            CollectOutputSources(item, reply);
            const std::string item_type = item.value("type", std::string{});
            if (item_type == "reasoning") {
                // Thinking models reject the follow-up request unless the reasoning item
                // of the previous turn is handed back verbatim, so keep the finished item
                // as the provider sent it (id / summary / content / encrypted_content).
                if (final) reasoning_items.push_back(item.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
            } else if (item_type == "function_call") {
                const std::string item_id = item.value("id", std::string{});
                const std::string id = item.value("call_id", item_id);
                calls_by_id[id].id = id;
                calls_by_id[id].name = item.value("name", std::string{});
                // Argument deltas can be dropped or interleaved between parallel tool calls,
                // so a finished item always wins over the accumulated value: a truncated
                // string makes the tool fail with "invalid tool arguments" later on.
                const std::string item_arguments = item.value("arguments", std::string{});
                if (!item_arguments.empty()) calls_by_id[id].arguments_json = item_arguments;
                if (!item_id.empty()) call_id_by_item_id[item_id] = id;
            } else if (item_type == "message" && reply.text.empty()) {
                // Gateways that do not stream text deltas only publish the finished
                // text together with the output item.
                const std::string text = ContentText(item.value("content", nlohmann::json::array()));
                if (!text.empty()) {
                    reply.text = text;
                    if (callbacks.on_text_delta) callbacks.on_text_delta(text);
                }
            }
        };
        const auto chunk_callback = [&](const char* data, size_t size) {
            if (!http.stream) return StreamAction::Continue;
            received_bytes += size;
            if (raw_prefix.size() < 512) {
                raw_prefix.append(data, std::min<size_t>(size, 512 - raw_prefix.size()));
            }
            decoder.Feed(data, size, [&](const std::string& event_name, const std::string& event_data) {
                try {
                    const nlohmann::json event = nlohmann::json::parse(event_data);
                    const std::string type = event.value("type", event_name);
                    ++received_events;
                    if (event_type_counts.size() < 12 || event_type_counts.count(type) != 0) ++event_type_counts[type];
                    if (type == "response.output_text.delta") {
                        const std::string text = event.value("delta", std::string{});
                        reply.text += text;
                        if (callbacks.on_text_delta && !text.empty()) callbacks.on_text_delta(text);
                    } else if (type == "response.output_text.done") {
                        if (reply.text.empty()) {
                            const std::string text = event.value("text", std::string{});
                            if (!text.empty()) {
                                reply.text = text;
                                if (callbacks.on_text_delta) callbacks.on_text_delta(text);
                            }
                        }
                    } else if (type == "response.reasoning_summary_text.delta" ||
                               type == "response.reasoning_text.delta") {
                        ++reasoning_events;
                        const std::string text = event.value("delta", std::string{});
                        // Keep the raw reasoning even when nobody is watching the UI: the
                        // provider wants it back on the next request.
                        if (type == "response.reasoning_text.delta") reasoning_buffer += text;
                        if (callbacks.on_reasoning_delta && !text.empty()) callbacks.on_reasoning_delta(text);
                    } else if (type == "response.reasoning_text.done") {
                        reasoning_done = event.value("text", std::string{});
                    } else if (type == "response.output_item.added" || type == "response.output_item.done") {
                        absorb_output_item(event.value("item", nlohmann::json::object()),
                                           type == "response.output_item.done");
                    } else if (type == "response.output_text.annotation.added") {
                        const auto annotation = event.find("annotation");
                        if (annotation != event.end()) CollectCitation(*annotation, reply);
                    } else if (type == "response.function_call_arguments.delta") {
                        const std::string item_id = event.value("item_id", std::string{});
                        const auto mapped = call_id_by_item_id.find(item_id);
                        const std::string id = mapped == call_id_by_item_id.end()
                                                   ? event.value("call_id", item_id)
                                                   : mapped->second;
                        calls_by_id[id].id = id;
                        calls_by_id[id].arguments_json += event.value("delta", std::string{});
                    } else if (type == "response.function_call_arguments.done") {
                        const std::string item_id = event.value("item_id", std::string{});
                        const auto mapped = call_id_by_item_id.find(item_id);
                        const std::string id = mapped == call_id_by_item_id.end()
                                                   ? event.value("call_id", item_id)
                                                   : mapped->second;
                        calls_by_id[id].id = id;
                        if (event.contains("arguments") && event["arguments"].is_string()) {
                            calls_by_id[id].arguments_json = event["arguments"].get<std::string>();
                        }
                    } else if (type == "response.completed") {
                        const auto response = event.value("response", nlohmann::json::object());
                        reply.finish_reason = response.value("status", std::string("completed"));
                        if (reply.finish_reason == "failed" || reply.finish_reason == "incomplete") {
                            stream_error = EventErrorMessage(event, "Responses API returned " + reply.finish_reason);
                        }
                        reply.provider_request_id = response.value("id", std::string{});
                        AddUsage(response.value("usage", nlohmann::json::object()), reply);
                        // Some gateways only publish the finished output here.
                        if (response.contains("output") && response["output"].is_array()) {
                            for (const auto& item : response["output"]) absorb_output_item(item, true);
                        }
                        stream_complete = true;
                    } else if (type == "error" || type == "response.failed" || type == "response.incomplete") {
                        stream_error = EventErrorMessage(event, "Responses API returned " + type);
                        reply.finish_reason = "error";
                        stream_complete = true;
                    }
                } catch (const nlohmann::json::exception&) {
                    reply.finish_reason = "malformed_sse";
                }
            });
            if (cancellation != nullptr && cancellation->IsCancelled()) return StreamAction::Cancel;
            return stream_complete ? StreamAction::Stop : StreamAction::Continue;
        };
        const auto response = HttpRequestStreaming(http, chunk_callback, cancellation);
        std::string content_type;
        std::string request_id;
        if (response.ok) {
            for (const auto& [name, value] : response.value.headers) {
                const std::string header = LowerAscii(Trim(name));
                if (header == "content-type") content_type = value;
                if (header == "x-request-id" || header == "request-id") request_id = value.substr(0, 200);
            }
        }
        std::string type_summary;
        for (const auto& [name, count] : event_type_counts) {
            if (!type_summary.empty()) type_summary += ", ";
            type_summary += name + "=" + std::to_string(count);
        }
        // One readable line describing what the provider actually did, attached to
        // every failure and journalled on success so a stalled turn can be
        // explained afterwards without guessing or screenshots.
        const auto describe_http = [&]() {
            std::string info = "POST " + http.url + " | 请求体 " + std::to_string(http.body.size()) + " 字节";
            if (!response.ok) return info + " | 传输失败";
            info += " | HTTP " + std::to_string(response.value.status);
            if (!content_type.empty()) info += " " + content_type;
            info += " | 等待响应头 " + std::to_string(response.value.header_ms) + "ms";
            info += " | 总耗时 " + std::to_string(response.value.total_ms) + "ms";
            info += " | 收到 " + std::to_string(received_bytes) + " 字节 / " + std::to_string(received_events) + " 个事件";
            if (!type_summary.empty()) info += " [" + type_summary + "]";
            info += " | 回传推理 " + std::to_string(replayed_reasoning) + " 条 / " +
                    std::to_string(replayed_reasoning_bytes) + " 字节";
            if (!request_id.empty()) info += " | 请求编号 " + SanitizeDisplayText(request_id);
            return info;
        };
        if (!response.ok) return Result<ModelReply>::Failure(response.error + " | " + describe_http());
        if (response.value.status < 200 || response.value.status >= 300) {
            return Result<ModelReply>::Failure(ExtractErrorMessage(response.value) + " | " + describe_http());
        }
        if (!http.stream) {
            try {
                const nlohmann::json json = nlohmann::json::parse(response.value.body);
                if (json.contains("error")) {
                    return Result<ModelReply>::Failure(EventErrorMessage(json, "Responses API returned an error"));
                }
                reply.provider_request_id = json.value("id", std::string{});
                reply.finish_reason = json.value("status", std::string{});
                if (reply.finish_reason == "failed" || reply.finish_reason == "incomplete") {
                    return Result<ModelReply>::Failure(EventErrorMessage(json, "Responses API returned " + reply.finish_reason));
                }
                AddUsage(json.value("usage", nlohmann::json::object()), reply);
                if (json.contains("output") && json["output"].is_array()) {
                    for (const auto& item : json["output"]) {
                        CollectOutputSources(item, reply);
                        const std::string type = item.value("type", std::string{});
                        if (type == "message") {
                            reply.text += ContentText(item.value("content", nlohmann::json::array()));
                        } else if (type == "function_call") {
                            ToolCall call;
                            call.id = item.value("call_id", item.value("id", std::string{}));
                            call.name = item.value("name", std::string{});
                            call.arguments_json = item.value("arguments", std::string("{}"));
                            reply.tool_calls.push_back(std::move(call));
                        }
                    }
                }
                if (callbacks.on_text_delta && !reply.text.empty()) callbacks.on_text_delta(reply.text);
            } catch (const nlohmann::json::exception& exception) {
                return Result<ModelReply>::Failure(std::string("invalid provider JSON: ") + exception.what());
            }
        }
        if (http.stream) decoder.Finish();
        // The finished reasoning belongs to the assistant turn: hand it back on the next
        // request so thinking models do not reject the tool loop with HTTP 400.
        reply.reasoning_text = reasoning_done.empty() ? reasoning_buffer : reasoning_done;
        reply.reasoning_items = reasoning_items;
        if (!stream_error.empty()) return Result<ModelReply>::Failure(stream_error);
        if (reply.finish_reason == "error") {
            return Result<ModelReply>::Failure("Responses API returned an error");
        }
        if (reply.finish_reason == "malformed_sse") return Result<ModelReply>::Failure("provider sent malformed SSE data");
        for (auto& [id, call] : calls_by_id) {
            (void)id;
            if (call.name.empty()) continue;
            if (call.arguments_json.empty()) call.arguments_json = "{}";
            reply.tool_calls.push_back(std::move(call));
        }
        if (reply.text.empty() && reply.tool_calls.empty()) {
            // Never report an empty turn as success: the UI would end the turn with an
            // empty assistant bubble and no hint about what the provider actually did.
            std::string detail;
            if (http.stream && received_bytes == 0) {
                detail = "API 已连接，但供应商返回了空响应体（0 字节）：没有收到模型输出。"
                         "请检查上游接口或模型服务状态。";
            } else if (http.stream && received_events == 0) {
                detail = "供应商没有返回可解析的流式内容（收到 " + std::to_string(received_bytes) +
                         " 字节，未识别到任何 SSE 事件）。";
                std::string snippet;
                for (const char character : raw_prefix) {
                    if (static_cast<unsigned char>(character) < 0x20) continue;
                    snippet.push_back(character);
                    if (snippet.size() >= 200) break;
                }
                if (!snippet.empty()) detail += " 响应开头：" + snippet;
            } else if (http.stream && !stream_complete) {
                detail = "流式响应意外中断（收到 " + std::to_string(received_events) + " 个事件";
                if (reasoning_events > 0) detail += "，其中推理片段 " + std::to_string(reasoning_events) + " 个";
                detail += "）：供应商没有发送完成事件，也没有给出任何文本或工具调用。";
            } else {
                detail = "供应商返回了空响应（status=" +
                         (reply.finish_reason.empty() ? std::string("unknown") : reply.finish_reason) +
                         "）：没有任何文本或工具调用。";
            }
            return Result<ModelReply>::Failure(detail + " | " + describe_http());
        }
        reply.diagnostics = describe_http();
        return Result<ModelReply>::Success(std::move(reply));
    }
};

}  // namespace

std::unique_ptr<ProviderAdapter> CreateProviderAdapter(const std::string& protocol) {
    const std::string normalized = LowerAscii(Trim(protocol));
    if (normalized == "chat_completions" || normalized == "openai_chat_completions") return std::make_unique<OpenAiChatAdapter>();
    if (normalized == "anthropic_messages") return std::make_unique<AnthropicMessagesAdapter>();
    if (normalized == "responses" || normalized == "openai_responses") return std::make_unique<OpenAiResponsesAdapter>();
    return nullptr;
}

std::string ProviderEndpointFor(const ProviderConfig& provider) {
    const std::string protocol = LowerAscii(Trim(provider.protocol));
    if (protocol == "anthropic_messages") return JoinUrl(provider.base_url, "/messages");
    if (protocol == "responses" || protocol == "openai_responses") return JoinUrl(provider.base_url, "/responses");
    return JoinUrl(provider.base_url, "/chat/completions");
}

std::string ProviderConnectionCheckUrl(const ProviderConfig& provider) {
    return JoinUrl(provider.base_url, "/models");
}

}  // namespace spark
