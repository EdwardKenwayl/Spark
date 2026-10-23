#pragma once

#include "core/session.h"

namespace spark {
struct AgentCallbacks;

// Batches live previews into the same journal used for completed messages.
// The owning agent calls callbacks and destroys this object on its worker thread.
class LiveTurn {
public:
    LiveTurn(SessionJournal* journal, AgentCallbacks& callbacks, bool resume);
    ~LiveTurn();
private:
    void Flush();
    void Delta(const char* field, const std::string& text);
    SessionJournal* journal_;
    std::string text_, reasoning_;
    uint64_t flushed_ = 0;
    std::map<std::string, uint64_t> tools_;
};
}  // namespace spark
