#include "gui/transcript.h"
#include "gui/history_window.h"
#include "gui/history_browser.h"
#include "gui/chat_ui_helpers.h"

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

void TestEditedImageReplay(const fs::path& root) {
    ChatMessage edited;
    edited.role = MessageRole::User;
    const std::string image = PathToUtf8(root / "edited.png");
    edited.image_paths = {image};
    edited.content = u8"[附件引用]\n- " + image + "\n\nOriginal question";
    auto attachments = edited.image_paths;
    edited.content = ui::UserMessageText(edited.content, &attachments) + " updated";
    edited.image_paths = attachments;
    SessionJournal journal;
    Check(journal.Open(PathToUtf8(root), PathToUtf8(root)).ok, "edited image journal opens");
    journal.MutableState().messages = {edited};
    journal.SaveSnapshot();
    SessionJournal restored;
    Check(restored.Resume(journal.Path()).ok, "edited image journal reloads");
    Transcript model;
    const auto replay = model.BeginReplay(restored.State(), {123, 1000});
    while (!model.ReplayStep(replay)) {}
    Check(model.Count() > 0 && model.Item(0)->text == "Original question updated" &&
              model.Item(0)->image_paths == std::vector<std::string>{image},
          "edited images remain structured and visible after journal reload and transcript replay");
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
              groups[first].active && groups[first].failed && groups[second].hidden && !groups[first].mixed,
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
    ChatItem resumed_status;
    resumed_status.kind = ChatItemKind::TurnStatus;
    const auto resumed = transcript.Apply({TranscriptEventKind::Add, kNoChatItem, resumed_status}, {128, 100});
    Check(Transcript::Elapsed(*transcript.Item(paused), 600) == 700 &&
              Transcript::Elapsed(*transcript.Item(resumed), 600) == 500,
          "resume freezes the interrupted segment and starts a fresh timer");
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

void TestCompactionActivity() {
    Transcript transcript;
    Add(transcript, ChatItemKind::User, "image question");
    const auto status = Add(transcript, ChatItemKind::TurnStatus);
    const auto started = transcript.Item(status)->start_tick_ms;
    transcript.BeginCompaction({124, 1500});
    transcript.BeginCompaction({124, 1501});
    Check(transcript.Count() == 3 && transcript.Item(2)->kind == ChatItemKind::Compaction &&
              !transcript.Item(2)->finished && transcript.Item(status)->start_tick_ms == started,
          "one compaction progress row is visible without resetting the processing timer");
    transcript.FinishCompaction(true, {125, 2000});
    Check(transcript.Count() == 3 && transcript.Item(2)->finished && transcript.Item(2)->success,
          "success replaces the existing progress row");
    transcript.BeginCompaction({126, 2500});
    transcript.CancelCompaction({127, 3000});
    transcript.Apply({TranscriptEventKind::FinishTurn, status, {}}, {127, 3000});
    transcript.ToggleTurnStatus(status);
    Check(!transcript.Item(3)->visible && !transcript.Item(3)->finished,
          "cancellation leaves no progress or success row, even when history is expanded");
}

void TestResumeReplay(const fs::path& root) {
    SessionJournal journal;
    Check(journal.Open(PathToUtf8(root / "resume"), PathToUtf8(root)).ok, "resume fixture opens");
    const auto message = [&](MessageRole role, const char* text) {
        ChatMessage item;
        item.role = role;
        item.content = text;
        journal.MutableState().messages.push_back(item);
        Check(journal.Append("message", item).ok, "fixture message persists");
    };
    message(MessageRole::User, "one request");
    message(MessageRole::Assistant, "step one");
    journal.SavePause(3601000);
    message(MessageRole::System, "resume");
    journal.ClearPause();
    message(MessageRole::Assistant, "step two");
    journal.SavePause(900);
    message(MessageRole::System, "resume");
    journal.ClearPause();
    message(MessageRole::Assistant, "answer");
    journal.Append("turn.duration", {{"elapsed_ms", uint64_t{1200}}});
    journal.SaveSnapshot();
    SessionJournal restored;
    Check(restored.Resume(journal.Path()).ok, "resume fixture reopens");
    Transcript transcript;
    const auto replay = transcript.BeginReplay(restored.State(), {123, 1000});
    while (!transcript.ReplayStep(replay)) {}
    std::vector<uint64_t> durations;
    std::vector<std::string> answers;
    size_t users = 0;
    for (const auto& item : transcript.Items()) {
        if (item.kind == ChatItemKind::TurnStatus) durations.push_back(item.elapsed_ms);
        if (item.kind == ChatItemKind::Assistant) answers.push_back(item.text);
        if (item.kind == ChatItemKind::User) ++users;
    }
    Check(durations == std::vector<uint64_t>{3601000, 900, 1200} &&
          answers == std::vector<std::string>{"step one", "step two", "answer"} && users == 1,
          "replay restores all interrupted segments and their independent elapsed times in the original turn");
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

void TestCompactionFolding() {
    Transcript transcript;
    ChatItem manual;
    manual.kind = ChatItemKind::Compaction;
    manual.success = false;
    const auto before = transcript.Apply({TranscriptEventKind::Add, kNoChatItem, manual}, {100, 1000});
    Add(transcript, ChatItemKind::User, "question");
    const auto status = Add(transcript, ChatItemKind::TurnStatus);
    const auto first = Add(transcript, ChatItemKind::Compaction);
    Add(transcript, ChatItemKind::Assistant, "intermediate");
    const auto second = transcript.Apply({TranscriptEventKind::Add, kNoChatItem, manual}, {124, 1100});
    Add(transcript, ChatItemKind::Tool);
    const auto final = Add(transcript, ChatItemKind::Assistant, "answer");
    const auto third = Add(transcript, ChatItemKind::Compaction);
    Check(transcript.Item(first)->visible && transcript.Item(second)->visible && transcript.Item(third)->visible,
          "all compactions remain visible while their turn is active");
    transcript.Apply({TranscriptEventKind::FinishTurn, status, {}}, {130, 1200});
    Check(!transcript.Item(first)->visible && !transcript.Item(second)->visible &&
              !transcript.Item(third)->visible && transcript.Item(final)->visible &&
              transcript.Item(before)->visible,
          "finishing a turn folds every owned compaction without hiding a pre-turn manual notice");
    const auto after = transcript.Apply({TranscriptEventKind::Add, kNoChatItem, manual}, {131, 1300});
    transcript.ToggleTurnStatus(status);
    Check(transcript.Item(first)->visible && transcript.Item(second)->visible &&
              transcript.Item(third)->visible && first < second && second < final && final < third &&
              transcript.Item(before)->visible && transcript.Item(after)->visible,
          "expansion restores all compactions at their original positions, including a trailing in-turn notice");
    transcript.ToggleTurnStatus(status);
    Check(!transcript.Item(first)->visible && !transcript.Item(second)->visible &&
              !transcript.Item(third)->visible && transcript.Item(before)->visible && transcript.Item(after)->visible,
          "repeated folding leaves a manual compaction emitted after completion outside the finished turn");
    Add(transcript, ChatItemKind::User, "next question");
    const auto paused = Add(transcript, ChatItemKind::TurnStatus);
    const auto paused_compaction = Add(transcript, ChatItemKind::Compaction);
    transcript.Apply({TranscriptEventKind::PauseTurn, paused, {}}, {132, 1400});
    Check(transcript.Item(paused_compaction)->visible, "pausing preserves visible in-turn compactions");
    transcript.ToggleTurnStatus(paused);
    Check(!transcript.Item(paused_compaction)->visible, "a paused turn can fold its compactions");
    transcript.ToggleTurnStatus(paused);
    Check(transcript.Item(paused_compaction)->visible && transcript.Item(after)->visible,
          "a paused turn restores its own compactions without absorbing the preceding manual notice");
}

void TestCompactionReplayBoundaries() {
    SessionState state;
    for (const auto role : {MessageRole::User, MessageRole::Assistant, MessageRole::User,
                            MessageRole::Assistant, MessageRole::Assistant}) {
        ChatMessage message;
        message.role = role;
        message.content = "compaction boundary";
        message.timestamp = 100 + state.messages.size() * 10;
        state.messages.push_back(message);
    }
    state.metadata["context_compactions"] = nlohmann::json::array({
        {{"trigger", "manual"}, {"message_count", 0}, {"timestamp", uint64_t{90}}},
        {{"trigger", "auto"}, {"message_count", 1}, {"timestamp", uint64_t{105}}},
        {{"trigger", "manual"}, {"message_count", 2}, {"timestamp", uint64_t{115}}},
        {{"trigger", "auto"}, {"message_count", 3}, {"timestamp", uint64_t{125}}},
        {{"trigger", "auto"}, {"message_count", 4}, {"timestamp", uint64_t{135}}},
        {{"trigger", "manual"}, {"message_count", 5}, {"timestamp", uint64_t{145}}}
    });
    for (const auto budget : {size_t{1}, size_t{64}}) {
        Transcript transcript;
        const auto generation = transcript.BeginReplay(state, {999, 1000});
        while (!transcript.ReplayStep(generation, budget)) {}
        std::vector<size_t> compactions, statuses;
        for (size_t index = 0; index < transcript.Count(); ++index) {
            if (transcript.Item(index)->kind == ChatItemKind::Compaction) compactions.push_back(index);
            if (transcript.Item(index)->kind == ChatItemKind::TurnStatus) statuses.push_back(index);
        }
        Check(compactions.size() == 6 && statuses.size() == 2,
              "bounded and full replay retain every valid compaction exactly once");
        if (compactions.size() != 6 || statuses.size() != 2) continue;
        const std::vector<size_t> expected = {0, 3, 5, 8, 10, 12};
        Check(compactions == expected && transcript.Item(6)->kind == ChatItemKind::User &&
                  transcript.Item(11)->final_assistant,
              "compactions replay at their message boundaries in recorded order");
        Check(transcript.Item(compactions[0])->timestamp == 90 &&
                  transcript.Item(compactions[1])->timestamp == 105 &&
                  transcript.Item(compactions[5])->timestamp == 145,
              "replayed compactions retain event timestamps instead of receiving the replay clock");
        Check(transcript.Item(compactions[0])->visible && !transcript.Item(compactions[1])->visible &&
                  transcript.Item(compactions[2])->visible && !transcript.Item(compactions[3])->visible &&
                  !transcript.Item(compactions[4])->visible && transcript.Item(compactions[5])->visible,
              "only in-turn compactions fold; leading, between-turn and trailing manual notices stay visible");
        transcript.ToggleTurnStatus(statuses[0]);
        transcript.ToggleTurnStatus(statuses[0]);
        transcript.ToggleTurnStatus(statuses[1]);
        transcript.ToggleTurnStatus(statuses[1]);
        Check(transcript.Item(compactions[0])->visible && transcript.Item(compactions[2])->visible &&
                  transcript.Item(compactions[5])->visible && !transcript.Item(compactions[1])->visible &&
                  !transcript.Item(compactions[3])->visible && !transcript.Item(compactions[4])->visible,
              "toggling neighboring turns never adopts a manual compaction between completed turns");
    }
    state.metadata["context_compactions"].push_back(
        {{"trigger", "auto"}, {"message_count", 5}, {"timestamp", uint64_t{150}}});
    state.metadata["live_turn"] = {{"active", true}, {"turn_index", 1}};
    Transcript active;
    const auto generation = active.BeginReplay(state, {999, 1000});
    while (!active.ReplayStep(generation, 1)) {}
    Check(!active.Item(7)->finished && active.Items().back().kind == ChatItemKind::Compaction &&
              active.Items().back().visible && active.Item(8)->visible && active.Item(10)->visible,
          "active replay keeps both interior and end-boundary compactions visible");
}

void TestInvalidCompactionBoundaries() {
    SessionState state;
    ChatMessage user;
    user.role = MessageRole::User;
    ChatMessage assistant;
    assistant.role = MessageRole::Assistant;
    assistant.content = "answer";
    state.messages = {user, assistant};
    state.metadata["context_compactions"] = nlohmann::json::array({
        {{"message_count", 99}},
        {{"message_count", -1}},
        {{"message_count", "1"}},
        {{"message_count", 1.5}},
        {{"message_count", nullptr}},
        {{"trigger", "auto"}},
        {{"trigger", "auto"}, {"message_count", 1}, {"timestamp", uint64_t{105}}},
        {{"message_count", 100}}
    });
    Transcript transcript;
    const auto generation = transcript.BeginReplay(state, {999, 1000});
    while (!transcript.ReplayStep(generation, 1)) {}
    const auto count = std::count_if(transcript.Items().begin(), transcript.Items().end(),
        [](const ChatItem& item) { return item.kind == ChatItemKind::Compaction; });
    Check(count == 1 && transcript.Item(2)->kind == ChatItemKind::Compaction &&
              transcript.Item(2)->timestamp == 105 && !transcript.Item(2)->visible,
          "invalid and future boundaries neither move to the tail nor block a later valid event");
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

void TestIndexedHistoryNavigation() {
    SessionState state;
    for (int turn = 0; turn < 400; ++turn) {
        ChatMessage user, reply;
        user.role = MessageRole::User;
        user.content = "Prompt " + std::to_string(turn);
        reply.role = MessageRole::Assistant;
        reply.content = "Reply " + std::to_string(turn);
        state.messages.push_back(std::move(user));
        state.messages.push_back(std::move(reply));
    }
    HistoryBrowser browser([&] { return &state; });
    browser.Update();
    const auto latest = HistoryRailLayout(browser.Count(), 0, 500, 10, 12);
    const auto oldest = HistoryRailLayout(browser.Count(), 1000, 500, 10, 12);
    Check(latest.first + latest.count == 400 && oldest.first == 0 && oldest.count == latest.count,
          "the rail can scroll independently from the last marker to the first without overlapping");
    browser.Hover(10, 100);
    Check(browser.PreviewUnavailable(), "the oldest markers remain navigable without a preview");
    browser.Hover(150, 200);
    Check(!browser.UpdatePreview(349) && !browser.PreviewReady(), "preview starts in the loading state");
    Check(browser.UpdatePreview(350) && browser.prompt == "Prompt 150" && browser.reply == "Reply 150",
          "preview reads only the selected turn's prompt and final reply");
    browser.Hover(151, 351);
    Check(browser.prompt.empty() && browser.reply.empty() && !browser.PreviewReady(),
          "moving to another marker clears the previous preview immediately");
    browser.Open(200, 392, {1700000000, 1000});
    while (!browser.Step()) {}
    Check(browser.page.Contains(200) && browser.page.end_turn - browser.page.first_turn == 10 &&
              browser.transcript.HistoryFirstMessage() == browser.page.first_turn * 2 &&
              browser.transcript.Item(0)->text == "Prompt 196" && browser.transcript.Count() == 30,
          "seeking to the middle renders only its ten-turn neighborhood using absolute indices");
    browser.Reset();
    Check(browser.transcript.Count() == 0 && browser.Count() == 0 && !browser.PreviewReady() &&
              !browser.UpdatePreview(1000), "switching chats discards the loaded page and pending preview");
}

void TestHistoryWindows() {
    std::vector<ChatMessage> messages;
    for (size_t turn = 0; turn < 20; ++turn) {
        ChatMessage user;
        user.role = MessageRole::User;
        user.content = "question " + std::to_string(turn);
        messages.push_back(user);
        ChatMessage assistant;
        assistant.role = MessageRole::Assistant;
        assistant.content = "answer " + std::to_string(turn);
        messages.push_back(assistant);
    }
    const auto recent = SelectHistoryWindow(messages);
    const auto older = SelectHistoryWindow(messages, recent.first_message);
    const auto oldest = SelectHistoryWindow(messages, older.first_message);
    Check(recent.first_message == 24 && recent.first_turn == 12 && recent.total_turns == 20 &&
              older.first_message == 8 && older.first_turn == 4 && older.total_turns == 20 &&
              oldest.first_message == 0 && oldest.first_turn == 0 && oldest.total_turns == 20,
          "history selects the last eight turns and adds eight older turns without trimming the tail");
    Check(SelectHistoryWindow(messages, 0).first_message == 0 &&
              SelectHistoryWindow(messages, messages.size() + 100).first_message == recent.first_message,
          "history cursors clamp safely at both ends");
    const auto empty = SelectHistoryWindow({});
    Check(empty.first_message == 0 && empty.first_turn == 0 && empty.total_turns == 0,
          "empty history has an empty absolute window");
    ChatMessage preamble;
    preamble.role = MessageRole::Assistant;
    const auto no_users = SelectHistoryWindow({preamble});
    Check(no_users.first_message == 0 && no_users.total_turns == 0,
          "history without a user retains its available messages");
    auto prefixed = messages;
    prefixed.insert(prefixed.begin(), preamble);
    const auto first_user = SelectHistoryWindow(prefixed, 3, 8);
    Check(first_user.first_message == 1 && first_user.first_turn == 0 && first_user.total_turns == 20,
          "the oldest partial page starts at its first user rather than a non-user preamble");
    messages.erase(messages.begin() + 1, messages.end());
    ChatMessage filler;
    filler.role = MessageRole::Assistant;
    messages.resize(1000, filler);
    const auto long_turn = SelectHistoryWindow(messages);
    Check(long_turn.first_message == 0 && long_turn.total_turns == 1,
          "one long turn is retained completely rather than cut at an item limit");

    SessionState state;
    state.messages = {messages.front(), messages.back(), messages.front(), messages.back()};
    Transcript transcript;
    auto generation = transcript.BeginReplay(state, {500, 2000}, {}, 3);
    while (!transcript.ReplayStep(generation, 1)) {}
    Check(transcript.HistoryFirstMessage() == 2 && transcript.LastUserIndex() == 0,
          "replay aligns an interior message backward to its owning user boundary");
    generation = transcript.BeginReplay(state, {500, 2000}, {}, SIZE_MAX);
    while (!transcript.ReplayStep(generation)) {}
    Check(transcript.HistoryFirstMessage() == 2,
          "out-of-range replay starts retain the final complete turn");
    generation = transcript.BeginReplay(state, {500, 2000});
    while (!transcript.ReplayStep(generation)) {}
    Check(transcript.HistoryFirstMessage() == 0 && transcript.LastUserIndex() == 2,
          "existing replay calls still restore all turns");
    transcript.Clear();
    Check(transcript.HistoryFirstMessage() == 0, "clearing resets the saved history origin");
}

void TestWindowReplayMetadata() {
    SessionState state;
    state.metadata["turn_durations_ms"] = nlohmann::json::array();
    state.metadata["turn_usages"] = nlohmann::json::array();
    for (size_t turn = 0; turn < 12; ++turn) {
        ChatMessage user;
        user.role = MessageRole::User;
        user.content = "question " + std::to_string(turn);
        state.messages.push_back(user);
        ChatMessage assistant;
        assistant.role = MessageRole::Assistant;
        assistant.content = "answer " + std::to_string(turn);
        state.messages.push_back(assistant);
        state.metadata["turn_durations_ms"].push_back(uint64_t{100} + turn);
        state.metadata["turn_usages"].push_back({{"input_tokens", uint64_t{1000} + turn}});
    }
    state.messages.back().tool_calls = {
        {"done", "read_file", "{}"}, {"pending", "run_command", "{}"}};
    ChatMessage result;
    result.role = MessageRole::Tool;
    result.tool_call_id = "done";
    result.content = "full saved output";
    state.messages.push_back(result);
    state.context_messages = {state.messages.front()};
    state.context_messages.front().content = "model context remains complete";
    state.metadata["turn_errors"] = nlohmann::json::array({
        {{"turn_index", 2}, {"error", "outside window"}},
        {{"turn_index", 7}, {"error", "absolute turn seven"}}});
    state.metadata["context_compactions"] = nlohmann::json::array({
        {{"message_count", 6}, {"timestamp", uint64_t{6}}},
        {{"message_count", 8}, {"timestamp", uint64_t{8}}, {"trigger", "manual"}},
        {{"message_count", 10}, {"timestamp", uint64_t{10}}},
        {{"message_count", 25}, {"timestamp", uint64_t{25}}}});
    state.metadata["live_turn"] = {
        {"active", true}, {"turn_index", 11}, {"started_tick", 1000},
        {"elapsed_ms", uint64_t{50}}, {"reasoning", "active summary"},
        {"tools", nlohmann::json::array({
            {{"tool_call_id", "done"}, {"finished", true}, {"success", false},
             {"output", "short live output"}, {"elapsed_ms", uint64_t{17}}},
            {{"tool_call_id", "pending"}, {"started_tick", 1200}, {"finished", false}}
        })}};
    const auto metadata = state.metadata;
    const auto window = SelectHistoryWindow(state.messages);
    Transcript transcript;
    const auto generation = transcript.BeginReplay(state, {500, 2000}, {}, window.first_message);
    state.messages[window.first_message].content = "changed after snapshot";
    while (!transcript.ReplayStep(generation, 1)) {}
    std::vector<uint64_t> times, usages, compactions;
    std::vector<std::string> users, errors;
    size_t live_status = kNoChatItem, done = kNoChatItem, pending = kNoChatItem;
    for (size_t index = 0; index < transcript.Count(); ++index) {
        const auto& item = *transcript.Item(index);
        if (item.kind == ChatItemKind::User) users.push_back(item.text);
        if (item.kind == ChatItemKind::TurnStatus) {
            if (item.finished) times.push_back(item.elapsed_ms);
            else live_status = index;
        }
        if (item.kind == ChatItemKind::UsageStats) usages.push_back(item.input_tokens);
        if (item.kind == ChatItemKind::Compaction) compactions.push_back(item.timestamp);
        if (item.kind == ChatItemKind::Error) errors.push_back(item.text);
        if (item.kind == ChatItemKind::Tool && item.call_id == "done") done = index;
        if (item.kind == ChatItemKind::Tool && item.call_id == "pending") pending = index;
    }
    Check(users.size() == 8 && users.front() == "question 4" && users.back() == "question 11" &&
              transcript.HistoryFirstMessage() == 8,
          "window replay owns only the requested complete tail with its absolute origin");
    Check(times == std::vector<uint64_t>({104, 105, 106, 107, 108, 109, 110}) &&
              usages == std::vector<uint64_t>({1004, 1005, 1006, 1007, 1008, 1009, 1010}) &&
              errors == std::vector<std::string>({"absolute turn seven"}),
          "window metadata uses absolute turn indices for durations, usage and errors");
    Check(compactions == std::vector<uint64_t>({8, 10, 25}),
          "absolute compaction boundaries retain the origin and final boundary but omit older events");
    Check(live_status != kNoChatItem && done != kNoChatItem && pending != kNoChatItem &&
              transcript.Item(live_status)->start_tick_ms == 1000 &&
              Transcript::Elapsed(*transcript.Item(live_status), 2200) == 1250 &&
              transcript.Item(done)->finished && !transcript.Item(done)->success &&
              transcript.Item(done)->elapsed_ms == 17 &&
              transcript.Item(done)->result == "full saved output" &&
              !transcript.Item(pending)->finished && transcript.Item(pending)->start_tick_ms == 1200,
          "absolute final-turn identity restores live clocks and tools without duplicating saved output");
    if (pending != kNoChatItem) {
        ChatItem completion;
        completion.result = "finished after replay";
        transcript.Apply({TranscriptEventKind::Finish, pending, completion}, {501, 2300});
        Check(transcript.Item(pending)->finished &&
                  transcript.Item(pending)->result == "finished after replay" &&
                  transcript.HistoryFirstMessage() == 8,
              "live event indices still address the restored tail after window replay");
    }
    Check(state.messages.size() == 25 && state.context_messages.size() == 1 &&
              state.context_messages.front().content == "model context remains complete" &&
              state.metadata == metadata,
          "window replay leaves the journal messages, model context and metadata intact");
    auto current = transcript.BeginReplay(state, {502, 3000}, {true, 900}, window.first_message);
    while (!transcript.ReplayStep(current)) {}
    const auto final_status = transcript.LastUserIndex() + 1;
    Check(!transcript.Item(final_status)->finished && transcript.Item(final_status)->start_tick_ms == 900 &&
              Transcript::Elapsed(*transcript.Item(final_status), 3200) == 2350,
          "an explicit run clock overrides saved live timing at the absolute final window turn");
    state.metadata["live_turn"]["active"] = false;
    state.metadata["paused_turn"] = {{"elapsed_ms", uint64_t{555}}};
    state.metadata["context_compactions"].back()["trigger"] = "manual";
    current = transcript.BeginReplay(state, {503, 4000}, {}, window.first_message);
    while (!transcript.ReplayStep(current, 1)) {}
    const auto paused_status = transcript.LastUserIndex() + 1;
    const auto tail_notice = std::find_if(transcript.Items().begin(), transcript.Items().end(),
        [](const ChatItem& item) { return item.kind == ChatItemKind::Compaction && item.timestamp == 25; });
    Check(transcript.Item(paused_status)->paused && transcript.Item(paused_status)->elapsed_ms == 555 &&
              tail_notice != transcript.Items().end() && tail_notice->visible,
          "a window tail manual compaction finishes only the absolute final paused turn");
}

void TestWhitespaceBetweenTools() {
    SessionState state;
    ChatMessage user;
    user.role = MessageRole::User;
    user.content = "Continue work";
    state.messages.push_back(user);
    for (int i = 0; i < 4; ++i) {
        ChatMessage assistant;
        assistant.role = MessageRole::Assistant;
        assistant.content = i == 2 ? "Actual progress update" : " \n\t";
        ToolCall call;
        call.id = std::to_string(i);
        call.name = "run_command";
        call.arguments_json = "{}";
        assistant.tool_calls.push_back(call);
        state.messages.push_back(assistant);
        ChatMessage result;
        result.role = MessageRole::Tool;
        result.tool_call_id = call.id;
        result.content = "done";
        state.messages.push_back(result);
    }
    Transcript transcript;
    const auto generation = transcript.BeginReplay(state, {500, 2000}, {true, 900});
    while (!transcript.ReplayStep(generation, 1)) {}
    const auto restored_groups = transcript.Groups(2000);
    Check(transcript.Count() == 7 && restored_groups[2].size == 2 && restored_groups[3].hidden &&
              transcript.Item(4)->text == "Actual progress update" &&
              restored_groups[5].size == 2 && restored_groups[6].hidden,
          "restoring a running turn ignores blank assistant text but keeps real progress boundaries");

    Transcript live;
    ChatItem tool;
    tool.kind = ChatItemKind::Tool;
    tool.tool_name = "run_command";
    const auto first = live.Apply({TranscriptEventKind::Add, kNoChatItem, tool}, {500, 2000});
    const auto blank = Add(live, ChatItemKind::Assistant, "");
    ChatItem delta;
    delta.text = "\n ";
    live.Apply({TranscriptEventKind::AppendText, blank, delta}, {500, 2000});
    live.Apply({TranscriptEventKind::Finish, blank, {}}, {500, 2000});
    const auto second = live.Apply({TranscriptEventKind::Add, kNoChatItem, tool}, {500, 2000});
    const auto live_groups = live.Groups(2000);
    Check(live_groups[first].size == 2 && live_groups[second].hidden,
          "streamed whitespace does not split a live command group");
}

void TestMixedToolGroups() {
    Transcript transcript;
    std::vector<size_t> indices;
    for (const char* name : {"read_file", "generate_image", "write_file", "custom_alpha",
                             "custom_beta", "search_text", "run_command"}) {
        if (!indices.empty()) Add(transcript, ChatItemKind::Reasoning, "transparent summary");
        ChatItem item;
        item.kind = ChatItemKind::Tool;
        item.tool_name = name;
        indices.push_back(transcript.Apply({TranscriptEventKind::Add, kNoChatItem, item}, {123, 1000}));
    }
    ChatItem failure;
    failure.success = false;
    failure.result = "individual tool failure";
    transcript.Apply({TranscriptEventKind::Finish, indices[1], failure}, {124, 1200});
    auto groups = transcript.Groups(1500);
    Check(groups[0].leader && groups[0].size == 7 && groups[0].mixed &&
              groups[0].read_count == 1 && groups[0].search_count == 1 &&
              groups[0].command_count == 1 && groups[0].active && groups[0].failed &&
              groups[indices.back()].hidden,
          "mixed command, image, write and custom tools share one neutral activity group");
    transcript.ToggleToolGroup(0);
    transcript.ToggleTool(indices[1]);
    Check(!transcript.Groups(1500)[indices.back()].hidden && transcript.Item(indices[1])->expanded &&
              !transcript.Item(indices[1])->success &&
              transcript.Item(indices[1])->result == "individual tool failure",
          "mixed group expansion preserves individual tool failures and output");
    Add(transcript, ChatItemKind::Assistant, "visible boundary");
    ChatItem item;
    item.kind = ChatItemKind::Tool;
    item.tool_name = "read_file";
    const auto next = transcript.Apply({TranscriptEventKind::Add, kNoChatItem, item}, {124, 1500});
    item.tool_name = "search_text";
    transcript.Apply({TranscriptEventKind::Add, kNoChatItem, item}, {124, 1500});
    groups = transcript.Groups(1600);
    Check(groups[0].size == 7 && groups[next].leader && groups[next].size == 2 && !groups[next].mixed,
          "assistant text remains a boundary between mixed tool groups");
}

void TestReasoningActivityGroups() {
    Transcript transcript;
    const auto initial = Add(transcript, ChatItemKind::Reasoning, "initial summary");
    Check(!transcript.Groups(1500)[initial].hidden,
          "initial reasoning without a preceding tool keeps its independent activity row");
    transcript.Apply({TranscriptEventKind::Hide, initial, {}}, {123, 1000});
    ChatItem tool;
    tool.kind = ChatItemKind::Tool;
    tool.tool_name = "read_file";
    const auto first = transcript.Apply({TranscriptEventKind::Add, kNoChatItem, tool}, {123, 1000});
    transcript.Apply({TranscriptEventKind::Finish, first, {}}, {124, 1200});
    const auto summary = Add(transcript, ChatItemKind::Reasoning, "latest summary");
    auto groups = transcript.Groups(1500);
    Check(groups[first].leader && groups[first].size == 1 && groups[first].reasoning == summary &&
              groups[first].active && groups[summary].hidden,
          "live reasoning takes over the single preceding tool header without adding a second row");
    const auto count = transcript.Count();
    ChatItem delta;
    delta.text = " updated";
    transcript.Apply({TranscriptEventKind::AppendText, summary, delta}, {124, 1300});
    groups = transcript.Groups(1500);
    Check(transcript.Count() == count && groups[first].reasoning == summary &&
              transcript.Item(summary)->text == "latest summary updated" && groups[summary].hidden,
          "reasoning deltas refresh the existing shared activity header without adding rows");
    const auto newer = Add(transcript, ChatItemKind::Reasoning, "newer summary");
    groups = transcript.Groups(1500);
    Check(groups[first].reasoning == newer && groups[summary].hidden && groups[newer].hidden,
          "one activity header points at the newest live reasoning while hiding older live rows");
    transcript.Apply({TranscriptEventKind::Hide, summary, {}}, {124, 1200});
    transcript.Apply({TranscriptEventKind::Hide, newer, {}}, {124, 1200});
    tool.tool_name = "run_command";
    const auto second = transcript.Apply({TranscriptEventKind::Add, kNoChatItem, tool}, {124, 1200});
    groups = transcript.Groups(1500);
    Check(groups[first].leader && groups[first].size == 2 && groups[first].reasoning == kNoChatItem &&
              groups[first].active && groups[second].hidden && !groups[first].mixed,
          "tool start after hiding reasoning restores the live command group header");
    transcript.Apply({TranscriptEventKind::Finish, second, {}}, {125, 1400});
    groups = transcript.Groups(1500);
    Check(!groups[first].active && groups[first].reasoning == kNoChatItem,
          "hidden reasoning cannot keep a completed tool group active");
    const auto finished = Add(transcript, ChatItemKind::Reasoning, "finished summary");
    transcript.Apply({TranscriptEventKind::Finish, finished, {}}, {125, 1400});
    groups = transcript.Groups(1500);
    Check(!groups[first].active && groups[first].reasoning == kNoChatItem,
          "finished reasoning cannot reactivate an earlier tool group");
    Add(transcript, ChatItemKind::Assistant, "assistant boundary");
    const auto after_assistant = Add(transcript, ChatItemKind::Reasoning, "separate summary");
    groups = transcript.Groups(1500);
    Check(!groups[after_assistant].hidden && groups[first].reasoning == kNoChatItem,
          "assistant text prevents reasoning from joining an earlier tool group");
    Add(transcript, ChatItemKind::User, "next turn");
    const auto after_user = Add(transcript, ChatItemKind::Reasoning, "next turn summary");
    groups = transcript.Groups(1500);
    Check(!groups[after_user].hidden && groups[first].reasoning == kNoChatItem,
          "a new user boundary keeps reasoning independent of the previous turn tools");
    Transcript notices;
    const auto before_notice = notices.Apply({TranscriptEventKind::Add, kNoChatItem, tool}, {123, 1000});
    notices.Apply({TranscriptEventKind::Finish, before_notice, {}}, {124, 1200});
    const auto notice = Add(notices, ChatItemKind::Compaction);
    const auto after_notice = Add(notices, ChatItemKind::Reasoning, "summary after notice");
    const auto notice_groups = notices.Groups(1500);
    Check(notice_groups[before_notice].reasoning == after_notice &&
              notice_groups[after_notice].hidden && notices.Item(notice)->visible,
          "a compaction notice keeps its own row without splitting the reasoning activity header");
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
        TestEditedImageReplay(root);
        TestCompactionActivity();
        TestResumeReplay(root);
        TestJournalReplay(root);
        TestTurnBoundaries();
        TestCompactionFolding();
        TestCompactionReplayBoundaries();
        TestInvalidCompactionBoundaries();
        TestLiveReplay(root);
        TestLiveTurnIdentity();
        TestHistoryWindows();
        TestIndexedHistoryNavigation();
        TestWindowReplayMetadata();
        TestWhitespaceBetweenTools();
        TestMixedToolGroups();
        TestReasoningActivityGroups();
        TestHistory(root);
    } catch (const std::exception& exception) {
        Check(false, exception.what());
    }
    std::error_code error;
    spark::fs::remove_all(root, error);
    if (failures == 0) std::cout << "PASS: transcript events, grouping, replay, clocks and history\n";
    return failures == 0 ? 0 : 1;
}
