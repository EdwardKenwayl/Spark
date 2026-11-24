#include "loopback_http.h"
#include "core/agent.h"
#include "core/context_compactor.h"

#include <fstream>
#include <iostream>

namespace {
using namespace spark;
using Server = test::LoopbackHttpServer;
int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void TestImageTurn(bool compact_history, bool high_reported_usage = false) {
    const auto root = fs::temp_directory_path() / NativePath("spark-image-" + NewSessionId());
    fs::create_directories(root);
    const auto image = root / NativePath("clipboard.bmp");
    // A valid 1024x1024 24-bit BMP: its transport size must not become text tokens.
    std::string bitmap(54 + 1024 * 1024 * 3, '\0');
    const auto field = [&](size_t offset, uint32_t value, size_t count) {
        for (size_t byte = 0; byte < count; ++byte)
            bitmap[offset + byte] = static_cast<char>((value >> (8 * byte)) & 255);
    };
    bitmap[0] = 'B'; bitmap[1] = 'M';
    field(2, static_cast<uint32_t>(bitmap.size()), 4);
    field(10, 54, 4); field(14, 40, 4);
    field(18, 1024, 4); field(22, 1024, 4);
    field(26, 1, 2); field(28, 24, 2);
    std::ofstream file(image, std::ios::binary);
    file.write(bitmap.data(), static_cast<std::streamsize>(bitmap.size()));
    file.close();

    size_t summaries = 0;
    Server* active_server = nullptr;
    Server server([&](Server::Connection& connection, size_t) {
        const auto body = nlohmann::json::parse(active_server->Requests().back().body);
        const bool summary = body.dump().find("CONTEXT CHECKPOINT COMPACTION") != std::string::npos;
        std::string response;
        if (summary) {
            ++summaries;
            response = "data: {\"choices\":[{\"delta\":{\"content\":\"PRIVATE_SUMMARY: continue with the latest image question.\"},"
                       "\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
        } else {
            response = "data: {\"choices\":[{\"delta\":{\"content\":\"image answer\"},\"finish_reason\":\"stop\"}],"
                       "\"usage\":{\"prompt_tokens\":" + std::to_string(high_reported_usage ? 320000 : 4096) +
                       ",\"completion_tokens\":4}}\n\ndata: [DONE]\n\n";
        }
        connection.Send(Server::Headers(200, response.size(), "Content-Type: text/event-stream\r\n") + response);
    }, 8, 12 * 1024 * 1024);
    active_server = &server;
    AppConfig config;
    config.workspace.path = PathToUtf8(root);
    config.storage.data_dir = JoinPath(config.workspace.path, "data");
    config.skills.enabled = false;
    config.agent.request_timeout_seconds = 3;
    config.agent.response_header_timeout_seconds = 3;
    config.context.reserve_output_tokens = 1024;
    config.context.summary_max_tokens = 512;
    ProviderConfig provider;
    provider.id = "fixture";
    provider.protocol = "chat_completions";
    provider.base_url = server.Url("/v1");
    provider.api_key = "fixture";
    ModelConfig model;
    model.id = "image-model";
    model.capabilities.context_window = 272000;
    provider.models.push_back(model);
    config.providers.push_back(provider);
    config.active = {provider.id, model.id, ""};
    SessionJournal journal;
    Check(journal.Open(config.storage.data_dir, config.workspace.path).ok, "journal opens");
    if (compact_history) {
        ChatMessage old;
        old.role = MessageRole::User;
        old.content = std::string(1024000, 'a');
        journal.MutableState().messages.push_back(old);
    }
    ChatMessage probe;
    probe.image_paths = {PathToUtf8(image)};
    const auto image_tokens = EstimateMessageTokens(probe);
    Check(image_tokens < 10000, "BMP bytes are not counted as base64 text tokens");
    std::vector<std::string> events;
    std::string visible;
    uint64_t context = 0;
    int completed = 0;
    AgentCallbacks callbacks;
    callbacks.on_phase = [&](AgentPhase phase, const std::string&) {
        if (phase == AgentPhase::Compacting) events.push_back("compacting");
        if (phase == AgentPhase::WaitingModel) events.push_back("request");
    };
    callbacks.on_context_compacted = [&](uint64_t, uint64_t, const std::string&) { events.push_back("compacted"); };
    callbacks.on_text_delta = [&](const std::string& text) { visible += text; };
    callbacks.on_context_updated = [&](uint64_t value, uint64_t) { context = value; };
    callbacks.on_turn_finished = [&] { ++completed; events.push_back("finished"); };
    CancellationToken cancellation;
    Agent agent(config, &journal, nullptr);
    const auto result = agent.RunTurn("Describe this image.", [](const ApprovalRequest&) { return true; },
        callbacks, &cancellation, false, {PathToUtf8(image)});
    Check(result.ok && completed == 1, "image turn completes after any necessary compaction");
    Check(server.Error().empty(), "provider fixture has no transport error");
    Check(summaries == (compact_history || high_reported_usage ? 1u : 0u), "history pressure causes exactly one compaction");
    const std::vector<std::string> expected = high_reported_usage
        ? std::vector<std::string>{"request", "compacting", "compacted", "finished"} : compact_history
        ? std::vector<std::string>{"compacting", "compacted", "request", "finished"}
        : std::vector<std::string>{"request", "finished"};
    Check(events == expected, "compaction precedes the normal request and final response");
    Check(visible == "image answer", "summary stays out of streamed assistant output");
    Check(high_reported_usage ? context < 10000 : context == 4100,
          "reported context wins until a compaction replaces that context");
    Check(!journal.State().messages.back().image_paths.size() &&
        journal.State().messages.back().content == "image answer", "final answer is persisted");
    Check(journal.State().messages[journal.State().messages.size() - 2].image_paths.size() == 1,
          "compaction keeps the current image in the actual transcript");
    std::cout << (compact_history ? "long history" : "new image")
              << ": image_tokens=" << image_tokens << " summaries=" << summaries
              << " context=" << context << " visible=" << visible
              << " completed=" << completed << " ok=" << result.ok << '\n';
    std::error_code ignored;
    fs::remove_all(root, ignored);
}
}

int main() {
    TestImageTurn(false);
    TestImageTurn(true);
    TestImageTurn(false, true);
    return failures == 0 ? 0 : 1;
}
