#pragma once

#include "core/agent_types.h"
#include "core/common.h"

#include <string>
#include <iosfwd>
#include <vector>

namespace spark {

inline constexpr char kInterruptedTurnMarker[] =
    "The user interrupted the previous turn on purpose. Any running tools may have been aborted and may have "
    "partially executed. Wait for the user to resume before continuing.";

struct SessionState {
    std::string id;
    std::string title;
    std::string workspace;
    uint64_t created_at = 0;
    uint64_t updated_at = 0;
    // Complete user-visible transcript. Compaction must never shrink this list.
    std::vector<ChatMessage> messages;
    // Model context after compaction. Empty means the transcript is still the context.
    std::vector<ChatMessage> context_messages;
    nlohmann::json metadata = nlohmann::json::object();
};

const std::vector<ChatMessage>& ModelContextMessages(const SessionState& state);

class SessionJournal {
public:
    SessionJournal() = default;
    SessionJournal(const SessionJournal&) = default;
    SessionJournal& operator=(const SessionJournal&) = default;
    SessionJournal(SessionJournal&&) noexcept = default;
    SessionJournal& operator=(SessionJournal&&) noexcept = default;
    ~SessionJournal();

    Result<void> Open(const std::string& data_directory, const std::string& workspace);
    Result<void> Resume(const std::string& journal_path, const CancellationToken* cancellation = nullptr);
    // Points ListRecent() at a storage root without opening a session. The UI needs
    // this on a cold start, before any conversation has been resumed or created.
    void SetStorageRoot(const std::string& data_directory);
    Result<void> Append(const std::string& event_type, const nlohmann::json& payload);
    Result<void> SaveSnapshot();
    Result<std::vector<std::string>> ListRecent(int limit = 50) const;

    const SessionState& State() const { return state_; }
    SessionState& MutableState() { return state_; }
    const std::string& Path() const { return path_; }

private:
    Result<void> LoadEvents(std::istream& stream, const CancellationToken* cancellation);

    SessionState state_;
    std::string path_;
    std::string data_directory_;
};

std::string NewSessionId();
std::string CreateNoProjectConversationWorkspace(const std::string& base_directory, std::string* error);

}  // namespace spark
