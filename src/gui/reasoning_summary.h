#pragma once

#include "core/config.h"
#include "gui/localization.h"

#include <algorithm>
#include <array>

namespace spark {

inline constexpr std::array<const char*, 8> kReasoningEfforts{
    "none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra"};

inline std::wstring ReasoningSummary(const ModelConfig& model) {
    const auto efforts = EffectiveReasoningEfforts(model);
    if (efforts.empty())
        return i18n::Text(model.capabilities.reasoning ? L"Auto" : L"Off");
    std::vector<int> positions;
    positions.reserve(efforts.size());
    for (const auto& effort : efforts) {
        const auto found = std::find_if(kReasoningEfforts.begin(), kReasoningEfforts.end(),
            [&](const char* value) { return effort == value; });
        positions.push_back(found == kReasoningEfforts.end()
            ? -1 : static_cast<int>(found - kReasoningEfforts.begin()));
    }
    const bool ordered = std::all_of(positions.begin(), positions.end(),
        [](int position) { return position >= 0; }) &&
        std::is_sorted(positions.begin(), positions.end()) &&
        std::adjacent_find(positions.begin(), positions.end()) == positions.end();
    std::wstring result;
    if (!ordered) {
        for (const auto& effort : efforts) {
            if (!result.empty()) result += L", ";
            result += Utf8ToWide(effort);
        }
        return result;
    }
    size_t first = 0;
    for (size_t end = 1; end <= efforts.size(); ++end) {
        if (end < efforts.size() && positions[end] == positions[end - 1] + 1) continue;
        if (!result.empty()) result += L", ";
        result += Utf8ToWide(efforts[first]);
        if (end - first >= 3) result += L" \u2192 " + Utf8ToWide(efforts[end - 1]);
        else for (size_t index = first + 1; index < end; ++index)
            result += L", " + Utf8ToWide(efforts[index]);
        first = end;
    }
    return result;
}

}  // namespace spark
