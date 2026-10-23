#include "core/common.h"
#include "core/config.h"
#include "core/context_compactor.h"
#include "core/skills.h"
#include "platform/user_directories.h"

#include <chrono>
#include <fstream>
#include <iostream>

namespace {
int failures = 0;
void Check(bool condition, const char* description) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}
}  // namespace

int main() {
    namespace fs = spark::fs;
    using namespace spark;
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / fs::u8path(u8"Spark-\u6d4b\u8bd5-" + std::to_string(suffix));
    fs::create_directories(root);
    try {
        const std::string path = PathToUtf8(root / fs::u8path(u8"\u914d\u7f6e.txt"));
        const std::string contents = u8"Hello \u4e16\u754c \U0001f680\n";
        Check(WideToUtf8(Utf8ToWide(contents)) == contents, "Unicode round trip includes non-BMP characters");
        Check(!IsValidUtf8("\xc0\xaf") && !IsValidUtf8("\xed\xa0\x80") && !IsValidUtf8("\xf4\x90\x80\x80"),
              "invalid, surrogate and out-of-range UTF-8 is rejected");
        Check(PathToUtf8(NativePath(path)) == path, "native paths preserve UTF-8 independent of locale");
        std::string error;
        Check(WriteFileUtf8Atomic(path, contents, &error), "atomic write accepts a Unicode path");
        Check(ReadFileUtf8(path).value_or("") == contents, "Unicode file read matches the write");
        Check(WriteFileUtf8Atomic(path, "updated", &error), "atomic replacement succeeds");
        Check(ReadFileUtf8(path).value_or("") == "updated", "replacement is visible");
        Check(WriteFileUtf8Atomic(path, std::string("a\0b", 3), &error), "binary NUL is written without truncation");
        Check(ReadFileUtf8(path).value_or("") == std::string("a\0b", 3), "binary NUL survives a file read");
        Check(WriteFileUtf8Atomic(path, "\xef\xbb\xbf" + contents, &error) &&
                  ReadFileUtf8(path).value_or("") == contents, "UTF-8 BOM is stripped only when reading");
        ChatMessage attachment;
        attachment.role = MessageRole::User;
        attachment.image_paths = {path};
        const auto small_image_tokens = EstimateMessageTokens(attachment);
        Check(WriteFileUtf8Atomic(path, std::string(30000, 'x'), &error), "grow the Unicode attachment fixture");
        Check(EstimateMessageTokens(attachment) > small_image_tokens + 9000,
              "context estimation reads attachment sizes through native Unicode paths");
        Check(!WriteFileUtf8Atomic(PathToUtf8(root), "must not replace a directory", &error) &&
                  fs::is_directory(root), "failed replacement preserves an existing directory");
        Check(!WriteFileUtf8Atomic(PathToUtf8(root / "missing" / "child"), "no implicit folders", &error),
              "missing-parent write returns an error");

        const auto big = root / "oversized";
        {
            spark::fs_ofstream output(big, std::ios::binary);
            output.seekp(64 * 1024 * 1024);
            output.put('x');
        }
        Check(!ReadFileUtf8(PathToUtf8(big), &error).has_value(), "file reads enforce the 64 MiB cap");
        Check(IsPathWithin(PathToUtf8(root / "child"), PathToUtf8(root)), "child containment works");
        Check(!IsPathWithin(PathToUtf8(root) + "-sibling", PathToUtf8(root)), "prefix siblings are not children");
        Check(IsPathWithin(PathToUtf8(root), PathToUtf8(root.root_path())), "filesystem root contains its descendants");
#ifndef _WIN32
        Check(NormalizePathForComparison("/tmp/Case") != NormalizePathForComparison("/tmp/case"),
              "POSIX paths are not silently lowercased");
        fs::create_directory(root / "work");
        fs::create_directory(root / "outside");
        fs::create_directory_symlink(root / "outside", root / "work" / "escape");
        Check(!IsPathWithin(PathToUtf8(root / "work" / "escape" / "future"), PathToUtf8(root / "work")),
              "symlink escape remains blocked for not-yet-created files");
        Check(CanonicalPathForAccess(path + "/../child").empty() && CanonicalPathForAccess(path + "/").empty(),
              "existing regular files cannot be traversed as directories");
        const auto outside_file = PathToUtf8(root / "outside" / "original");
        Check(WriteFileUtf8Atomic(outside_file, "original"), "write the symlink replacement fixture");
        fs::create_symlink(NativePath(outside_file), root / "config-link");
        Check(WriteFileUtf8Atomic(PathToUtf8(root / "config-link"), "replacement") &&
                  !fs::is_symlink(root / "config-link") && ReadFileUtf8(outside_file).value_or("") == "original" &&
                  ReadFileUtf8(PathToUtf8(root / "config-link")).value_or("") == "replacement",
              "atomic replacement replaces the selected symlink without modifying its target");
#endif
        const auto config_path = PathToUtf8(root / "config.json");
        auto config = ConfigFromJson({
            {"active", {{"provider", "test"}, {"model", "test-model"}}},
            {"providers", {{"test", {{"protocol", "openai_responses"}, {"base_url", "https://example.invalid/v1"},
                                    {"api_key", "fixture-not-a-real-key"},
                                    {"models", {{{"id", "test-model"}}}}}}}},
            {"storage", {{"data_dir", "./data"}}}
        }, config_path);
        Check(config.workspace.path == JoinPath(UserDirectory(UserDirectoryKind::Documents), "Spark"),
              "no-project output resolves under the platform documents directory");
#ifdef _WIN32
        Check(config.ui.shell == "cmd.exe", "omitted shell retains the Windows command interpreter");
#else
        Check(config.ui.shell == "/bin/sh", "omitted shell resolves to a native POSIX command interpreter");
#endif
        auto explicit_shell = ConfigToJson(config);
        explicit_shell["ui"]["shell"] = "custom-shell";
        Check(ConfigFromJson(explicit_shell, config_path).ui.shell == "custom-shell",
              "an explicitly configured shell is never overwritten by the platform default");
        Check(ConfigToJson(config)["workspace"]["path"] == "${DOCUMENTS}/Spark",
              "saved default workspace remains portable");
        Check(PortableConfigPath(PathToUtf8(root / fs::u8path(u8"\u8d44\u6599")), config_path, false) ==
                  u8"./\u8d44\u6599", "relative config paths preserve Unicode");
        Check(SaveConfigAtomically(config, config_path).ok, "configuration saves on the native filesystem");
        const auto loaded = LoadConfig(config_path, false);
        Check(loaded.ok && ConfigToJson(loaded.value) == ConfigToJson(config),
              "provider and path configuration round-trips across native I/O");
        config.workspace.path = PathToUtf8(root);
        config.skills.include_user_root = false;
        const auto skill = root / ".spark" / "skills" / fs::u8path(u8"\u5ba1\u67e5");
        fs::create_directories(skill);
        Check(WriteFileUtf8Atomic(PathToUtf8(skill / "SKILL.md"),
                                 "---\nname: fixture-review\ndescription: Local fixture\n---\nRead only.\n"),
              "local skill fixture is written");
        const auto skills = DiscoverSkills(config);
        Check(skills.size() == 1 && skills.front().name == "fixture-review" &&
                  NormalizePathForComparison(skills.front().directory) == NormalizePathForComparison(PathToUtf8(skill)),
              "skill discovery uses native Unicode paths");
        Check(PathExists(GetExecutablePath()) && IsDirectory(GetExecutableDirectory()),
              "executable discovery reports an existing binary and directory");
        Check(!IsoUtcTime().empty(), "portable UTC timestamp formatting succeeds");
    } catch (const std::exception& exception) {
        Check(false, exception.what());
    }
    fs::remove_all(root);
    if (failures == 0) std::cout << "PASS: native platform, Unicode, paths, atomic I/O, config and Skills\n";
    return failures == 0 ? 0 : 1;
}
