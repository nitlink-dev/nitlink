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
        {L"overlay.presentRate", L"Present rate"},
        {L"overlay.presentFps", L"Present FPS"},
        {L"overlay.captureFps", L"Capture FPS"},
        {L"overlay.contentFps", L"Content FPS (est.)"},
        {L"overlay.appIngest", L"App ingest"},
        {L"overlay.fps", L"FPS"},
        {L"overlay.ms", L"MS"},
        {L"overlay.gpu", L"GPU"},
        {L"overlay.waitingForSource", L"Waiting for source signal…"},
        {L"overlay.initializingCapture", L"Initializing capture device…"},
        {L"overlay.switchingHdr", L"Switching HDR…"},
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
        {L"overlay.presentRate", L"呈現幀率"},
        {L"overlay.presentFps", L"呈現 FPS"},
        {L"overlay.captureFps", L"擷取 FPS"},
        {L"overlay.contentFps", L"內容變化 FPS（估計）"},
        {L"overlay.appIngest", L"應用程式接收"},
        {L"overlay.fps", L"FPS"},
        {L"overlay.ms", L"毫秒"},
        {L"overlay.gpu", L"GPU"},
        {L"overlay.waitingForSource", L"正在等待來源訊號…"},
        {L"overlay.initializingCapture", L"正在初始化擷取裝置…"},
        {L"overlay.switchingHdr", L"正在切換 HDR…"},
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

const Table& SimplifiedChineseTable()
{
    static const Table table = {
        {L"error.com", L"COM 运行时初始化失败。"},
        {L"error.mediaFoundation", L"Media Foundation 初始化失败。"},
        {L"error.application", L"NitLink 初始化失败。\n请确认已连接采集卡。"},
        {L"dialog.chooseNoSignalImage", L"选择无信号图片"},
        {L"dialog.imagesFilter", L"图片（*.png；*.jpg；*.jpeg；*.bmp）"},
        {L"dialog.allFilesFilter", L"所有文件（*.*）"},
        {L"overlay.noSignal", L"无信号"},
        {L"overlay.frameRate", L"帧率"},
        {L"overlay.presentRate", L"呈现频率"},
        {L"overlay.presentFps", L"呈现 FPS"},
        {L"overlay.captureFps", L"采集 FPS"},
        {L"overlay.contentFps", L"内容变化 FPS（估算）"},
        {L"overlay.appIngest", L"程序接收"},
        {L"overlay.fps", L"FPS"},
        {L"overlay.ms", L"毫秒"},
        {L"overlay.gpu", L"GPU"},
        {L"overlay.waitingForSource", L"正在等待来源信号…"},
        {L"overlay.initializingCapture", L"正在初始化采集设备…"},
        {L"overlay.switchingHdr", L"正在切换 HDR…"},
        {L"overlay.inputSignalLost", L"输入信号丢失"},
        {L"overlay.waitingForHdmi", L"正在等待 HDMI 信号来源…"},
        {L"overlay.checkHdmi", L"请确认信号来源已打开，且 HDMI 线材两端都已连接。"},
        {L"overlay.badgeColor", L"色彩"},
        {L"value.sourceFrameRate", L"来源帧率"},
        {L"value.captureRate", L"采集帧率"},
        {L"value.displayRefresh", L"显示器刷新率"},
        {L"value.auto", L"自动"},
        {L"value.stretch", L"拉伸"},
        {L"value.right", L"右侧"},
        {L"value.left", L"左侧"},
        {L"toast.noAudio", L"没有音频：Windows 麦克风访问权已关闭。请前往「设置 > 隐私和安全性 > 麦克风」。"},
        {L"toast.windowsHdrDisabled", L"Windows HDR 尚未启用。请按 Win+Alt+B 后再试一次。"},
        {L"toast.vsyncOn", L"垂直同步：开启（Alt+V）"},
        {L"toast.vsyncOff", L"垂直同步：关闭（Alt+V）"},
        {L"toast.gc553proCaptureSwitch", L"HDR 来源已检测，正在切换采集模式…"},
        {L"toast.presentPacing", L"画面呈现节奏"},
        {L"toast.aspectRatio", L"宽高比"},
        {L"toast.colorRangeAuto", L"色彩范围：由 Media Foundation 自动判定（Alt+R）"},
        {L"toast.colorRangeFull", L"色彩范围：强制完整范围（Alt+R）"},
        {L"toast.colorRangeLimited", L"色彩范围：强制有限范围（Alt+R）"},
        {L"toast.screenshotSaved", L"截图已保存："},
        {L"toast.captureFormatUnavailable", L"无法使用指定的采集格式，已恢复为自动。"},
        {L"toast.noSignalImageLoaded", L"自定义无信号图片已加载"},
        {L"toast.noSignalImageLoadFailed", L"无法加载自定义无信号图片"},
        {L"toast.configRecoveryRequired", L"写入及还原均失败，目前的配置文件未被使用，并已停用保存。请从 {backup} 还原设置，再重命名或删除该复原文件，然后重新启动。"},
        {L"toast.configRecoveryCopy", L"存在设置复原备份：{backup}。请先检查内容，再重命名或删除该文件。"},
        {L"toast.configFolderNotWritable", L"无法在 {path} 创建设置。请检查文件夹权限及快捷方式的“起始位置”。"},
        {L"toast.configLoadFailed", L"无法加载设置，变更将不会保存。原始 nitlink.json 已保留。请修正或重命名该文件后重新启动。"},
        {L"toast.configLoadFailedBackup", L"无法加载设置，变更将不会保存。备份：{backup}。请修正或重命名 nitlink.json 后重新启动。"},
        {L"toast.configSaveFailed", L"无法保存设置。请检查文件夹权限，以及 nitlink.json 是否为只读或被锁定。"},
        {L"toast.settingsProfile", L"无法使用设置：无法打开 WebView2 配置文件文件夹。"},
        {L"toast.settingsRuntime", L"无法使用设置。请安装或更新 Microsoft Edge WebView2 运行时。"},
        {L"toast.settingsStartFailed", L"无法启动设置。请更新 WebView2 并重新启动 NitLink。"},
        {L"toast.settingsFiles", L"设置文件丢失、空白或无法读取。请重新安装完整的 NitLink 文件夹。"},
        {L"toast.settingsSecurity", L"无法使用设置。请更新 WebView2 以启用必要的安全功能。"},
        {L"toast.settingsMemory", L"内存不足，无法加载设置。请重新启动 NitLink。"},
        {L"toast.settingsResources", L"无法安全地加载设置资源。请重新安装 NitLink。"},
        {L"toast.settingsNavigation", L"无法加载设置页面。请重新安装完整的 NitLink 文件夹。"},
        {L"toast.settingsProcessFailed", L"设置浏览器已停止。请重新启动 NitLink 以恢复设置。"},
        {L"toast.settingsTimeout", L"设置未能完成加载。请更新 WebView2 或重新安装 NitLink，然后重新启动。"},
        {L"toast.settingsRetry", L"正在重新启动设置，采集画面可继续播放。"},
        {L"toast.settingsStarting", L"设置将于加载完成后打开。"},
        {L"toast.imageInvalidPath", L"请选择具有完整本机路径的图片。"},
        {L"toast.imageNetworkLocation", L"不支持网络图片，请先将图片复制到本机磁盘。"},
        {L"toast.imageFileUnavailable", L"找不到图片或无法读取，请重新选择。"},
        {L"toast.imageTooLarge", L"图片过大：上限为 512 MiB，每边不超过 16384 像素。"},
        {L"toast.imageUnsupportedFormat", L"不支持此图片内容，请使用 PNG、JPEG 或 BMP 文件。"},
        {L"toast.imageOutOfMemory", L"内存不足，无法加载图片。请选择较小的图片。"},
        {L"toast.p010SelectionFallback", L"所选采集分辨率／帧率没有相符的 P010 模式；改用原生 P010 {width}×{height} @ {fps} FPS。"},
        {L"toast.p010Unavailable", L"未协商到兼容的 P010 模式；目前使用 {format} 采集，HDR 输出已停用。"},
        {L"toast.manualFormatHdrRequiresP010", L"目前采集格式手动设置为 {format}；HDR 需要 P010。请改用「自动」或 P010 以启用 HDR。"},
        {L"diagnostic.levelsNoRange", L"信号层级：此路径没有 YUV 范围（请使用 HDR / P010 采集测试）"},
        {L"diagnostic.levelsY", L"Y"},
        {L"diagnostic.levelsSignal", L"信号"},
        {L"diagnostic.levelsDecode", L"解码"},
        {L"diagnostic.levelsCb", L"Cb"},
        {L"diagnostic.levelsCr", L"Cr"},
        {L"diagnostic.bits8", L"8 比特"},
        {L"value.full", L"完整"},
        {L"value.limited", L"有限"},
        {L"value.needBlack", L"？需要黑阶"},
        {L"unit.ms", L"毫秒"},
        {L"unit.fps", L"FPS"},
        {L"title.hdr", L"HDR"},
        {L"title.sdr", L"SDR"},
    };
    return table;
}

const Table& GermanTable()
{
    static const Table table = {
        {L"error.com", L"Die COM-Laufzeit konnte nicht initialisiert werden."},
        {L"error.mediaFoundation", L"Media Foundation konnte nicht initialisiert werden."},
        {L"error.application", L"NitLink konnte nicht initialisiert werden.\nPrüfen Sie, ob eine Aufnahmekarte angeschlossen ist."},
        {L"dialog.chooseNoSignalImage", L"Bild für „Kein Signal“ auswählen"},
        {L"dialog.imagesFilter", L"Bilder (*.png;*.jpg;*.jpeg;*.bmp)"},
        {L"dialog.allFilesFilter", L"Alle Dateien (*.*)"},
        {L"overlay.noSignal", L"KEIN SIGNAL"},
        {L"overlay.frameRate", L"Bildrate"},
        {L"overlay.presentRate", L"Ausgaberate"},
        {L"overlay.presentFps", L"Ausgabe-FPS"},
        {L"overlay.captureFps", L"Aufnahme-FPS"},
        {L"overlay.contentFps", L"Inhalts-FPS (geschätzt)"},
        {L"overlay.appIngest", L"App-Eingang"},
        {L"overlay.fps", L"FPS"},
        {L"overlay.ms", L"MS"},
        {L"overlay.gpu", L"GPU"},
        {L"overlay.waitingForSource", L"Warte auf Quellsignal…"},
        {L"overlay.initializingCapture", L"Aufnahmegerät wird initialisiert…"},
        {L"overlay.switchingHdr", L"HDR wird umgeschaltet…"},
        {L"overlay.inputSignalLost", L"Eingangssignal verloren"},
        {L"overlay.waitingForHdmi", L"Warte auf HDMI-Quelle…"},
        {L"overlay.checkHdmi", L"Stellen Sie sicher, dass die Quelle eingeschaltet ist und das HDMI-Kabel an beiden Enden fest steckt."},
        {L"overlay.badgeColor", L"FARBE"},
        {L"value.sourceFrameRate", L"Quell-Bildrate"},
        {L"value.captureRate", L"Aufnahmerate"},
        {L"value.displayRefresh", L"Bildwiederholrate"},
        {L"value.auto", L"Auto"},
        {L"value.stretch", L"Strecken"},
        {L"value.right", L"Rechts"},
        {L"value.left", L"Links"},
        {L"toast.noAudio", L"Kein Audio: Der Mikrofonzugriff von Windows ist deaktiviert. Einstellungen > Datenschutz und Sicherheit > Mikrofon."},
        {L"toast.windowsHdrDisabled", L"Windows-HDR ist nicht aktiviert. Drücken Sie Win+Alt+B und versuchen Sie es erneut."},
        {L"toast.vsyncOn", L"VSync: EIN (Alt+V)"},
        {L"toast.vsyncOff", L"VSync: AUS (Alt+V)"},
        {L"toast.gc553proCaptureSwitch", L"HDR-Quelle erkannt. Aufnahmemodus wird umgeschaltet…"},
        {L"toast.presentPacing", L"Ausgabetakt"},
        {L"toast.aspectRatio", L"Seitenverhältnis"},
        {L"toast.colorRangeAuto", L"Farbbereich: AUTO über Media Foundation (Alt+R)"},
        {L"toast.colorRangeFull", L"Farbbereich: VOLL erzwungen (Alt+R)"},
        {L"toast.colorRangeLimited", L"Farbbereich: BEGRENZT erzwungen (Alt+R)"},
        {L"toast.screenshotSaved", L"Screenshot gespeichert: "},
        {L"toast.captureFormatUnavailable", L"Aufnahmeformat nicht verfügbar, auf automatisch zurückgesetzt."},
        {L"toast.noSignalImageLoaded", L"Eigenes „Kein Signal“-Bild geladen"},
        {L"toast.noSignalImageLoadFailed", L"Eigenes „Kein Signal“-Bild konnte nicht geladen werden"},
        {L"toast.configRecoveryRequired", L"Die aktuelle Einstellungsdatei wird nicht verwendet, und Speichern ist nach einem Schreib- und Wiederherstellungsfehler deaktiviert. Stellen Sie die Einstellungen aus {backup} wieder her, benennen Sie diese Wiederherstellungsdatei um oder löschen Sie sie, und starten Sie dann neu."},
        {L"toast.configRecoveryCopy", L"Es gibt eine Wiederherstellungskopie der Einstellungen: {backup}. Prüfen Sie sie, bevor Sie sie umbenennen oder löschen."},
        {L"toast.configFolderNotWritable", L"Die Einstellungen konnten nicht unter {path} erstellt werden. Prüfen Sie die Ordnerberechtigungen und das Feld „Ausführen in“ der Verknüpfung."},
        {L"toast.configLoadFailed", L"Die Einstellungen konnten nicht geladen werden; Änderungen werden nicht gespeichert. Die ursprüngliche nitlink.json bleibt erhalten. Korrigieren Sie sie oder benennen Sie sie um, und starten Sie dann neu."},
        {L"toast.configLoadFailedBackup", L"Die Einstellungen konnten nicht geladen werden; Änderungen werden nicht gespeichert. Sicherung: {backup}. Korrigieren Sie nitlink.json oder benennen Sie sie um, und starten Sie dann neu."},
        {L"toast.configSaveFailed", L"Die Einstellungen konnten nicht gespeichert werden. Prüfen Sie die Ordnerberechtigungen und ob nitlink.json schreibgeschützt oder gesperrt ist."},
        {L"toast.settingsProfile", L"Einstellungen nicht verfügbar: Der WebView2-Profilordner konnte nicht geöffnet werden."},
        {L"toast.settingsRuntime", L"Einstellungen nicht verfügbar. Installieren oder aktualisieren Sie die Microsoft Edge WebView2 Runtime."},
        {L"toast.settingsStartFailed", L"Die Einstellungen konnten nicht gestartet werden. Aktualisieren Sie WebView2 und starten Sie NitLink neu."},
        {L"toast.settingsFiles", L"Einstellungsdateien fehlen, sind leer oder nicht lesbar. Installieren Sie den vollständigen NitLink-Ordner neu."},
        {L"toast.settingsSecurity", L"Einstellungen nicht verfügbar. Aktualisieren Sie WebView2, um die erforderlichen Sicherheitsfunktionen zu aktivieren."},
        {L"toast.settingsMemory", L"Nicht genügend Arbeitsspeicher zum Laden der Einstellungen. Starten Sie NitLink neu."},
        {L"toast.settingsResources", L"Die Einstellungsressourcen konnten nicht sicher geladen werden. Installieren Sie NitLink neu."},
        {L"toast.settingsNavigation", L"Die Einstellungsseite konnte nicht geladen werden. Installieren Sie den vollständigen NitLink-Ordner neu."},
        {L"toast.settingsProcessFailed", L"Der Einstellungsbrowser wurde beendet. Starten Sie NitLink neu, um die Einstellungen wiederherzustellen."},
        {L"toast.settingsTimeout", L"Die Einstellungen wurden nicht fertig geladen. Aktualisieren Sie WebView2 oder installieren Sie NitLink neu, und starten Sie dann neu."},
        {L"toast.settingsRetry", L"Die Einstellungen werden neu gestartet. Das Aufnahmebild läuft weiter."},
        {L"toast.settingsStarting", L"Die Einstellungen öffnen sich, sobald sie geladen sind."},
        {L"toast.imageInvalidPath", L"Wählen Sie ein Bild über einen vollständigen lokalen Dateipfad aus."},
        {L"toast.imageNetworkLocation", L"Bilder im Netzwerk werden nicht unterstützt. Kopieren Sie das Bild auf ein lokales Laufwerk."},
        {L"toast.imageFileUnavailable", L"Die Bilddatei fehlt oder ist nicht lesbar. Wählen Sie sie erneut aus."},
        {L"toast.imageTooLarge", L"Das Bild ist zu groß: maximal 512 MiB und 16384 Pixel pro Seite."},
        {L"toast.imageUnsupportedFormat", L"Nicht unterstützter Bildinhalt. Verwenden Sie eine PNG-, JPEG- oder BMP-Datei."},
        {L"toast.imageOutOfMemory", L"Nicht genügend Arbeitsspeicher zum Laden des Bildes. Wählen Sie ein kleineres Bild."},
        {L"toast.p010SelectionFallback", L"Kein P010-Modus passt zur gewählten Aufnahmeauflösung/Bildrate; natives P010 {width}x{height} @ {fps} FPS wird verwendet."},
        {L"toast.p010Unavailable", L"Es wurde kein kompatibler P010-Modus ausgehandelt; {format}-Aufnahme wird verwendet, die HDR-Ausgabe ist deaktiviert."},
        {L"toast.manualFormatHdrRequiresP010", L"HDR erfordert P010, das Aufnahmeformat ist aber manuell auf {format} gesetzt. Wählen Sie Auto oder P010, um HDR zu aktivieren."},
        {L"diagnostic.levelsNoRange", L"PEGEL: kein YUV-Bereich auf diesem Pfad (mit HDR- / P010-Aufnahme testen)"},
        {L"diagnostic.levelsY", L"Y"},
        {L"diagnostic.levelsSignal", L"sig"},
        {L"diagnostic.levelsDecode", L"dek"},
        {L"diagnostic.levelsCb", L"Cb"},
        {L"diagnostic.levelsCr", L"Cr"},
        {L"diagnostic.bits8", L"8b"},
        {L"value.full", L"VOLL"},
        {L"value.limited", L"BEGRENZT"},
        {L"value.needBlack", L"? Schwarz nötig"},
        {L"unit.ms", L"ms"},
        {L"unit.fps", L"FPS"},
        {L"title.hdr", L"HDR"},
        {L"title.sdr", L"SDR"},
    };
    return table;
}

const Table& KoreanTable()
{
    static const Table table = {
        {L"error.com", L"COM 런타임을 초기화하지 못했습니다."},
        {L"error.mediaFoundation", L"Media Foundation을 초기화하지 못했습니다."},
        {L"error.application", L"NitLink를 초기화하지 못했습니다.\n캡처보드가 연결되어 있는지 확인하세요."},
        {L"dialog.chooseNoSignalImage", L"신호 없음 이미지 선택"},
        {L"dialog.imagesFilter", L"이미지 (*.png;*.jpg;*.jpeg;*.bmp)"},
        {L"dialog.allFilesFilter", L"모든 파일 (*.*)"},
        {L"overlay.noSignal", L"신호 없음"},
        {L"overlay.frameRate", L"프레임 레이트"},
        {L"overlay.presentRate", L"출력 빈도"},
        {L"overlay.presentFps", L"출력 FPS"},
        {L"overlay.captureFps", L"캡처 FPS"},
        {L"overlay.contentFps", L"콘텐츠 FPS (추정)"},
        {L"overlay.appIngest", L"앱 전달 지연"},
        {L"overlay.fps", L"FPS"},
        {L"overlay.ms", L"MS"},
        {L"overlay.gpu", L"GPU"},
        {L"overlay.waitingForSource", L"소스 신호 대기 중…"},
        {L"overlay.initializingCapture", L"캡처 장치 초기화 중…"},
        {L"overlay.switchingHdr", L"HDR 전환 중…"},
        {L"overlay.inputSignalLost", L"입력 신호 끊김"},
        {L"overlay.waitingForHdmi", L"HDMI 소스 대기 중…"},
        {L"overlay.checkHdmi", L"소스 장치의 전원이 켜져 있고 HDMI 케이블 양쪽이 제대로 연결되어 있는지 확인하세요."},
        {L"overlay.badgeColor", L"색상"},
        {L"value.sourceFrameRate", L"소스 프레임 레이트"},
        {L"value.captureRate", L"캡처 프레임 레이트"},
        {L"value.displayRefresh", L"디스플레이 주사율"},
        {L"value.auto", L"자동"},
        {L"value.stretch", L"늘이기"},
        {L"value.right", L"오른쪽"},
        {L"value.left", L"왼쪽"},
        {L"toast.noAudio", L"오디오 없음: Windows 마이크 액세스가 꺼져 있습니다. 설정 > 개인 정보 및 보안 > 마이크에서 확인하세요."},
        {L"toast.windowsHdrDisabled", L"Windows HDR이 활성화되어 있지 않습니다. Win+Alt+B를 누른 후 다시 시도하세요."},
        {L"toast.vsyncOn", L"수직 동기화: 켜짐 (Alt+V)"},
        {L"toast.vsyncOff", L"수직 동기화: 꺼짐 (Alt+V)"},
        {L"toast.gc553proCaptureSwitch", L"HDR 소스를 감지했습니다. 캡처 모드 전환 중…"},
        {L"toast.presentPacing", L"화면 출력 주기"},
        {L"toast.aspectRatio", L"화면 비율"},
        {L"toast.colorRangeAuto", L"색상 범위: Media Foundation 자동 설정 (Alt+R)"},
        {L"toast.colorRangeFull", L"색상 범위: 전체 범위 강제 적용 (Alt+R)"},
        {L"toast.colorRangeLimited", L"색상 범위: 제한 범위 강제 적용 (Alt+R)"},
        {L"toast.screenshotSaved", L"스크린샷 저장됨: "},
        {L"toast.captureFormatUnavailable", L"캡처 형식을 사용할 수 없어 자동 설정으로 되돌렸습니다."},
        {L"toast.noSignalImageLoaded", L"사용자 지정 신호 없음 이미지를 불러왔습니다"},
        {L"toast.noSignalImageLoadFailed", L"사용자 지정 신호 없음 이미지를 불러오지 못했습니다"},
        {L"toast.configRecoveryRequired", L"쓰기와 복원에 실패하여 현재 설정 파일을 사용하지 않으며 저장 기능이 비활성화되었습니다. {backup}에서 설정을 복원한 뒤, 해당 복구 파일의 이름을 바꾸거나 삭제하고 다시 시작하세요."},
        {L"toast.configRecoveryCopy", L"설정 복구 사본이 있습니다: {backup}. 이름을 바꾸거나 삭제하기 전에 내용을 확인하세요."},
        {L"toast.configFolderNotWritable", L"{path}에 설정을 생성하지 못했습니다. 폴더 권한과 바로 가기의 시작 위치를 확인하세요."},
        {L"toast.configLoadFailed", L"설정을 불러오지 못했습니다. 변경 사항은 저장되지 않습니다. 원본 nitlink.json은 보존됩니다. 파일을 수정하거나 이름을 바꾼 뒤 다시 시작하세요."},
        {L"toast.configLoadFailedBackup", L"설정을 불러오지 못했습니다. 변경 사항은 저장되지 않습니다. 백업: {backup}. nitlink.json을 수정하거나 이름을 바꾼 뒤 다시 시작하세요."},
        {L"toast.configSaveFailed", L"설정을 저장하지 못했습니다. 폴더 권한과 nitlink.json이 읽기 전용이거나 잠겨 있는지 확인하세요."},
        {L"toast.settingsProfile", L"설정을 사용할 수 없습니다. WebView2 프로필 폴더를 열지 못했습니다."},
        {L"toast.settingsRuntime", L"설정을 사용할 수 없습니다. Microsoft Edge WebView2 런타임을 설치하거나 업데이트하세요."},
        {L"toast.settingsStartFailed", L"설정을 시작하지 못했습니다. WebView2를 업데이트하고 NitLink를 다시 시작하세요."},
        {L"toast.settingsFiles", L"설정 파일이 없거나, 비어 있거나, 읽을 수 없습니다. 전체 NitLink 폴더를 다시 설치하세요."},
        {L"toast.settingsSecurity", L"설정을 사용할 수 없습니다. 필요한 보안 기능을 활성화하려면 WebView2를 업데이트하세요."},
        {L"toast.settingsMemory", L"설정을 불러올 메모리가 부족합니다. NitLink를 다시 시작하세요."},
        {L"toast.settingsResources", L"설정 리소스를 안전하게 불러오지 못했습니다. NitLink를 다시 설치하세요."},
        {L"toast.settingsNavigation", L"설정 페이지를 불러오지 못했습니다. 전체 NitLink 폴더를 다시 설치하세요."},
        {L"toast.settingsProcessFailed", L"설정 브라우저가 중지되었습니다. 설정을 복구하려면 NitLink를 다시 시작하세요."},
        {L"toast.settingsTimeout", L"설정 로딩이 완료되지 않았습니다. WebView2를 업데이트하거나 NitLink를 다시 설치한 뒤 다시 시작하세요."},
        {L"toast.settingsRetry", L"설정을 다시 시작하는 중입니다. 캡처 영상은 계속 재생할 수 있습니다."},
        {L"toast.settingsStarting", L"로딩이 완료되면 설정이 열립니다."},
        {L"toast.imageInvalidPath", L"전체 로컬 파일 경로로 이미지를 선택하세요."},
        {L"toast.imageNetworkLocation", L"네트워크 이미지는 지원하지 않습니다. 이미지를 로컬 드라이브에 복사하세요."},
        {L"toast.imageFileUnavailable", L"이미지 파일이 없거나 읽을 수 없습니다. 다시 선택하세요."},
        {L"toast.imageTooLarge", L"이미지가 너무 큽니다. 최대 512 MiB이며 가로와 세로는 각각 16384픽셀 이하여야 합니다."},
        {L"toast.imageUnsupportedFormat", L"지원하지 않는 이미지 형식입니다. PNG, JPEG 또는 BMP 파일을 사용하세요."},
        {L"toast.imageOutOfMemory", L"이미지를 불러올 메모리가 부족합니다. 더 작은 이미지를 선택하세요."},
        {L"toast.p010SelectionFallback", L"선택한 캡처 해상도/프레임 레이트에 맞는 P010 모드가 없습니다. 네이티브 P010 {width}x{height} @ {fps} FPS를 사용합니다."},
        {L"toast.p010Unavailable", L"캡처보드와 호환되는 P010 모드를 맞추지 못했습니다. HDR 출력을 비활성화하고 {format} 캡처를 사용합니다."},
        {L"toast.manualFormatHdrRequiresP010", L"HDR에는 P010이 필요하지만 캡처 형식이 수동으로 {format}에 설정되어 있습니다. HDR을 활성화하려면 자동 또는 P010을 선택하세요."},
        {L"diagnostic.levelsNoRange", L"레벨: 이 경로에는 YUV 범위가 없습니다 (HDR / P010 캡처로 테스트하세요)"},
        {L"diagnostic.levelsY", L"Y"},
        {L"diagnostic.levelsSignal", L"신호"},
        {L"diagnostic.levelsDecode", L"디코딩"},
        {L"diagnostic.levelsCb", L"Cb"},
        {L"diagnostic.levelsCr", L"Cr"},
        {L"diagnostic.bits8", L"8비트"},
        {L"value.full", L"전체"},
        {L"value.limited", L"제한"},
        {L"value.needBlack", L"? 검은 화면 필요"},
        {L"unit.ms", L"ms"},
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
    if (value == "zh-CN") return LanguagePreference::SimplifiedChinese;
    if (value == "zh-TW") return LanguagePreference::TraditionalChinese;
    if (value == "de-DE") return LanguagePreference::German;
    if (value == "ko-KR") return LanguagePreference::Korean;
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
    case LanguagePreference::SimplifiedChinese:
        m_preferenceName = "zh-CN";
        m_localeName = L"zh-CN";
        break;
    case LanguagePreference::TraditionalChinese:
        m_preferenceName = "zh-TW";
        m_localeName = L"zh-TW";
        break;
    case LanguagePreference::German:
        m_preferenceName = "de-DE";
        m_localeName = L"de-DE";
        break;
    case LanguagePreference::Korean:
        m_preferenceName = "ko-KR";
        m_localeName = L"ko-KR";
        break;
    case LanguagePreference::System:
    default: {
        m_preferenceName = "system";
        const auto locale = DetectSystemLocale();
        m_localeName = IsSimplifiedChineseLocale(locale) ? L"zh-CN"
            : IsTraditionalChineseLocale(locale) ? L"zh-TW"
            : IsGermanLocale(locale) ? L"de-DE"
            : IsKoreanLocale(locale) ? L"ko-KR" : L"en-US";
        break;
    }
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

bool Localization::IsSimplifiedChineseLocale(const std::wstring& locale)
{
    return locale == L"zh-CN" || locale == L"zh-SG" || locale == L"zh-Hans" ||
        locale.starts_with(L"zh-Hans-");
}

// Every German region (de-AT, de-CH and others) uses the de-DE resources.
bool Localization::IsGermanLocale(const std::wstring& locale)
{
    return locale == L"de" || locale.starts_with(L"de-");
}

// Korean language variants use the ko-KR resources.
bool Localization::IsKoreanLocale(const std::wstring& locale)
{
    return locale == L"ko" || locale.starts_with(L"ko-");
}

std::wstring Localization::Get(const wchar_t* key) const
{
    if (!key || !*key) return L"";

    const Table& selected = m_localeName == L"zh-CN" ? SimplifiedChineseTable() : m_localeName == L"zh-TW"
        ? TraditionalChineseTable() : m_localeName == L"de-DE" ? GermanTable()
        : m_localeName == L"ko-KR" ? KoreanTable() : EnglishTable();
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
    return m_localeName == L"zh-CN" ? L"Microsoft YaHei UI"
        : m_localeName == L"zh-TW" ? L"Microsoft JhengHei UI"
        : m_localeName == L"ko-KR" ? L"Malgun Gothic" : fallback;
}

} // namespace NitLink
