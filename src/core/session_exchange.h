#pragma once

#include "core/common.h"

#include <cstdint>
#include <string>

namespace spark {

struct CodexSessionImportResult {
    std::string path;
    std::string title;
    size_t message_count = 0;
};

// Returns CODEX_HOME/sessions or the user's .codex/sessions directory.
std::string DefaultCodexSessionsRoot();

// Builds the canonical rollout path used by Codex: sessions/YYYY/MM/DD/rollout-<timestamp>-<uuid>.jsonl.
std::string SuggestedCodexRolloutPath(const std::string& codex_sessions_root, uint64_t created_at = 0);

// Converts a Spark session to a canonical Codex rollout path without modifying the source.
Result<void> ExportSparkSessionToCodex(const std::string& spark_session_path,
                                       const std::string& codex_output_path);

// Converts a Codex rollout into a new Spark session. The source is never modified.
Result<CodexSessionImportResult> ImportCodexSessionToSpark(const std::string& codex_session_path,
                                                           const std::string& data_directory,
                                                           const std::string& workspace,
                                                           const std::string& scope);

}  // namespace spark
