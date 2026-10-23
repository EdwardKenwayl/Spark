#include "core/session.h"

#include <chrono>
#include "platform/filesystem.h"
#include <iostream>
#include <set>

namespace {
int failures = 0;
void Check(bool condition, const char* message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}
}  // namespace

int main() {
    using namespace spark;
    namespace fs = spark::fs;
    const auto root = fs::temp_directory_path() / NativePath(u8"Spark-\u4f1a\u8bdd-" + NewSessionId());
    fs::create_directories(root);
    try {
        SessionJournal journal;
        Check(journal.Open(PathToUtf8(root / "data"), PathToUtf8(root)).ok, "create a journal under a Unicode path");
        ChatMessage user;
        user.role = MessageRole::User;
        user.content = u8"\u4fdd\u7559\u6211\u7684\u65e7\u5bf9\u8bdd \U0001f680";
        user.timestamp = UnixTimeSeconds();
        journal.MutableState().messages.push_back(user);
        Check(journal.Append("message", user).ok, "append a Unicode message");
        Check(journal.Append("turn.completed", {{"turn_index", 0}, {"elapsed_ms", 12345}}).ok,
              "append a completed-turn duration");
        Check(journal.Append("turn.error", {{"turn_index", 0}, {"message", "fixture old error"}}).ok &&
                  journal.Append("turn.error", {{"turn_index", 0}, {"message", "fixture latest error"}}).ok,
              "replace an error for a retried turn");
        Check(journal.Append("context.compacted", {{"summary", "fixture handoff"}, {"before_tokens", 300000},
                                                    {"after_tokens", 5000}, {"message_count", 1}}).ok,
              "append a context compaction event");
        ChatMessage handoff;
        handoff.role = MessageRole::System;
        handoff.content = "Conversation history handoff summary:\nfixture handoff";
        journal.MutableState().context_messages = {handoff};
        Check(journal.SaveSnapshot().ok, "save the portable snapshot format");
        SessionJournal resumed;
        Check(resumed.Resume(journal.Path()).ok, "resume the journal");
        Check(resumed.State().messages.size() == 1 && resumed.State().messages.front().content == user.content &&
                  resumed.State().messages.front().timestamp == user.timestamp,
              "compaction and reload preserve the visible transcript and original timestamp");
        Check(ModelContextMessages(resumed.State()).size() == 1 &&
                  ModelContextMessages(resumed.State()).front().content == handoff.content,
              "compacted model context survives separately");
        Check(resumed.State().metadata["turn_durations_ms"] == nlohmann::json::array({12345}) &&
                  resumed.State().metadata["turn_errors"].size() == 1 &&
                  resumed.State().metadata["turn_errors"][0]["message"] == "fixture latest error",
              "duration and latest error survive restart");
        Check(resumed.State().metadata["context_compactions"].size() == 1,
              "compaction notification survives restart");
        SessionJournal cold_start;
        cold_start.SetStorageRoot(PathToUtf8(root / "data"));
        const auto recent = cold_start.ListRecent(1);
        Check(recent.ok && recent.value == std::vector<std::string>{PathToUtf8(NativePath(journal.Path()).filename())},
              "a cold-start journal lists existing conversation filenames");
        Check(cold_start.ListRecent(0).value.empty(), "a zero limit returns no conversations");
        CancellationToken cancelled;
        cancelled.Cancel();
        SessionJournal abandoned;
        Check(!abandoned.Resume(journal.Path(), &cancelled).ok, "pre-cancelled resume exits immediately");
        std::string error;
        const auto conversation = CreateNoProjectConversationWorkspace(PathToUtf8(root / "outputs"), &error);
        Check(!conversation.empty() && IsPathWithin(conversation, PathToUtf8(root / "outputs")) &&
                  IsDirectory(JoinPath(conversation, "work")) && IsDirectory(JoinPath(conversation, "outputs")),
              "no-project work and outputs use native paths below the chosen output root");
        std::set<std::string> ids;
        for (int index = 0; index < 1000; ++index) ids.insert(NewSessionId());
        Check(ids.size() == 1000, "rapidly generated session IDs remain distinct");
    } catch (const std::exception& exception) {
        Check(false, exception.what());
    }
    fs::remove_all(root);
    if (failures == 0) std::cout << "PASS: native session creation, journal replay, compaction and restart persistence\n";
    return failures == 0 ? 0 : 1;
}
