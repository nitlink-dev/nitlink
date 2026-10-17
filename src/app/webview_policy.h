#pragma once

#include <string_view>

namespace NitLink::WebViewPolicy {

inline constexpr wchar_t MenuUri[] = L"https://nitlink.invalid/nitlink-menu.html";
inline constexpr wchar_t ContentSecurityPolicy[] =
    L"default-src 'none'; script-src 'self'; style-src 'unsafe-inline'; "
    L"img-src 'self'; font-src 'self'; connect-src 'none'; frame-src 'none'; "
    L"worker-src 'none'; object-src 'none'; base-uri 'none'; form-action 'none'; "
    L"frame-ancestors 'none'";

enum class ResourceKind { Document, Script, Font, Image };
struct Resource {
    std::wstring_view uri;
    const wchar_t* path;
    const wchar_t* contentType;
    ResourceKind kind;
};

// URI paths never become filesystem paths. Only these packaged files are served.
inline constexpr Resource Resources[] = {
    {MenuUri, L"nitlink-menu.html", L"text/html; charset=utf-8", ResourceKind::Document},
    {L"https://nitlink.invalid/locales/en-US.js", L"locales/en-US.js", L"text/javascript; charset=utf-8", ResourceKind::Script},
    {L"https://nitlink.invalid/locales/de-DE.js", L"locales/de-DE.js", L"text/javascript; charset=utf-8", ResourceKind::Script},
    {L"https://nitlink.invalid/locales/ko-KR.js", L"locales/ko-KR.js", L"text/javascript; charset=utf-8", ResourceKind::Script},
    {L"https://nitlink.invalid/locales/zh-CN.js", L"locales/zh-CN.js", L"text/javascript; charset=utf-8", ResourceKind::Script},
    {L"https://nitlink.invalid/locales/zh-TW.js", L"locales/zh-TW.js", L"text/javascript; charset=utf-8", ResourceKind::Script},
    {L"https://nitlink.invalid/assets/menu/menu.js", L"assets/menu/menu.js", L"text/javascript; charset=utf-8", ResourceKind::Script},
    {L"https://nitlink.invalid/assets/menu/Archivo-400.ttf", L"assets/menu/Archivo-400.ttf", L"font/ttf", ResourceKind::Font},
    {L"https://nitlink.invalid/assets/menu/Archivo-500.ttf", L"assets/menu/Archivo-500.ttf", L"font/ttf", ResourceKind::Font},
    {L"https://nitlink.invalid/assets/menu/Archivo-600.ttf", L"assets/menu/Archivo-600.ttf", L"font/ttf", ResourceKind::Font},
    {L"https://nitlink.invalid/assets/menu/kofi-cup.png", L"assets/menu/kofi-cup.png", L"image/png", ResourceKind::Image},
};

// The menu has no query or fragment navigation. Exact serialized URLs reject
// sibling documents, alternate authorities, encodings and network file paths.
inline bool IsTrustedDocument(std::wstring_view uri) { return uri == MenuUri; }
inline bool CanReceiveMessage(std::wstring_view source, std::wstring_view current) {
    return IsTrustedDocument(source) && IsTrustedDocument(current);
}
inline const Resource* FindResource(std::wstring_view uri) {
    for (const auto& resource : Resources) if (resource.uri == uri) return &resource;
    return nullptr;
}
inline bool CanOpenExternal(std::wstring_view current, std::wstring_view target, bool userInitiated) {
    return userInitiated && IsTrustedDocument(current) &&
        (target == L"https://tally.so/r/EkZjNr" ||
         target == L"https://ko-fi.com/klosed89" ||
         target == L"https://github.com/HolyBearTW" ||
         target == L"https://store.steampowered.com/app/5346980");
}
inline bool IsLocalScreenshotPath(std::wstring_view path) {
    if (path.size() < 8 || path.size() > 32767 || path[1] != L':' || path[2] != L'\\' ||
        !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) ||
        !path.ends_with(L".png")) return false;
    for (size_t i = 2; i < path.size(); ++i) {
        if (path[i] < 0x20 || path[i] == L'"' || path[i] == L':' || path[i] == L'/' ||
            path[i] == L'?' || path[i] == L'*') return false;
    }
    return path.find(L"\\..\\") == path.npos && path.find(L"\\.\\") == path.npos;
}
} // namespace NitLink::WebViewPolicy
