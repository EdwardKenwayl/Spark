#include "gui/transcript.h"

#include <algorithm>
#include "platform/filesystem.h"
#include <iostream>

namespace {
using namespace spark;
int failures = 0;
void Check(bool condition, const char* message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

size_t Add(Transcript& transcript, ChatItemKind kind, const std::string& text = {}) {
    ChatItem item;
    item.kind = kind;
    item.text = text;
    return transcript.Apply({TranscriptEventKind::Add, kNoChatItem, item}, {123, 1000});
}

void TestEventsAndTimers() {
    Transcript transcript;
    Add(transcript, ChatItemKind::User, "question");
    const auto status = Add(transcript, ChatItemKind::TurnStatus);
    const auto reasoning = Add(transcript, ChatItemKind::Reasoning, "thinking");
    const auto intermediate = Add(transcript, ChatItemKind::Assistant, "intermediate");
    ChatItem tool;
    tool.kind = ChatItemKind::Tool;
    tool.tool_name = "read_file";
    const auto first = transcript.Apply({TranscriptEventKind::Add, kNoChatItem, tool}, {123, 1000});
    const auto separator = Add(transcript, ChatItemKind::Reasoning, "still thinking");
    tool.tool_name = "run_command";
    const auto second = transcript.Apply({TranscriptEventKind::Add, kNoChatItem, tool}, {123, 1200});
    ChatItem completion;
    completion.success = false;
    completion.result = "failed";
    completion.elapsed_ms = 200;
    transcript.Apply({TranscriptEventKind::Finish, first, completion}, {124, 1250});
    auto groups = transcript.Groups(1500);
    Check(groups[first].leader && groups[first].size == 2 && groups[first].read_count == 1 &&
              groups[first].command_count == 1 && groups[first].elapsed_ms == 500 &&
              groups[first].active && groups[first].failed && groups[second].hidden,
          "exploration groups span reasoning and combine completed and live tool timing");
    transcript.ToggleToolGroup(first);
    transcript.ToggleTool(second);
    Check(!transcript.Groups(1500)[second].hidden && transcript.Item(second)->expanded,
          "group and individual tool expansion remain independent");
    Check(Transcript::Elapsed(*transcript.Item(second), 1100) == 0 &&
              Transcript::Elapsed(*transcript.Item(status), 1600) == 600,
          "explicit monotonic clocks drive timers without underflow");
    completion.elapsed_ms = 0;
    transcript.Apply({TranscriptEventKind::Finish, second, completion}, {125, 1600});
    Check(transcript.Item(second)->elapsed_ms == 400, "tool completion freezes the live duration");
    const auto final = Add(transcript, ChatItemKind::Assistant, "answer");
    ChatItem delta;
    delta.text = "\x1b[31m!\x1b[0m";
    transcript.Apply({TranscriptEventKind::AppendText, final, delta}, {125, 1600});
    transcript.Apply({TranscriptEventKind::Finish, final, {}}, {126, 1700});
    completion.elapsed_ms = 700;
    transcript.Apply({TranscriptEventKind::FinishTurn, status, completion}, {126, 1700});
    Check(transcript.Item(final)->text == "answer!" && transcript.Item(final)->final_assistant &&
              !transcript.Item(intermediate)->final_assistant && !transcript.Item(intermediate)->visible &&
              !transcript.Item(first)->visible && !transcript.Item(reasoning)->visible,
          "turn completion hides intermediate activity and grants final-reply metadata once");
    transcript.ToggleTurnStatus(status);
    Check(transcript.Item(intermediate)->visible && transcript.Item(first)->visible &&
              !transcript.Item(separator)->visible && transcript.Item(final)->final_assistant,
          "expanding a finished turn restores tools and intermediate replies, leaving reasoning hidden");
    const auto user = Add(transcript, ChatItemKind::User, "edit me");
    const auto paused = Add(transcript, ChatItemKind::TurnStatus);
    Add(transcript, ChatItemKind::Tool);
    transcript.Apply({TranscriptEventKind::PauseTurn, paused, completion}, {127, 1800});
    Check(transcript.Item(paused)->paused && transcript.Item(paused)->expanded &&
              transcript.Items().back().visible, "paused turns keep their activity expanded");
    ChatItem resumed;
    resumed.start_tick_ms = 100;
    resumed.elapsed_ms = 7200000;
    transcript.Apply({TranscriptEventKind::ResumeTurn, paused, resumed}, {128, 100});
    Check(Transcript::Elapsed(*transcript.Item(paused), 600) == 7200500,
          "resuming after reboot retains accumulated time beyond the current system uptime");
    transcript.EraseFrom(user);
    Check(transcript.LastUserIndex() == 0 && transcript.Item(user) == nullptr,
          "editing the last user turn removes its tail and repairs the last-user index");
    const auto error = Add(transcript, ChatItemKind::Error, "old failure");
    Check(Add(transcript, ChatItemKind::Error, "latest failure") == error &&
              transcript.Item(error)->text == "latest failure", "errors replace the previous error in their turn");
    const auto bounded = Add(transcript, ChatItemKind::Reasoning, std::string(16383, 'a') + u8"\u4e2d\u6587");
    Check(transcript.Item(bounded)->text.size() <= 16384 && IsValidUtf8(transcript.Item(bounded)->text),
          "reasoning truncation remains bounded at a UTF-8 scalar boundary");
}

void TestJournalReplay(const spark::fs::path& root) {
    SessionJournal journal;
    Check(journal.Open(PathToUtf8(root / "data"), PathToUtf8(root)).ok, "fixture journal opens");
    const auto message = [&](MessageRole role, const std::string& content) {
        ChatMessage item;
        item.role = role;
        item.content = content;
        item.timestamp = 1700000000 + journal.State().messages.size();
        journal.MutableState().messages.push_back(item);
        Check(journal.Append("message", item).ok, "fixture message is journaled");
    };
    message(MessageRole::User, u8"\u4fdd\u7559\u5bf9\u8bdd");
    message(MessageRole::Assistant, "before tools");
    ChatMessage call;
    call.role = MessageRole::Assistant;
    call.tool_calls = {{"call-1", "read_file", "{}"}, {"missing", "search_text", "{}"}};
    journal.MutableState().messages.push_back(call);
    Check(journal.Append("message", call).ok, "tool calls persist");
    ChatMessage result;
    result.role = MessageRole::Tool;
    result.tool_call_id = "call-1";
    result.content = "fixture contents";
    journal.MutableState().messages.push_back(result);
    Check(journal.Append("message", result).ok &&
              journal.Append("tool.execution", {{"tool_call_id", "call-1"}, {"elapsed_ms", 42}}).ok,
          "tool result and duration persist");
    Check(journal.Append("context.compacted", {{"trigger", "manual"}, {"message_count", 4},
                                              {"summary", "model-only handoff"}}).ok, "compaction persists");
    message(MessageRole::Assistant, "final answer");
    Check(journal.Append("turn.duration", {{"turn_index", 0}, {"elapsed_ms", 999}}).ok &&
              journal.Append("turn.error", {{"turn_index", 0}, {"error", "fixture failure"},
                                           {"timestamp", 1700000009}}).ok &&
              journal.Append("turn.usage", {{"turn_index", 0}, {"input_tokens", 20},
                                            {"output_tokens", 10}, {"model_calls", 1}}).ok,
          "turn metadata persists");
    journal.MutableState().context_messages = {ChatMessage{}};
    journal.MutableState().context_messages[0].content = "must not replace the visible transcript";
    Check(journal.SaveSnapshot().ok, "snapshot persists");
    SessionJournal resumed;
    Check(resumed.Resume(journal.Path()).ok, "journal resumes");
    Transcript transcript;
    const auto generation = transcript.BeginReplay(resumed.State(), {200, 5000});
    Check(!transcript.ReplayStep(generation, 1) && transcript.Count() == 2,
          "replay yields after a bounded batch without losing the pending turn");
    while (!transcript.ReplayStep(generation)) {}
    const auto& items = transcript.Items();
    Check(items.size() == 9 && items[0].text == u8"\u4fdd\u7559\u5bf9\u8bdd" &&
              items[0].timestamp == 1700000000 && items[5].kind == ChatItemKind::Compaction &&
              !items[5].success && items[6].text == "final answer" && items[6].final_assistant,
          "replay retains transcript ordering, Unicode, timestamps and manual compaction position");
    Check(items[1].finished && items[1].elapsed_ms == 999 && items[3].elapsed_ms == 42 &&
              items[4].finished && !items[4].success && items[7].kind == ChatItemKind::Error &&
              items[8].kind == ChatItemKind::UsageStats && items[8].input_tokens == 20,
          "replay restores turn and tool timing, incomplete-tool failure, errors and usage");
    Check(!transcript.NeedsAnimation(), "finished history has no active animation timers");
    const auto obsolete = transcript.BeginReplay(resumed.State(), {201, 6000});
    transcript.Clear();
    Add(transcript, ChatItemKind::User, "fresh view");
    Check(!transcript.ReplayStep(obsolete) && transcript.Count() == 1 &&
              transcript.Item(0)->text == "fresh view", "a stale replay timer cannot mutate a cleared view");
    SessionState paused = resumed.State();
    paused.metadata["paused_turn"] = {{"elapsed_ms", 456}};
    auto current = transcript.BeginReplay(paused, {202, 7000});
    while (!transcript.ReplayStep(current)) {}
    Check(transcript.Item(1)->paused && transcript.Item(1)->expanded && transcript.Item(2)->visible,
          "paused journal restoration preserves expanded activity");
    current = transcript.BeginReplay(paused, {203, 8000}, {true, 7500});
    while (!transcript.ReplayStep(current)) {}
    Check(!transcript.Item(1)->finished && transcript.Item(1)->start_tick_ms == 7500 &&
              !transcript.Item(4)->finished && transcript.NeedsAnimation(),
          "an explicitly running session keeps the final turn and unfinished tools live");
}

void TestTurnBoundaries() {
    SessionState state;
    for (const auto role : {MessageRole::User, MessageRole::Assistant, MessageRole::User, MessageRole::Assistant}) {
        ChatMessage message;
        message.role = role;
        message.content = "turn boundary";
        state.messages.push_back(message);
    }
    state.metadata["turn_durations_ms"] = nlohmann::json::array({uint64_t{10}, uint64_t{20}});
    state.metadata["paused_turn"] = {{"elapsed_ms", 30}};
    Transcript transcript;
    auto generation = transcript.BeginReplay(state, {300, 1000});
    state.messages.clear();
    while (!transcript.ReplayStep(generation)) {}
    Check(transcript.Item(1)->elapsed_ms == 10 && !transcript.Item(1)->paused &&
              transcript.Item(2)->final_assistant && transcript.Item(4)->paused &&
              transcript.Item(4)->elapsed_ms == 30 && !transcript.Item(5)->final_assistant,
          "replay owns its snapshot, indexes durations per user turn and pauses only the final turn");
    state.messages.resize(130);
    for (auto& message : state.messages) message.role = MessageRole::User;
    generation = transcript.BeginReplay(state, {301, 2000});
    Check(!transcript.ReplayStep(generation, 1000) && transcript.Count() == 128,
          "even oversized replay requests yield after the hard 64-message batch cap");
}

void TestLiveReplay(const spark::fs::path& root) {
    SessionState state;
    const auto message = [&](MessageRole role, const std::string& content) -> ChatMessage& {
        state.messages.emplace_back();
        auto& item = state.messages.back();
        item.role = role;
        item.content = content;
        return item;
    };
    message(MessageRole::User, "previous question");
    message(MessageRole::Assistant, "previous answer");
    message(MessageRole::User, "external question");
    auto& assistant = message(MessageRole::Assistant, "saved response");
    assistant.reasoning_text = "saved thought";
    assistant.tool_calls = {{"done", "read_file", "{}"}, {"pending", "search_text", "{}"}};
    message(MessageRole::Tool, "full saved output").tool_call_id = "done";
    state.metadata["turn_durations_ms"] = nlohmann::json::array({uint64_t{50}});
    auto& live = state.metadata["live_turn"];
    live = {{"active", true}, {"turn_index", 1}, {"started_tick", 1000},
            {"text", "saved responsestreaming tail"}, {"reasoning", "saved thoughtlive thought"},
            {"tools", nlohmann::json::array({
                {{"tool_call_id", "done"}, {"started_tick", 1100}, {"finished", true},
                 {"success", false}, {"output", "short output"}, {"elapsed_ms", 0}},
                {{"tool_call_id", "pending"}, {"started_tick", 1200}, {"finished", false}},
                {{"tool_call_id", "new"}, {"tool_name", "run_command"}, {"arguments", "{}"},
                 {"started_tick", 1300}, {"finished", true}, {"success", false},
                 {"output", "live failure"}, {"elapsed_ms", 75}}
            })}};
    live["tools"].push_back(live["tools"].back());
    SessionJournal journal;
    Check(journal.Open(PathToUtf8(root / "external-data"), PathToUtf8(root)).ok,
          "external journal fixture opens");
    journal.MutableState().messages = state.messages;
    journal.MutableState().metadata = state.metadata;
    Check(journal.SaveSnapshot().ok, "external journal persists the live metadata snapshot");
    SessionJournal resumed;
    const auto loaded = resumed.Resume(journal.Path());
    Check(loaded.ok, "external live journal resumes");
    if (!loaded.ok) return;
    Transcript transcript;
    auto generation = transcript.BeginReplay(resumed.State(), {400, 2000});
    size_t steps = 0;
    while (!transcript.ReplayStep(generation, 1)) ++steps;
    Check(steps >= state.messages.size(), "live tools share the bounded replay batch budget");
    const auto find = [&](ChatItemKind kind, const std::string& key) -> const ChatItem* {
        for (const auto& item : transcript.Items())
            if (item.kind == kind && (kind == ChatItemKind::Tool ? item.call_id : item.text) == key) return &item;
        return nullptr;
    };
    const auto count = [&](ChatItemKind kind) {
        return std::count_if(transcript.Items().begin(), transcript.Items().end(),
                             [kind](const ChatItem& item) { return item.kind == kind; });
    };
    const auto* status = transcript.Item(4);
    Check(transcript.Item(1)->finished && transcript.Item(1)->elapsed_ms == 50 &&
              status && !status->finished && status->start_tick_ms == 1000 &&
              Transcript::Elapsed(*status, 2500) == 1500,
          "live metadata keeps only its owning turn active with the persisted start tick");
    const auto* text = find(ChatItemKind::Assistant, "streaming tail");
    const auto* reasoning = find(ChatItemKind::Reasoning, "live thought");
    Check(count(ChatItemKind::Assistant) == 3 && text && !text->finished && !text->final_assistant &&
              reasoning && !reasoning->visible && find(ChatItemKind::Assistant, "saved response"),
          "only unsaved live text and reasoning are added, leaving completed replies unduplicated");
    const auto* done = find(ChatItemKind::Tool, "done");
    const auto* pending = find(ChatItemKind::Tool, "pending");
    const auto* added = find(ChatItemKind::Tool, "new");
    Check(count(ChatItemKind::Tool) == 3 && done && done->finished && !done->success &&
              done->result == "full saved output" && done->elapsed_ms == 0 && done->start_tick_ms == 1100 &&
              pending && !pending->finished && pending->start_tick_ms == 1200 &&
              added && added->finished && !added->success && added->result == "live failure" &&
              added->elapsed_ms == 75 && transcript.NeedsAnimation(),
          "tool IDs reconcile saved/live completions without reopening, truncating, or duplicating rows");
    generation = transcript.BeginReplay(state, {401, 3000}, {true, 0});
    while (!transcript.ReplayStep(generation)) {}
    pending = find(ChatItemKind::Tool, "pending");
    Check(transcript.Item(4)->start_tick_ms == 1000 && pending &&
              Transcript::Elapsed(*pending, 3500) == 2300 && count(ChatItemKind::Tool) == 3,
          "external running hints retain journal timers and stable tool IDs on the next poll");
    message(MessageRole::Assistant, "streaming tail").reasoning_text = "live thought";
    generation = transcript.BeginReplay(state, {402, 4000});
    while (!transcript.ReplayStep(generation)) {}
    text = find(ChatItemKind::Assistant, "streaming tail");
    Check(count(ChatItemKind::Assistant) == 3 && count(ChatItemKind::Reasoning) == 0 &&
              text && text->finished,
          "a completed assistant message absorbs overlapping cumulative live buffers");
    live["active"] = false;
    generation = transcript.BeginReplay(state, {403, 5000});
    while (!transcript.ReplayStep(generation)) {}
    text = find(ChatItemKind::Assistant, "streaming tail");
    Check(transcript.Item(4)->finished && !transcript.NeedsAnimation() &&
              text && text->final_assistant,
          "inactive live metadata leaves normal completed-history folding intact");
    live["active"] = true;
    for (const auto index : {0, -1, 99}) {
        live["turn_index"] = index;
        generation = transcript.BeginReplay(state, {404, 6000});
        while (!transcript.ReplayStep(generation)) {}
        Check(transcript.Item(4)->finished && count(ChatItemKind::Tool) == 2 &&
                  count(ChatItemKind::Reasoning) == 0,
              "stale, negative, and future live indices cannot attach activity to a different turn");
    }
    state.messages.clear();
    live["turn_index"] = 0;
    generation = transcript.BeginReplay(state, {405, 7000});
    while (!transcript.ReplayStep(generation)) {}
    Check(transcript.Count() == 0, "orphan live metadata cannot invent a user turn");
}

void TestLiveTurnIdentity() {
    SessionState state;
    ChatMessage user;
    user.role = MessageRole::User;
    ChatMessage assistant;
    assistant.role = MessageRole::Assistant;
    assistant.tool_calls = {{"reused", "read_file", "{}"}};
    ChatMessage result;
    result.role = MessageRole::Tool;
    result.tool_call_id = "reused";
    result.content = "previous result";
    state.messages = {user, assistant, result, user};
    state.metadata["live_turn"] = {
        {"active", true}, {"turn_index", 1}, {"started_tick", 1000}, {"phase", "waiting_model"},
        {"reasoning", "current thought"}, {"tools", nlohmann::json::array({
            {{"tool_call_id", "reused"}, {"tool_name", "read_file"},
             {"started_tick", 1300}, {"finished", false}}
        })}};
    Transcript transcript;
    auto generation = transcript.BeginReplay(state, {500, 2000}, {true, 900});
    while (!transcript.ReplayStep(generation)) {}
    Check(transcript.Count() == 7 && transcript.Item(2)->call_id == "reused" &&
              transcript.Item(2)->finished && transcript.Item(2)->result == "previous result" &&
              !transcript.Item(2)->visible && transcript.Item(4)->start_tick_ms == 900 &&
              transcript.Item(5)->call_id == "reused" && !transcript.Item(5)->finished &&
              transcript.Item(5)->start_tick_ms == 1300 && transcript.Item(6)->visible &&
              transcript.Item(6)->text == "current thought",
          "tool identity is scoped to its turn, reasoning-only tails show, and explicit run clocks take priority");
    state.metadata["live_turn"]["tools"] = nlohmann::json::array();
    for (size_t index = 0; index < 130; ++index)
        state.metadata["live_turn"]["tools"].push_back({{"tool_call_id", std::to_string(index)}});
    generation = transcript.BeginReplay(state, {501, 3000});
    Check(!transcript.ReplayStep(generation, 1000) && transcript.Count() == 65,
          "large live tails yield within the same hard 64-entry budget as journal messages");
    transcript.Clear();
    Check(!transcript.ReplayStep(generation) && transcript.Count() == 0,
          "clearing during live-tail replay rejects its remaining tools and text");
}

void TestHistory(const spark::fs::path& root) {
    const std::string path = PathToUtf8(root / NativePath(u8"\u5386\u53f2.jsonl"));
    std::string text = "\xef\xbb\xbf";
    const auto event = [&](const char* type, const nlohmann::json& payload, uint64_t time) {
        text += nlohmann::json({{"type", type}, {"timestamp", time}, {"payload", payload}}).dump() + "\r\n";
    };
    event("session.created", {{"title", "draft"}, {"workspace", PathToUtf8(root)}}, 1);
    Check(WriteFileUtf8Atomic(path, text), "history draft fixture is written");
    auto history = ReadTranscriptHistory(path);
    Check(history.ok && !history.value.has_messages, "draft-only histories stay distinguishable from conversations");
    event("message", {{"role", "user"}, {"content", "old message"}}, 2);
    event("session.pinned", {{"pinned", true}}, 3);
    event("session.scope", {{"scope", "recent"}}, 4);
    event("snapshot", {{"title", "renamed"}, {"messages", nlohmann::json::array()}}, 5);
    text += "{\"broken tail\":";
    Check(WriteFileUtf8Atomic(path, text), "history edit fixture is written");
    history = ReadTranscriptHistory(path);
    Check(history.ok && history.value.path == path && history.value.title == "renamed" &&
              history.value.pinned && history.value.scope == "recent" && history.value.updated_at == 5 &&
              history.value.has_messages && history.value.workspace == PathToUtf8(root),
          "history survives BOM, CRLF, empty edited snapshots and damaged tails with pin/scope/title intact");
    CancellationToken cancellation;
    cancellation.Cancel();
    Check(!ReadTranscriptHistory(path, &cancellation).ok && !ReadTranscriptHistory(path + ".missing").ok,
          "history reads report cancellation and missing files without creating a journal");
}
}  // namespace

int main() {
    const auto root = spark::fs::temp_directory_path() / spark::NativePath("spark-transcript-" + spark::NewSessionId());
    spark::fs::create_directories(root);
    try {
        TestEventsAndTimers();
        TestJournalReplay(root);
        TestTurnBoundaries();
        TestLiveReplay(root);
        TestLiveTurnIdentity();
        TestHistory(root);
    } catch (const std::exception& exception) {
        Check(false, exception.what());
    }
    std::error_code error;
    spark::fs::remove_all(root, error);
    if (failures == 0) std::cout << "PASS: transcript events, grouping, replay, clocks and history\n";
    return failures == 0 ? 0 : 1;
}
