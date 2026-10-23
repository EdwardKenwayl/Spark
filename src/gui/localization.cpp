#include "gui/localization.h"

#include "core/common.h"
#include "nlohmann/json.hpp"

#ifdef _WIN32
#include "platform/windows_compat.h"
#endif

#include <array>
#include <atomic>
#include <cwchar>
#include <iterator>
#include <map>
#include <mutex>
#ifndef _WIN32
#include "spark_locale_resources.h"
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif
#endif

namespace spark::i18n {
namespace {

constexpr Locale kLocales[]{
    {"zh-CN", L"简体中文"}, {"en-US", L"English"}, {"zh-TW", L"繁體中文"},
    {"ja-JP", L"日本語"}, {"ko-KR", L"한국어"}, {"de-DE", L"Deutsch"},
    {"fr-FR", L"Français"}, {"es-ES", L"Español"}, {"pt-BR", L"Português (Brasil)"},
    {"ru-RU", L"Русский"},
};
constexpr size_t kLocaleCount = std::size(kLocales);
std::atomic<size_t> current_index{0};

struct Catalog {
    std::map<std::wstring, std::array<std::wstring, kLocaleCount>, std::less<>> messages;

    Catalog() {
#ifdef _WIN32
        for (int id : {301, 302, 303, 304}) {
            const HMODULE module = GetModuleHandleW(nullptr);
            const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
            if (!resource) continue;
            const HGLOBAL handle = LoadResource(module, resource);
            const char* bytes = static_cast<const char*>(LockResource(handle));
            if (!bytes) continue;
            Add(nlohmann::json::parse(bytes, bytes + SizeofResource(module, resource), nullptr, false));
        }
#else
        for (const char* text : kSparkLocaleResources)
            Add(nlohmann::json::parse(text, nullptr, false));
#endif
    }

    void Add(const nlohmann::json& document) {
            if (!document.is_object()) return;
            for (auto entry = document.begin(); entry != document.end(); ++entry) {
                if (!entry.value().is_object()) continue;
                const std::wstring key = Utf8ToWide(entry.key());
                auto& translations = messages[key];
                for (size_t index = 0; index < kLocaleCount; ++index) {
                    const auto value = entry.value().find(SupportedLocales()[index].code);
                    if (value != entry.value().end() && value->is_string()) {
                        translations[index] = Utf8ToWide(value->get<std::string>());
                    }
                }
            }
    }
};

const Catalog& Translations() {
    static const Catalog catalog;
    return catalog;
}

}  // namespace

const std::vector<Locale>& SupportedLocales() {
    static const std::vector<Locale> locales(std::begin(kLocales), std::end(kLocales));
    return locales;
}

std::string DetectedSystemLocale() {
#ifdef _WIN32
    return WideToUtf8(windows_compat::LocaleName());
#else
#ifdef __APPLE__
    if (CFArrayRef languages = CFLocaleCopyPreferredLanguages()) {
        char buffer[128]{};
        bool converted = false;
        if (CFArrayGetCount(languages) > 0)
            converted = CFStringGetCString(static_cast<CFStringRef>(CFArrayGetValueAtIndex(languages, 0)),
                                            buffer, sizeof(buffer), kCFStringEncodingUTF8);
        CFRelease(languages);
        if (converted) return buffer;
    }
#endif
    for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        std::string value = EnvironmentVariable(name);
        if (value.empty()) continue;
        const size_t suffix = value.find_first_of(".@");
        if (suffix != std::string::npos) value.resize(suffix);
        return value;
    }
    return {};
#endif
}

void SetLocale(std::string_view code) {
    std::string normalized(code);
    for (char& character : normalized) {
        if (character == '_') character = '-';
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    }
    if (normalized == "system") {
        normalized = LowerAscii(DetectedSystemLocale());
    }
    size_t selected = 1;
    for (size_t index = 0; index < SupportedLocales().size(); ++index) {
        if (LowerAscii(SupportedLocales()[index].code) == normalized) {
            selected = index;
            break;
        }
        const std::string prefix = std::string(SupportedLocales()[index].code).substr(0, 2);
        if (normalized == prefix || normalized.rfind(prefix + "-", 0) == 0) selected = index;
    }
    if (normalized.rfind("zh", 0) == 0) selected = 0;
    if (normalized == "zh-tw" || normalized == "zh-hk" ||
        normalized == "zh-hant" || normalized.rfind("zh-hant-", 0) == 0) selected = 2;
    current_index.store(selected, std::memory_order_relaxed);
}

const std::string& CurrentLocale() {
    static const auto codes = [] {
        std::array<std::string, kLocaleCount> result;
        for (size_t index = 0; index < kLocaleCount; ++index) result[index] = kLocales[index].code;
        return result;
    }();
    return codes[current_index.load(std::memory_order_relaxed)];
}

const wchar_t* Text(std::wstring_view message) {
    const auto& catalog = Translations().messages;
    const auto entry = catalog.find(message);
    if (entry != catalog.end()) {
        const auto& translated = entry->second[current_index.load(std::memory_order_relaxed)];
        if (!translated.empty()) return translated.c_str();
        if (!entry->second[1].empty()) return entry->second[1].c_str();
        return entry->first.c_str();
    }
    // A missing entry must still own its English fallback, including dynamic callers.
    static std::mutex missing_mutex;
    static std::map<std::wstring, std::wstring, std::less<>> missing;
    std::lock_guard<std::mutex> lock(missing_mutex);
    const auto found = missing.find(message);
    if (found != missing.end()) return found->second.c_str();
    if (missing.size() >= 1024) return L"Translation unavailable";
    return missing.emplace(std::wstring(message), std::wstring(message)).first->second.c_str();
}

std::wstring Format(std::wstring_view message,
                    std::initializer_list<std::pair<std::wstring_view, std::wstring>> arguments) {
    const std::wstring pattern = Text(message);
    std::wstring result;
    result.reserve(pattern.size() + 64);
    size_t cursor = 0;
    while (cursor < pattern.size()) {
        const size_t opening = pattern.find(L'{', cursor);
        if (opening == std::wstring::npos) {
            result.append(pattern, cursor, std::wstring::npos);
            break;
        }
        result.append(pattern, cursor, opening - cursor);
        const size_t closing = pattern.find(L'}', opening + 1);
        if (closing == std::wstring::npos) {
            result.append(pattern, opening, std::wstring::npos);
            break;
        }
        const std::wstring_view key(pattern.data() + opening + 1, closing - opening - 1);
        bool replaced = false;
        for (const auto& argument : arguments) {
            if (argument.first == key) {
                result += argument.second;
                replaced = true;
                break;
            }
        }
        if (!replaced) result.append(pattern, opening, closing - opening + 1);
        cursor = closing + 1;
    }
    return result;
}

}  // namespace spark::i18n
