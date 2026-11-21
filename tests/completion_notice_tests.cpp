#include "gui/completion_notice.h"

#include <iostream>
#include "platform/filesystem.h"

int main() {
    using namespace spark;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
    };
    SessionState state;
    state.title = "**Project**";
    ChatMessage user; user.role = MessageRole::User; user.content = "question";
    ChatMessage assistant; assistant.role = MessageRole::Assistant;
    assistant.content = "**Finished** [file](https://example.invalid/file).\nSecond line.";
    assistant.reasoning_text = "Private reasoning must never appear";
    state.messages = {user, assistant};
    auto notice = MakeCompletionNotice("a.jsonl", state);
    check(notice && notice->path == "a.jsonl" && notice->title == L"Project" &&
          notice->body == L"Finished file. Second line.", "final text is plain, without reasoning or markdown");
    ChatMessage next = user; next.content = "next question"; state.messages.push_back(next);
    check(!MakeCompletionNotice("a.jsonl", state), "never reuses a previous turn's reply");
    state.messages.push_back(assistant);
    state.metadata["paused_turn"] = nlohmann::json::object();
    check(!MakeCompletionNotice("a.jsonl", state), "paused turns are silent");
    state.metadata.clear();
    state.messages.back().tool_calls.push_back(ToolCall{});
    check(!MakeCompletionNotice("a.jsonl", state), "tool commentary is not a final response");
    state.messages.back().tool_calls.clear();
    state.messages.back().content = std::string(20000, 'a');
    notice = MakeCompletionNotice("a.jsonl", state);
    check(notice && notice->body.size() == 261 && notice->body.back() == L'\u2026', "large replies have bounded previews");
    for (const char* origin : {"mcp", "subagent"}) {
        state.metadata["origin"] = origin;
        check(!MakeCompletionNotice("a.jsonl", state), "agent-owned sessions stay silent even if opened in the GUI");
    }
    state.metadata.clear();

    if (failures == 0) std::cout << "Completion notice checks passed.\n";
    return failures == 0 ? 0 : 1;
}
