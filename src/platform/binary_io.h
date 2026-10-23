#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace spark {

// Reads the initial size of a nonempty regular file, up to 16 MiB, without decoding.
// Symlinks follow their targets. Open, size and read failures return nullopt.
std::optional<std::string> ReadAttachmentBytes(std::string_view path);

}  // namespace spark
