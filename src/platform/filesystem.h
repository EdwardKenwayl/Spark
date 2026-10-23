#pragma once

#if defined(SPARK_WINDOWS_XP)
#include "ghc/filesystem.hpp"
#else
#include <filesystem>
#include <fstream>
#endif

namespace spark {

#if defined(SPARK_WINDOWS_XP)
namespace fs = ghc::filesystem;
using fs_ifstream = ghc::filesystem::ifstream;
using fs_ofstream = ghc::filesystem::ofstream;
using fs_fstream = ghc::filesystem::fstream;
#else
namespace fs = std::filesystem;
using fs_ifstream = std::ifstream;
using fs_ofstream = std::ofstream;
using fs_fstream = std::fstream;
#endif

}  // namespace spark
