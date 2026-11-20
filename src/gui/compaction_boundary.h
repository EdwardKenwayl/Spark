#pragma once

#include <cstdint>
#include <optional>
#include <nlohmann/json.hpp>

namespace spark {

// Both transcript adapters must accept the same saved compaction boundaries.
inline std::optional<uint64_t> CompactionMessageCount(const nlohmann::json& event) {
    const auto boundary = event.find("message_count");
    if (boundary == event.end() || !boundary->is_number_integer() ||
        (!boundary->is_number_unsigned() && boundary->get<int64_t>() < 0)) {
        return std::nullopt;
    }
    return boundary->get<uint64_t>();
}

}  // namespace spark
