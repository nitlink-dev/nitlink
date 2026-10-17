#pragma once

#include <initializer_list>
#include <string>
#include <utility>

namespace NitLink {

// User-facing language preference. The effective locale is resolved to a
// concrete locale by Localization::SetPreference; unsupported system locales
// intentionally resolve to en-US.
enum class LanguagePreference {
    System,
    English,
    TraditionalChinese,
    SimplifiedChinese,
    German,
    Korean,
};

class Localization {
public:
    static Localization& Instance();

    void SetPreference(const std::string& value);
    void SetPreference(LanguagePreference preference);

    LanguagePreference Preference() const { return m_preference; }
    const std::string& PreferenceName() const { return m_preferenceName; }
    const std::wstring& LocaleName() const { return m_localeName; }

    // Lookup always falls back to the complete en-US table. Unknown keys
    // return the key itself as a final non-empty diagnostic fallback.
    std::wstring Get(const wchar_t* key) const;

    // Replaces named placeholders such as {width}. The translated template
    // still follows the same per-key en-US fallback rules as Get().
    std::wstring Format(
        const wchar_t* key,
        std::initializer_list<std::pair<std::wstring, std::wstring>> values) const;

    // Native text uses Microsoft YaHei UI for Simplified Chinese and
    // Microsoft JhengHei UI for Traditional Chinese and Malgun Gothic for Korean.
    // English and German use the fallback family. DirectWrite still applies
    // its normal fallback chain for other glyphs.
    const wchar_t* UiFontFamily(const wchar_t* fallback = L"Segoe UI") const;

private:
    Localization();

    static LanguagePreference ParsePreference(const std::string& value);
    static std::wstring DetectSystemLocale();
    static bool IsTraditionalChineseLocale(const std::wstring& locale);
    static bool IsSimplifiedChineseLocale(const std::wstring& locale);
    static bool IsGermanLocale(const std::wstring& locale);
    static bool IsKoreanLocale(const std::wstring& locale);

    LanguagePreference m_preference = LanguagePreference::System;
    std::string m_preferenceName = "system";
    std::wstring m_localeName = L"en-US";
};

} // namespace NitLink
