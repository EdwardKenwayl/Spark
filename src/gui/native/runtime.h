#pragma once

#include "core/agent.h"
#include "gui/transcript.h"

#include <deque>

namespace spark {

enum class NativeApprovalDecision { Deny, Once, Turn };

struct NativeEvent {
    enum class Kind { Text, Reasoning, ToolStart, ToolFinish, Phase, Context, Compact, Done, Child };
    Kind kind = Kind::Phase;
    std::string path, first, second, third;
    bool success = false;
    uint64_t first_number = 0, second_number = 0;
    AgentPhase phase = AgentPhase::WaitingModel;
    SubAgentInfo child;
};

// UI thread consumes events; workers own journals and never touch widgets.
class NativeRuntime {
public:
    struct Approval {
        uint64_t id = 0;
        std::string path;
        ApprovalRequest request;
    };
    NativeRuntime() = default;
    ~NativeRuntime();
    Result<void> Start(const AppConfig& config, const SessionJournal& journal, const std::string& text,
                       bool resume, const std::vector<std::string>& images, AgentTurnOptions options = {});
    void Pause(const std::string& path);
    bool Running(const std::string& path) const;
    bool AnyRunning() const;
    uint64_t Started(const std::string& path) const;
    std::vector<NativeEvent> Poll();
    std::vector<Approval> Approvals();
    void Approve(uint64_t id, NativeApprovalDecision decision);
    Result<SessionJournal> FinishedJournal(const std::string& path);
    std::vector<SubAgentInfo> Children(const std::string& path) const;
    void Stop();

private:
    struct Run;
    void Emit(NativeEvent event);
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Run>> runs_;
    std::deque<NativeEvent> events_;
    size_t event_bytes_ = 0;
    uint64_t next_approval_ = 0;
};

}  // namespace spark
