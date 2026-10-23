#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spark::i18n {

struct Locale {
    const char* code;
    const wchar_t* name;
};

const std::vector<Locale>& SupportedLocales();
// Returns the Windows user-interface locale, or an empty string when Windows
// cannot provide one. The result is normalized by SetLocale before lookup.
std::string DetectedSystemLocale();
void SetLocale(std::string_view code);
const std::string& CurrentLocale();

// English messages are gettext-style keys; translations live in resources/locales.
// Returned pointers remain valid across language changes.
const wchar_t* Text(std::wstring_view message);
std::wstring Format(std::wstring_view message,
                    std::initializer_list<std::pair<std::wstring_view, std::wstring>> arguments);

}  // namespace spark::i18n
