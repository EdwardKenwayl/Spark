#pragma once

#include "core/common.h"

class wxWindow;

namespace spark {

Result<std::vector<std::string>> PickNativeAttachments(wxWindow* owner,
                                                       const std::string& initial_directory = {});

}  // namespace spark
