#include "localization.h"

#include <windows.h>

#include <map>

namespace NitLink {
namespace {

using Table = std::map<std::wstring, std::wstring>;

const Table& EnglishTable()
{
    static const Table table = {
        {L"error.com", L"Failed to initialize COM runtime."},
        {L"error.mediaFoundation", L"Failed to initialize Media Foundation."},
        {L"error.application", L"Failed to initialize NitLink.\nCheck that a capture card is connected."},
        {L"dialog.chooseNoSignalImage", L"Choose No Signal Image"},
        {L"dialog.imagesFilter", L"Images (*.png;*.jpg;*.jpeg;*.bmp)"},
        {L"dialog.allFilesFilter", L"All files (*.*)"},
        {L"overlay.noSignal", L"NO SIGNAL"},
        {L"overlay.frameRate", L"Frame rate"},
        {L"overlay.appIngest", L"App ingest"},
        {L"overlay.fps", L"FPS"},
        {L"overlay.ms", L"MS"},
        {L"overlay.gpu", L"GPU"},
        {L"overlay.waitingForSource", L"Waiting for source signal…"},
        {L"overlay.initializingCapture", L"Initializing capture device…"},
        {L"overlay.resyncingSignal", L"Resynchronizing signal…"},
        {L"overlay.inputSignalLost", L"Input signal lost"},
        {L"overlay.waitingForHdmi", L"Waiting for HDMI source…"},
        {L"overlay.checkHdmi", L"Make sure your source is powered on and the HDMI cable is seated at both ends."},
        {L"overlay.badgeColor", L"COLOR"},
        {L"value.sourceFrameRate", L"Source frame rate"},
        {L"value.captureRate", L"Capture rate"},
        {L"value.displayRefresh", L"Display refresh"},
        {L"value.auto", L"Auto"},
        {L"value.stretch", L"Stretch"},
        {L"value.right", L"Right"},
        {L"value.left", L"Left"},
        {L"toast.noAudio", L"No audio: Windows Microphone access is off. Settings > Privacy & security > Microphone."},
        {L"toast.windowsHdrDisabled", L"Windows HDR is not enabled. Press Win+Alt+B and try again."},
        {L"toast.vsyncOn", L"VSync: ON (Alt+V)"},
        {L"toast.vsyncOff", L"VSync: OFF (Alt+V)"},
        {L"toast.gc553proCaptureSwitch", L"HDR source detected. Switching capture mode…"},
        {L"toast.presentPacing", L"Present pacing"},
        {L"toast.aspectRatio", L"Aspect ratio"},
        {L"toast.colorRangeAuto", L"Color range: AUTO from Media Foundation (Alt+R)"},
        {L"toast.colorRangeFull", L"Color range: FULL forced (Alt+R)"},
        {L"toast.colorRangeLimited", L"Color range: LIMITED forced (Alt+R)"},
        {L"toast.screenshotSaved", L"Screenshot saved: "},
        {L"toast.captureFormatUnavailable", L"Capture format unavailable, reverted to automatic."},
        {L"toast.noSignalImageLoaded", L"Custom No Signal image loaded"},
        {L"toast.noSignalImageLoadFailed", L"Failed to load custom No Signal image"},
        {L"toast.configRecoveryRequired", L"The current settings file is not being used and saves are disabled after a write and restore failure. Restore the settings from {backup}, then rename or delete that recovery file and restart."},
        {L"toast.configRecoveryCopy", L"A settings recovery copy exists: {backup}. Review it before renaming or deleting it."},
        {L"toast.configFolderNotWritable", L"Settings could not be created at {path}. Check folder permissions and the shortcut\'s Start in folder."},
        {L"toast.configLoadFailed", L"Settings could not load; changes will not be saved. The original nitlink.json is retained. Correct or rename it, then restart."},
        {L"toast.configLoadFailedBackup", L"Settings could not load; changes will not be saved. Backup: {backup}. Correct or rename nitlink.json, then restart."},
        {L"toast.configSaveFailed", L"Settings could not be saved. Check folder permissions and whether nitlink.json is read-only or locked."},
        {L"toast.settingsProfile", L"Settings unavailable: the WebView2 profile folder could not be opened."},
        {L"toast.settingsRuntime", L"Settings unavailable. Install or update Microsoft Edge WebView2 Runtime."},
        {L"toast.settingsStartFailed", L"Settings could not start. Update WebView2 and restart NitLink."},
        {L"toast.settingsFiles", L"Settings files are missing, empty or unreadable. Reinstall the complete NitLink folder."},
        {L"toast.settingsSecurity", L"Settings unavailable. Update WebView2 to enable the required security features."},
        {L"toast.settingsMemory", L"Not enough memory to load settings. Restart NitLink."},
        {L"toast.settingsResources", L"Settings resources could not be loaded safely. Reinstall NitLink."},
        {L"toast.settingsNavigation", L"Settings page could not load. Reinstall the complete NitLink folder."},
        {L"toast.settingsProcessFailed", L"The settings browser stopped. Restart NitLink to restore settings."},
        {L"toast.settingsTimeout", L"Settings did not finish loading. Update WebView2 or reinstall NitLink, then restart."},
        {L"toast.settingsRetry", L"Restarting settings. The capture picture can continue."},
        {L"toast.settingsStarting", L"Settings will open when loading finishes."},
        {L"toast.imageInvalidPath", L"Choose an image using a full local file path."},
        {L"toast.imageNetworkLocation", L"Network images are unsupported. Copy the image to a local drive."},
        {L"toast.imageFileUnavailable", L"Image file is missing or unreadable. Choose it again."},
        {L"toast.imageTooLarge", L"Image is too large: maximum 512 MiB and 16384 pixels per side."},
        {L"toast.imageUnsupportedFormat", L"Unsupported image content. Use a PNG, JPEG or BMP file."},
        {L"toast.imageOutOfMemory", L"Not enough memory to load the image. Choose a smaller image."},
        {L"toast.p010SelectionFallback", L"No P010 mode matches the selected capture resolution/frame rate; using native P010 {width}x{height} @ {fps} FPS."},
        {L"toast.p010Unavailable", L"No compatible P010 mode was negotiated; using {format} capture with HDR output disabled."},
        {L"toast.manualFormatHdrRequiresP010", L"HDR requires P010 while the capture format is manually set to {format}. Select Auto or P010 to enable HDR."},
        {L"diagnostic.levelsNoRange", L"LEVELS: no YUV range on this path (test HDR / P010 capture)"},
        {L"diagnostic.levelsY", L"Y"},
        {L"diagnostic.levelsSignal", L"sig"},
        {L"diagnostic.levelsDecode", L"dec"},
        {L"diagnostic.levelsCb", L"Cb"},
        {L"diagnostic.levelsCr", L"Cr"},
        {L"diagnostic.bits8", L"8b"},
        {L"value.full", L"FULL"},
        {L"value.limited", L"LIMITED"},
        {L"value.needBlack", L"? need-black"},
        {L"unit.ms", L"ms"},
        {L"unit.fps", L"FPS"},
        {L"title.hdr", L"HDR"},
        {L"title.sdr", L"SDR"},
    };
    return table;
}

const Table& TraditionalChineseTable()
{
    static const Table table = {
        {L"error.com", L"COM 執行階段初始化失敗。"},
        {L"error.mediaFoundation", L"Media Foundation 初始化失敗。"},
        {L"error.application", L"NitLink 初始化失敗。\n請確認已連接擷取卡。"},
        {L"dialog.chooseNoSignalImage", L"選擇無訊號圖片"},
        {L"dialog.imagesFilter", L"圖片（*.png；*.jpg；*.jpeg；*.bmp）"},
        {L"dialog.allFilesFilter", L"所有檔案（*.*）"},
        {L"overlay.noSignal", L"無訊號"},
        {L"overlay.frameRate", L"幀率"},
        {L"overlay.appIngest", L"應用程式接收"},
        {L"overlay.fps", L"FPS"},
        {L"overlay.ms", L"毫秒"},
        {L"overlay.gpu", L"GPU"},
        {L"overlay.waitingForSource", L"正在等待來源訊號…"},
        {L"overlay.initializingCapture", L"正在初始化擷取裝置…"},
        {L"overlay.resyncingSignal", L"正在重新同步訊號…"},
        {L"overlay.inputSignalLost", L"輸入訊號遺失"},
        {L"overlay.waitingForHdmi", L"正在等待 HDMI 訊號來源…"},
        {L"overlay.checkHdmi", L"請確認訊號來源已開啟，且 HDMI 線材兩端都已連接。"},
        {L"overlay.badgeColor", L"色彩"},
        {L"value.sourceFrameRate", L"來源幀率"},
        {L"value.captureRate", L"擷取幀率"},
        {L"value.displayRefresh", L"顯示器更新率"},
        {L"value.auto", L"自動"},
        {L"value.stretch", L"拉伸"},
        {L"value.right", L"右側"},
        {L"value.left", L"左側"},
        {L"toast.noAudio", L"沒有音訊：Windows 麥克風存取權已關閉。請前往「設定 > 隱私權與安全性 > 麥克風」。"},
        {L"toast.windowsHdrDisabled", L"Windows HDR 尚未啟用。請按 Win+Alt+B 後再試一次。"},
        {L"toast.vsyncOn", L"垂直同步：開啟（Alt+V）"},
        {L"toast.vsyncOff", L"垂直同步：關閉（Alt+V）"},
        {L"toast.gc553proCaptureSwitch", L"HDR 來源已偵測，正在切換擷取模式…"},
        {L"toast.presentPacing", L"畫面呈現節奏"},
        {L"toast.aspectRatio", L"長寬比"},
        {L"toast.colorRangeAuto", L"色彩範圍：由 Media Foundation 自動判定（Alt+R）"},
        {L"toast.colorRangeFull", L"色彩範圍：強制完整範圍（Alt+R）"},
        {L"toast.colorRangeLimited", L"色彩範圍：強制有限範圍（Alt+R）"},
        {L"toast.screenshotSaved", L"螢幕擷取畫面已儲存："},
        {L"toast.captureFormatUnavailable", L"無法使用指定的擷取格式，已恢復為自動。"},
        {L"toast.noSignalImageLoaded", L"自訂無訊號圖片已載入"},
        {L"toast.noSignalImageLoadFailed", L"無法載入自訂無訊號圖片"},
        {L"toast.configRecoveryRequired", L"寫入及還原均失敗，目前的設定檔未被使用，並已停用儲存。請從 {backup} 還原設定，再重新命名或刪除該復原檔案，然後重新啟動。"},
        {L"toast.configRecoveryCopy", L"存在設定復原備份：{backup}。請先檢查內容，再重新命名或刪除該檔案。"},
        {L"toast.configFolderNotWritable", L"無法在 {path} 建立設定。請檢查資料夾權限及捷徑的「開始位置」。"},
        {L"toast.configLoadFailed", L"無法載入設定，變更將不會儲存。原始 nitlink.json 已保留。請修正或重新命名該檔案後重新啟動。"},
        {L"toast.configLoadFailedBackup", L"無法載入設定，變更將不會儲存。備份：{backup}。請修正或重新命名 nitlink.json 後重新啟動。"},
        {L"toast.configSaveFailed", L"無法儲存設定。請檢查資料夾權限，以及 nitlink.json 是否為唯讀或被鎖定。"},
        {L"toast.settingsProfile", L"無法使用設定：無法開啟 WebView2 設定檔資料夾。"},
        {L"toast.settingsRuntime", L"無法使用設定。請安裝或更新 Microsoft Edge WebView2 執行階段。"},
        {L"toast.settingsStartFailed", L"無法啟動設定。請更新 WebView2 並重新啟動 NitLink。"},
        {L"toast.settingsFiles", L"設定檔案遺失、空白或無法讀取。請重新安裝完整的 NitLink 資料夾。"},
        {L"toast.settingsSecurity", L"無法使用設定。請更新 WebView2 以啟用必要的安全功能。"},
        {L"toast.settingsMemory", L"記憶體不足，無法載入設定。請重新啟動 NitLink。"},
        {L"toast.settingsResources", L"無法安全地載入設定資源。請重新安裝 NitLink。"},
        {L"toast.settingsNavigation", L"無法載入設定頁面。請重新安裝完整的 NitLink 資料夾。"},
        {L"toast.settingsProcessFailed", L"設定瀏覽器已停止。請重新啟動 NitLink 以恢復設定。"},
        {L"toast.settingsTimeout", L"設定未能完成載入。請更新 WebView2 或重新安裝 NitLink，然後重新啟動。"},
        {L"toast.settingsRetry", L"正在重新啟動設定，擷取畫面可繼續播放。"},
        {L"toast.settingsStarting", L"設定將於載入完成後開啟。"},
        {L"toast.imageInvalidPath", L"請選擇具有完整本機路徑的圖片。"},
        {L"toast.imageNetworkLocation", L"不支援網路圖片，請先將圖片複製到本機磁碟。"},
        {L"toast.imageFileUnavailable", L"找不到圖片或無法讀取，請重新選擇。"},
        {L"toast.imageTooLarge", L"圖片過大：上限為 512 MiB，每邊不超過 16384 像素。"},
        {L"toast.imageUnsupportedFormat", L"不支援此圖片內容，請使用 PNG、JPEG 或 BMP 檔案。"},
        {L"toast.imageOutOfMemory", L"記憶體不足，無法載入圖片。請選擇較小的圖片。"},
        {L"toast.p010SelectionFallback", L"所選擷取解析度／幀率沒有相符的 P010 模式；改用原生 P010 {width}×{height} @ {fps} FPS。"},
        {L"toast.p010Unavailable", L"未協商到相容的 P010 模式；目前使用 {format} 擷取，HDR 輸出已停用。"},
        {L"toast.manualFormatHdrRequiresP010", L"目前擷取格式手動設定為 {format}；HDR 需要 P010。請改用「自動」或 P010 以啟用 HDR。"},
        {L"diagnostic.levelsNoRange", L"訊號層級：此路徑沒有 YUV 範圍（請使用 HDR / P010 擷取測試）"},
        {L"diagnostic.levelsY", L"Y"},
        {L"diagnostic.levelsSignal", L"訊號"},
        {L"diagnostic.levelsDecode", L"解碼"},
        {L"diagnostic.levelsCb", L"Cb"},
        {L"diagnostic.levelsCr", L"Cr"},
        {L"diagnostic.bits8", L"8 位元"},
        {L"value.full", L"完整"},
        {L"value.limited", L"有限"},
        {L"value.needBlack", L"？需要黑階"},
        {L"unit.ms", L"毫秒"},
        {L"unit.fps", L"FPS"},
        {L"title.hdr", L"HDR"},
        {L"title.sdr", L"SDR"},
    };
    return table;
}

} // namespace

Localization& Localization::Instance()
{
    static Localization instance;
    return instance;
}

Localization::Localization()
{
    SetPreference(LanguagePreference::System);
}

LanguagePreference Localization::ParsePreference(const std::string& value)
{
    if (value == "en-US") return LanguagePreference::English;
    if (value == "zh-TW") return LanguagePreference::TraditionalChinese;
    return LanguagePreference::System;
}

void Localization::SetPreference(const std::string& value)
{
    SetPreference(ParsePreference(value));
}

void Localization::SetPreference(LanguagePreference preference)
{
    m_preference = preference;
    switch (preference) {
    case LanguagePreference::English:
        m_preferenceName = "en-US";
        m_localeName = L"en-US";
        break;
    case LanguagePreference::TraditionalChinese:
        m_preferenceName = "zh-TW";
        m_localeName = L"zh-TW";
        break;
    case LanguagePreference::System:
    default:
        m_preferenceName = "system";
        m_localeName = IsTraditionalChineseLocale(DetectSystemLocale())
            ? L"zh-TW" : L"en-US";
        break;
    }
}

std::wstring Localization::DetectSystemLocale()
{
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
    LANGID langId = GetUserDefaultUILanguage();
    if (langId == 0) langId = GetSystemDefaultUILanguage();
    if (langId != 0 && LCIDToLocaleName(MAKELCID(langId, SORT_DEFAULT), locale,
                                        LOCALE_NAME_MAX_LENGTH, 0) > 0) {
        return locale;
    }
    return L"en-US";
}

bool Localization::IsTraditionalChineseLocale(const std::wstring& locale)
{
    return locale == L"zh-TW" || locale == L"zh-Hant-TW";
}

std::wstring Localization::Get(const wchar_t* key) const
{
    if (!key || !*key) return L"";

    const Table& selected = m_localeName == L"zh-TW"
        ? TraditionalChineseTable() : EnglishTable();
    auto it = selected.find(key);
    if (it != selected.end() && !it->second.empty()) return it->second;

    const Table& fallback = EnglishTable();
    it = fallback.find(key);
    if (it != fallback.end() && !it->second.empty()) return it->second;
    return key;
}

std::wstring Localization::Format(
    const wchar_t* key,
    std::initializer_list<std::pair<std::wstring, std::wstring>> values) const
{
    std::wstring text = Get(key);
    for (const auto& [name, value] : values) {
        const std::wstring placeholder = L"{" + name + L"}";
        size_t position = 0;
        while ((position = text.find(placeholder, position)) != std::wstring::npos) {
            text.replace(position, placeholder.size(), value);
            position += value.size();
        }
    }
    return text;
}

const wchar_t* Localization::UiFontFamily(const wchar_t* fallback) const
{
    return m_localeName == L"zh-TW" ? L"Microsoft JhengHei UI" : fallback;
}

} // namespace NitLink
