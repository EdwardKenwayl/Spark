#include "gui/localization.h"
#include "core/common.h"
#include "core/config.h"
#include "nlohmann/json.hpp"
#ifdef _WIN32
#include "platform/windows_compat.h"
#endif

#include <algorithm>
#include "platform/filesystem.h"
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>

namespace {
int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::set<std::wstring> Placeholders(const std::wstring& text) {
    std::set<std::wstring> names;
    size_t offset = 0;
    while ((offset = text.find(L'{', offset)) != std::wstring::npos) {
        const size_t closing = text.find(L'}', offset + 1);
        if (closing == std::wstring::npos) break;
        names.insert(text.substr(offset + 1, closing - offset - 1));
        offset = closing + 1;
    }
    return names;
}

std::set<std::string> TranslationKeys(const std::string& source, bool native) {
    using namespace spark;
    const std::regex literal(R"ref(\bL"((?:[^"\\]|\\.)*)")ref");
    const std::regex reference(R"ref((?:\bText|\bFormat|\bT|\bF)\(\s*$)ref");
    const std::regex adjacent(R"ref(\s*L"((?:[^"\\]|\\.)*)")ref");
    std::set<std::string> keys;
    size_t consumed = 0;
    for (auto match = std::sregex_iterator(source.begin(), source.end(), literal);
         match != std::sregex_iterator(); ++match) {
        const auto position = static_cast<size_t>(match->position());
        if (position < consumed) continue;
        const size_t prefix_begin = position > 128 ? position - 128 : 0;
        const std::string prefix = source.substr(prefix_begin, position - prefix_begin);
        std::smatch call;
        bool lookup = std::regex_search(prefix, call, reference);
        if (lookup && prefix_begin + static_cast<size_t>(call.position()) > 0) {
            const char preceding = source[prefix_begin + static_cast<size_t>(call.position()) - 1];
            if (preceding == '.' || preceding == '>') lookup = false;
        }
        // Non-UI Windows literals may contain C++ escapes such as \0 that JSON
        // does not accept. Decode only translation calls outside Native.
        if (!native && !lookup) continue;
        std::string encoded = (*match)[1].str();
        size_t end = position + static_cast<size_t>(match->length());
        for (;;) {
            std::smatch next;
            if (!std::regex_search(source.begin() + static_cast<std::ptrdiff_t>(end), source.end(),
                                   next, adjacent, std::regex_constants::match_continuous)) break;
            encoded += next[1].str();
            end += static_cast<size_t>(next.length());
        }
        consumed = end;
        const auto key = nlohmann::json::parse("\"" + encoded + "\"").get<std::string>();
        // Native wrappers also receive literals through arrays, ternaries and
        // helpers such as Label/Count. Lowercase placeholder names and format
        // specifiers are metadata, not translatable UI messages.
        const bool prose = native && !key.empty() && key.front() != '%' &&
            (std::any_of(key.begin(), key.end(), [](unsigned char c) { return c >= 'A' && c <= 'Z'; }) ||
             (key.find(' ') != std::string::npos &&
              std::any_of(key.begin(), key.end(), [](unsigned char c) { return c >= 'a' && c <= 'z'; })));
        if (lookup || prose) keys.insert(key);
    }
    return keys;
}

void TestNativeCoverage() {
    using namespace spark::i18n;
    const std::string sample = R"cpp(
        T(L"Welcome"); F(L"Hello {name}", {});
        Text(L"Direct"); i18n::Format(flag ? L"First" : L"Second", {});
        Label(parent, L"Description");
        const wchar_t* labels[] = {L"Array label"};
        T(L"Adjacent " L"message"); time.Format(L"%H:%M");
        i18n::Format(L"Count {count}", {{L"count", value}});
    )cpp";
    Check(TranslationKeys(sample, true) == std::set<std::string>{
        "Welcome", "Hello {name}", "Direct", "First", "Second", "Description",
        "Array label", "Adjacent message", "Count {count}"},
        "Native scanner covers wrappers, ternaries, arrays and adjacent literals without format metadata");
    for (const auto& locale : SupportedLocales()) {
        SetLocale(locale.code);
        for (const auto* message : {
                L"What do you want to do?",
                L"Choose the default task folder",
                L"Settings saved and applied.",
                L"Cleared data/cache. Sessions and archives were kept."}) {
            Check(locale.code == std::string("en-US") || std::wstring(Text(message)) != message,
                  "Native welcome and General help/status have real translations in every non-English locale");
        }
    }
    SetLocale("ko-KR");
    Check(std::wstring(Text(L"Subagents")) == L"하위 에이전트" &&
              std::wstring(Text(L"Sources")) == L"출처",
          "Korean summary metadata resolves Korean instead of the previous UI language");
    SetLocale("en-US");
}

void TestCatalogCoverage() {
    using namespace spark;
    std::map<std::string, nlohmann::json> messages;
    int id = 300;
    for (const char* name : {"shell", "settings", "chat", "auxiliary"}) {
        ++id;
#ifdef _WIN32
        const HMODULE module = GetModuleHandleW(nullptr);
        const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
        Check(resource != nullptr, "all translation catalogs are embedded in executables");
        if (!resource) continue;
        const char* bytes = static_cast<const char*>(LockResource(LoadResource(module, resource)));
        const auto document = nlohmann::json::parse(bytes, bytes + SizeofResource(module, resource),
                                                   nullptr, false);
#else
        fs_ifstream file(fs::path(SPARK_TEST_SOURCE_DIR) / "resources" / "locales" /
                         (std::string(name) + ".json"), std::ios::binary);
        const auto document = nlohmann::json::parse(file, nullptr, false);
#endif
        Check(document.is_object(), "translation resources are valid JSON catalogs");
        if (!document.is_object()) continue;
        for (auto entry = document.begin(); entry != document.end(); ++entry) {
            Check(entry.value().is_object(), "translation entries are keyed by locale");
            if (!entry.value().is_object()) continue;
            Check(messages.emplace(entry.key(), entry.value()).second, "each message has one canonical catalog entry");
            const auto expected = Placeholders(Utf8ToWide(entry.key()));
            Check(entry.value().value("en-US", std::string{}) == entry.key(), "English text matches its message key");
            for (const auto& locale : i18n::SupportedLocales()) {
                const auto translation = entry.value().find(locale.code);
                if (translation == entry.value().end() || !translation->is_string() ||
                    translation->get<std::string>().empty()) {
                    ++failures;
                    std::cerr << "Missing translation: " << entry.key() << " / " << locale.code << '\n';
                    continue;
                }
                const auto text = Utf8ToWide(translation->get<std::string>());
                if (Placeholders(text) != expected) {
                    ++failures;
                    std::cerr << "Placeholder mismatch: " << entry.key() << " / " << locale.code << '\n';
                }
                i18n::SetLocale(locale.code);
                Check(std::wstring(i18n::Text(Utf8ToWide(entry.key()))) == text,
                      "all translated entries are reachable through the runtime lookup");
            }
        }
    }
    const std::regex wide_literal(R"ref(L"((?:[^"\\]|\\.)*)")ref");
    const auto directory = spark::fs::path(SPARK_TEST_SOURCE_DIR) / "src";
    for (const auto& entry : spark::fs::recursive_directory_iterator(directory)) {
        if (entry.path().extension() != ".cpp" && entry.path().extension() != ".h") continue;
        if (entry.path().filename() == "localization.cpp") continue;
        spark::fs_ifstream file(entry.path(), std::ios::binary);
        const std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>{});
        if (entry.path().parent_path().filename() == "gui") {
            for (auto literal = std::sregex_iterator(source.begin(), source.end(), wide_literal);
                 literal != std::sregex_iterator(); ++literal) {
                const auto value = Utf8ToWide((*literal)[1].str());
                const bool han = std::any_of(value.begin(), value.end(), [](wchar_t character) {
                    return character >= 0x3400 && character <= 0x9fff;
                });
                if (han) {
                    ++failures;
                    std::cerr << "Hardcoded GUI translation: " << entry.path().filename().string() << ": "
                              << (*literal)[1].str() << '\n';
                }
            }
        }
        const bool native = entry.path().parent_path().filename() == "native";
        for (const auto& key : TranslationKeys(source, native)) {
            if (messages.find(key) == messages.end()) {
                ++failures;
                std::cerr << "UI text missing from catalogs: " << entry.path().filename().string() << ": " << key << '\n';
            }
        }
    }
    std::cout << "Checked " << messages.size() << " messages in " << i18n::SupportedLocales().size() << " locales.\n";
}
}  // namespace

int main() {
    using namespace spark::i18n;
    TestNativeCoverage();
    TestCatalogCoverage();
    const auto default_config =
        spark::ConfigFromJson(spark::ParseConfigJson(spark::DefaultConfigExampleJson()), "test-config.json");
    Check(default_config.ui.language == "system" &&
              spark::ConfigToJson(default_config)["ui"]["language"] == "system",
          "new configurations default to automatic system language and preserve that preference");
    const std::string detected_locale = DetectedSystemLocale();
    SetLocale(detected_locale);
    const std::string expected_system_locale = CurrentLocale();
    SetLocale("system");
    Check(CurrentLocale() == expected_system_locale,
          "automatic language selection resolves the current system UI language");
    SetLocale("zh-CN");
    Check(std::wstring(Text(L"Automatic")) == L"自动检测",
          "automatic language selection uses a concise localized label");
    SetLocale("en");
    Check(CurrentLocale() == "en-US" && std::wstring(Text(L"New chat")) == L"New chat",
          "English locale resolves catalog text");
    const wchar_t* previous = Text(L"New chat");
    SetLocale("zh-CN");
    Check(std::wstring(Text(L"New chat")) == L"新对话", "Chinese locale resolves catalog text");
    Check(std::wstring(previous) == L"New chat", "language switching preserves existing text pointers");
    SetLocale("zh_Hant");
    Check(CurrentLocale() == "zh-TW", "script alias selects Traditional Chinese");
    SetLocale("fr-CA");
    Check(CurrentLocale() == "fr-FR", "regional fallback selects the supported French catalog");
    SetLocale("unknown");
    Check(CurrentLocale() == "en-US", "unknown locales fall back to English");
    Check(std::wstring(Text(std::wstring(L"Unknown test message"))) == L"Unknown test message",
          "missing translations own their English fallback");
    for (const auto& locale : SupportedLocales()) {
        SetLocale(locale.code);
        Check(CurrentLocale() == locale.code && !std::wstring(Text(L"New chat")).empty(),
              "all selectable languages resolve catalog text");
        const auto formatted = Format(L"What would you like to do in {project}?",
                                      {{L"project", L"Sample {other} project"}});
        Check(formatted.find(L"Sample {other} project") != std::wstring::npos &&
                  formatted.find(L"{project}") == std::wstring::npos,
              "placeholder substitution preserves user content without recursively interpreting it");
    }
    SetLocale("en-US");
    Check(Format(L"{first} {second}", {{L"first", L"{second}"}, {L"second", L"done"}}) == L"{second} done",
          "replacements are not interpreted as template text");
    Check(Format(L"Missing {value}", {}) == L"Missing {value}", "unknown placeholders remain visible");
    return failures == 0 ? 0 : 1;
}
