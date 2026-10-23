#pragma once

#include "core/config.h"

#include <string>
#include <vector>

namespace spark {

struct SkillInfo {
    std::string name;
    std::string description;
    std::string short_description;
    std::string version;
    // Absolute path of the declaring SKILL.md and of its containing directory.
    std::string path;
    std::string directory;
    // Where the skill came from: workspace, user or a configured root label.
    std::string source;
    std::vector<std::string> allowed_tools;
};

std::vector<SkillInfo> DiscoverSkills(const AppConfig& config);
std::string SkillsPromptSection(const std::vector<SkillInfo>& skills);
const SkillInfo* FindSkillByName(const std::vector<SkillInfo>& skills, const std::string& name);
// Matches explicit "$skill-name" mentions in a user turn.
std::vector<SkillInfo> SelectExplicitSkills(const std::vector<SkillInfo>& skills, const std::string& user_text);
std::string SkillInvocationContext(const std::vector<SkillInfo>& skills, size_t max_bytes_per_skill = 65536);
// Reads SKILL.md or a resource inside the skill directory. Paths cannot escape it.
Result<std::string> ReadSkillResource(const SkillInfo& skill, const std::string& relative_path,
                                      size_t max_bytes = 262144);
std::string SkillFrontMatterField(const std::string& text, const std::string& key);

}  // namespace spark
