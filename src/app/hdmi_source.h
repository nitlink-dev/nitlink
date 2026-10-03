#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace NitLink {

// Identity is display-only. These labels must never be HDR-policy inputs.
inline constexpr std::array<std::pair<std::string_view, std::wstring_view>, 9>
    kManualHdmiSources{{
        {"auto", L""},
        {"ps5", L"PS5"},
        {"ps4", L"PS4"},
        {"switch2", L"Switch 2"},
        {"switch", L"Switch"},
        {"xbox_series", L"Xbox Series X|S"},
        {"xbox_one", L"Xbox One"},
        {"pc", L"PC"},
        {"other", L"Other"},
    }};

constexpr bool IsValidManualHdmiSource(std::string_view value) noexcept {
    for (const auto& [canonical, label] : kManualHdmiSources)
        if (value == canonical) return true;
    return false;
}

constexpr bool IsValidManualHdmiSource(std::wstring_view value) noexcept {
    for (const auto& [canonical, label] : kManualHdmiSources)
        if (value.size() == canonical.size() &&
            std::equal(value.begin(), value.end(), canonical.begin())) return true;
    return false;
}

constexpr std::string_view NormalizeManualHdmiSource(std::string_view value) noexcept {
    return IsValidManualHdmiSource(value) ? value : "auto";
}

inline constexpr size_t kMaxManualHdmiSourceCustomCharacters = 64;

constexpr bool IsHdmiSourceWhitespace(uint32_t c) noexcept {
    return c == 0x20 || c == 0xa0 || c == 0x1680 ||
        (c >= 0x2000 && c <= 0x200a) || c == 0x202f || c == 0x205f ||
        c == 0x3000 || c == 0xfeff;
}

constexpr std::wstring_view TrimManualHdmiSourceCustom(std::wstring_view value) noexcept {
    while (!value.empty() && IsHdmiSourceWhitespace(value.front())) value.remove_prefix(1);
    while (!value.empty() && IsHdmiSourceWhitespace(value.back())) value.remove_suffix(1);
    return value;
}

// Config stores UTF-8. Count Unicode scalars rather than bytes or UTF-16 units,
// reject invalid encodings and all C0/C1 controls, and trim Unicode spaces.
inline std::optional<std::string> NormalizeManualHdmiSourceCustom(std::string_view value) {
    if (value.size() > 4096) return std::nullopt;
    size_t first = value.size(), last = first, characters = 0, trimmedCharacters = 0;
    for (size_t i = 0; i < value.size();) {
        const size_t start = i;
        const auto lead = static_cast<unsigned char>(value[i++]);
        uint32_t c = lead;
        unsigned continuation = 0;
        uint32_t minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) { c &= 0x1f; continuation = 1; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { c &= 0x0f; continuation = 2; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { c &= 0x07; continuation = 3; minimum = 0x10000; }
        else if (lead >= 0x80) return std::nullopt;
        for (unsigned j = 0; j < continuation; ++j) {
            if (i == value.size()) return std::nullopt;
            const auto byte = static_cast<unsigned char>(value[i++]);
            if ((byte & 0xc0) != 0x80) return std::nullopt;
            c = (c << 6) | (byte & 0x3f);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) ||
            c < 0x20 || (c >= 0x7f && c <= 0x9f) || c == 0x2028 || c == 0x2029)
            return std::nullopt;
        const bool space = IsHdmiSourceWhitespace(c);
        if (first == value.size() && !space) first = start;
        if (first != value.size()) ++characters;
        if (!space) { last = i; trimmedCharacters = characters; }
    }
    if (trimmedCharacters > kMaxManualHdmiSourceCustomCharacters) return std::nullopt;
    return first == value.size() ? std::string{} : std::string(value.substr(first, last - first));
}

inline std::optional<std::string> NormalizeManualHdmiSourceCustom(std::wstring_view value) {
    if (value.size() > 4096) return std::nullopt;
    std::string utf8;
    for (size_t i = 0; i < value.size(); ++i) {
        uint32_t c = static_cast<uint32_t>(value[i]);
        if (c >= 0xd800 && c <= 0xdbff) {
            if (++i == value.size() || value[i] < 0xdc00 || value[i] > 0xdfff)
                return std::nullopt;
            c = 0x10000 + ((c - 0xd800) << 10) + (value[i] - 0xdc00);
        } else if ((c >= 0xdc00 && c <= 0xdfff) || c > 0x10ffff) return std::nullopt;
        if (c <= 0x7f) utf8 += static_cast<char>(c);
        else if (c <= 0x7ff) {
            utf8 += static_cast<char>(0xc0 | (c >> 6));
            utf8 += static_cast<char>(0x80 | (c & 0x3f));
        } else if (c <= 0xffff) {
            utf8 += static_cast<char>(0xe0 | (c >> 12));
            utf8 += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
            utf8 += static_cast<char>(0x80 | (c & 0x3f));
        } else {
            utf8 += static_cast<char>(0xf0 | (c >> 18));
            utf8 += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
            utf8 += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
            utf8 += static_cast<char>(0x80 | (c & 0x3f));
        }
    }
    return NormalizeManualHdmiSourceCustom(std::string_view(utf8));
}

constexpr std::wstring_view GetEffectiveHdmiSourceLabel(
    std::string_view manualSource,
    std::wstring_view detectedSource = {},
    std::wstring_view sourceName = {},
    std::wstring_view customSource = {}) noexcept {
    if (manualSource == "other") {
        const auto custom = TrimManualHdmiSourceCustom(customSource);
        return custom.empty() ? std::wstring_view(L"Other") : custom;
    }
    for (const auto& [canonical, label] : kManualHdmiSources)
        if (manualSource == canonical && !label.empty()) return label;
    return !detectedSource.empty() ? detectedSource : sourceName;
}

} // namespace NitLink
