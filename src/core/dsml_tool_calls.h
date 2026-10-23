#pragma once

#include "core/agent_types.h"

#include <string>
#include <string_view>
#include <vector>

namespace spark {

struct DsmlParseResult {
    std::string text;
    std::vector<ToolCall> tool_calls;
    bool found = false;
    bool incomplete = false;
    bool malformed = false;
};

DsmlParseResult ExtractDsmlToolCalls(const std::string& text);

// Keeps DSML tool-call markup out of the live transcript while still allowing
// normal text before and after the block to stream immediately.
class DsmlStreamFilter {
public:
    std::string Push(std::string_view chunk);
    std::string Finish();
    bool incomplete() const { return incomplete_; }

private:
    std::string pending_;
    bool suppressing_ = false;
    bool incomplete_ = false;
};

}  // namespace spark
