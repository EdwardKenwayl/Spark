#include "loopback_http.h"
#include "core/common.h"
#include "core/context_compactor.h"
#include "core/provider.h"

#include <fstream>
#include <iostream>

namespace {
using namespace spark;
using Server = spark::test::LoopbackHttpServer;
using Json = nlohmann::json;
int failures = 0;
void Check(bool value, const char* label) {
    if (!value) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
size_t Images(const Json& node) {
    size_t count = 0;
    if (node.is_object()) {
        const std::string type = node.value("type", std::string{});
        if (type == "image_url" || type == "input_image" || type == "image") ++count;
    }
    if (node.is_structured()) for (const auto& child : node) count += Images(child);
    return count;
}
void RoundTrip(const std::string& protocol, const std::string& image) {
    const std::string body = protocol == "responses"
        ? R"({"status":"completed","output":[{"type":"message","content":[{"type":"output_text","text":"ok"}]}]})"
        : protocol == "anthropic_messages"
        ? R"({"content":[{"type":"text","text":"ok"}],"stop_reason":"end_turn"})"
        : R"({"choices":[{"message":{"content":"ok"},"finish_reason":"stop"}]})";
    Server server([&](Server::Connection& connection, size_t) {
        connection.Send(Server::Headers(200, body.size(), "Content-Type: application/json\r\n") + body);
    }, 2, 72 * 1024 * 1024);
    ProviderConfig provider;
    provider.protocol = protocol; provider.base_url = server.Url("/v1"); provider.api_key = "fixture";
    ModelConfig model; model.id = "image-model"; model.capabilities.streaming = false;
    ProviderRequest request;
    request.provider = &provider; request.model = &model; request.request_timeout_seconds = 10;
    ChatMessage user; user.role = MessageRole::User; user.content = "Inspect the tool images.";
    ChatMessage assistant; assistant.role = MessageRole::Assistant;
    assistant.tool_calls.push_back({"call-image", "view_image", "{}"});
    ChatMessage tool; tool.role = MessageRole::Tool; tool.name = "view_image";
    tool.tool_call_id = "call-image"; tool.content = "Four screenshots.";
    tool.image_paths = {image, image, image, image};
    request.messages = {user, assistant, tool};
    const auto result = CreateProviderAdapter(protocol)->Complete(request, {}, nullptr);
    const auto requests = server.Requests();
    Check(result.ok && result.value.text == "ok", "image request returns a reply");
    Check(requests.size() == 1 && server.Error().empty(), "one actual request reaches the fixture");
    if (requests.empty()) return;
    const size_t bytes = requests[0].body.size();
    const size_t images = Images(Json::parse(requests[0].body));
    Check(bytes < 1024 * 1024, "screenshots are prepared instead of resending raw BMP bytes");
    Check(images == 4, "each tool image is included exactly once");
    Check(requests[0].body.find("image/png") != std::string::npos ||
          requests[0].body.find("image/jpeg") != std::string::npos, "large BMP uses compressed image encoding");
    const auto compacted = BuildCompactedHistory({user, assistant, tool}, "The screenshots show a blue desktop.");
    size_t retained = 0;
    for (const auto& message : compacted) retained += message.image_paths.size();
    tool.image_paths.resize(12, image);
    const auto many = BuildCompactedHistory({user, assistant, tool}, "Screenshots inspected; continue.");
    size_t limited = 0;
    for (const auto& message : many) limited += message.image_paths.size();
    Check(retained == 4 && limited == 4, "compaction retains recent images within its image budget");
    Check(tool.image_paths.size() == 12, "compaction leaves the transcript unchanged");
    request.messages = compacted;
    const auto followup = CreateProviderAdapter(protocol)->Complete(request, {}, nullptr);
    const auto after = server.Requests();
    Check(followup.ok && after.size() == 2, "inference continues after compaction");
    if (after.size() == 2) {
        Check(Images(Json::parse(after.back().body)) == 4 && after.back().body.size() < 1024 * 1024,
              "actual post-compaction request uses prepared recent images");
    }
    std::cout << protocol << ": request_bytes=" << bytes << " images=" << images
              << " compacted_images=" << limited << " reply=" << (result.ok ? result.value.text : result.error) << '\n';
}
}
int main() {
    using namespace spark;
    const auto root = fs::temp_directory_path() /
        ("spark-image-payload-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const auto file = root / "screenshot.bmp";
    std::string bmp(54 + 2048 * 1024 * 3, '\0');
    const auto field = [&](size_t offset, uint32_t value, size_t count) {
        for (size_t i = 0; i < count; ++i) bmp[offset + i] = static_cast<char>(value >> (i * 8));
    };
    bmp[0] = 'B'; bmp[1] = 'M';
    field(2, static_cast<uint32_t>(bmp.size()), 4); field(10, 54, 4); field(14, 40, 4);
    field(18, 2048, 4); field(22, 1024, 4); field(26, 1, 2); field(28, 24, 2);
    std::ofstream stream(file, std::ios::binary); stream.write(bmp.data(), bmp.size()); stream.close();
    for (const auto& protocol : {"chat_completions", "responses", "anthropic_messages"})
        RoundTrip(protocol, PathToUtf8(file));
    std::ifstream unchanged(file, std::ios::binary);
    Check(std::string(std::istreambuf_iterator<char>(unchanged), {}) == bmp, "original image bytes are unchanged");
    std::error_code ignored; fs::remove_all(root, ignored);
    return failures ? 1 : 0;
}
