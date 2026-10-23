#pragma once

#include <string>

namespace spark {

enum class UserDirectoryKind { Documents, AppData, LocalAppData };

// Native known folders on Windows, XDG directories on Linux, Library on macOS.
std::string UserDirectory(UserDirectoryKind kind);

}  // namespace spark
