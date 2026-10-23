#include "core/live_turn.h"
#include "core/agent.h"
#include "platform/clock.h"

#include <algorithm>

namespace spark {
LiveTurn::LiveTurn(SessionJournal* journal, AgentCallbacks& callbacks, bool resume) : journal_(journal) {
    if (!journal_) return;
    const size_t turns = static_cast<size_t>(std::count_if(journal_->State().messages.begin(),
        journal_->State().messages.end(), [](const ChatMessage& message) { return message.role == MessageRole::User; }));
    const auto& metadata = journal_->State().metadata;
    const uint64_t previous = resume ? metadata.value("paused_turn", nlohmann::json::object())
        .value("elapsed_ms", uint64_t{0}) : 0;
    journal_->Append("turn.started", {{"started_tick", MonotonicMillis()}, {"elapsed_ms", previous},
        {"turn_index", resume && turns ? turns - 1 : turns}});
    const auto text = callbacks.on_text_delta;
    callbacks.on_text_delta = [this, text](const std::string& delta) {
        Delta("text", delta);
        if (text) text(delta);
    };
    const auto reasoning = callbacks.on_reasoning_delta;
    callbacks.on_reasoning_delta = [this, reasoning](const std::string& delta) {
        Delta("reasoning", delta);
        if (reasoning) reasoning(delta);
    };
    const auto phase = callbacks.on_phase;
    callbacks.on_phase = [this, phase](AgentPhase value, const std::string& detail) {
        Flush();
        if (value == AgentPhase::WaitingModel && detail != "thinking")
            journal_->Append("turn.live_reset", nlohmann::json::object());
        const char* name = value == AgentPhase::RunningTool ? "running_tool" :
            value == AgentPhase::Compacting ? "compacting" :
            value == AgentPhase::Streaming ? "streaming" : "waiting_model";
        journal_->Append("turn.phase", {{"phase", name}, {"detail", detail}});
        if (phase) phase(value, detail);
    };
    const auto start = callbacks.on_tool_start;
    callbacks.on_tool_start = [this, start](const std::string& id, const std::string& name, const std::string& arguments) {
        Flush();
        tools_[id] = MonotonicMillis();
        journal_->Append("turn.tool_start", {{"tool_call_id", id}, {"tool_name", name},
            {"arguments", arguments}, {"started_tick", tools_[id]}});
        if (start) start(id, name, arguments);
    };
    const auto finish = callbacks.on_tool_finish;
    callbacks.on_tool_finish = [this, finish](const std::string& id, const std::string& name, bool ok,
                                             const std::string& output) {
        Flush();
        const auto found = tools_.find(id);
        journal_->Append("turn.tool_finish", {{"tool_call_id", id}, {"success", ok}, {"output", output},
            {"elapsed_ms", found == tools_.end() ? 0 : MonotonicMillis() - found->second}});
        tools_.erase(id);
        if (finish) finish(id, name, ok, output);
    };
}
void LiveTurn::Delta(const char* field, const std::string& text) {
    (field[0] == 't' ? text_ : reasoning_) += text;
    if (text_.size() + reasoning_.size() >= 4096 || MonotonicMillis() - flushed_ >= 100) Flush();
}
void LiveTurn::Flush() {
    if (!journal_) return;
    if (!text_.empty()) {
        journal_->Append("turn.text_delta", {{"delta", text_}});
        text_.clear();
    }
    if (!reasoning_.empty()) {
        journal_->Append("turn.reasoning_delta", {{"delta", reasoning_}});
        reasoning_.clear();
    }
    flushed_ = MonotonicMillis();
}
LiveTurn::~LiveTurn() {
    if (!journal_) return;
    try {
        Flush();
        journal_->Append("turn.ended", nlohmann::json::object());
    } catch (...) {}
}
}  // namespace spark
