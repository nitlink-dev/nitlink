#include "app/config.h"
#include "capture/frame_buffer.h"
#include "common/input_limits.h"
#include "overlay/no_signal_image.h"
#include <windows.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
#include <thread>

using namespace NitLink;
using Microsoft::WRL::ComPtr;
static void Check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
static void Write(const std::filesystem::path& path, const std::string& body) {
    std::ofstream file(path, std::ios::binary); file << body;
    Check(file.good(), "fixture write");
}
static std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

static void ConfigTests(const std::filesystem::path& dir) {
    const auto path = dir / "config.ini";
    Config config;
    Write(path, "language = zh-CN\n");
    Check(config.Load(path.string()) && config.language == "zh-CN", "load Simplified Chinese preference");
    Check(config.Save(path.string()), "save Simplified Chinese preference");
    Config languageReload;
    Check(languageReload.Load(path.string()) && languageReload.language == "zh-CN",
          "Simplified Chinese preference survives restart");
    config = Config{};
    Write(path, "language = de-DE\n");
    Check(config.Load(path.string()) && config.language == "de-DE", "load German preference");
    Check(config.Save(path.string()), "save German preference");
    Config germanReload;
    Check(germanReload.Load(path.string()) && germanReload.language == "de-DE",
          "German preference survives restart");
    Write(path, "language = ko-KR\n");
    Check(config.Load(path.string()) && config.language == "ko-KR", "load Korean preference");
    Check(config.Save(path.string()), "save Korean preference");
    Config koreanReload;
    Check(koreanReload.Load(path.string()) && koreanReload.language == "ko-KR",
          "Korean preference survives restart");
    config = Config{};
    Write(path, "audio_volume = nan\npip_opacity = inf\nnis_sharpness = -inf\nwindow_width = 444junk\npip_x = 23junk\n");
    Check(config.Load(path.string()), "load malformed values");
    Check(config.audioVolume == 1 && config.pipOpacity == 0.9f && config.nisSharpness == Config{}.nisSharpness &&
        config.windowWidth == 444 && config.pipX == 23, "finite checks retain defaults; integers accept leading number");
    Write(path, "present_cap_hz = -1   # no cap\nwindow_width = 2560 # ultrawide\n"
                "audio_volume = 0.5 # gain\naspect_ratio = 4 : 3 # classic\n"
                "no_signal_image = C:\\photo#1.png\n");
    Check(config.Load(path.string()) && config.presentCapHz == -1 && config.windowWidth == 2560 &&
          config.audioVolume == 0.5f && config.aspectRatio == "4:3" &&
          config.noSignalImage == "C:\\photo#1.png", "numeric comments and spaced aspect preserve path hashes");
    Write(path, "present_cap_hz = 117 Hz\nwindow_width = 1920.0\naudio_volume = 0.3 gain\n");
    Check(config.Load(path.string()) && config.presentCapHz == 117 && config.windowWidth == 1920 &&
          config.audioVolume == 0.3f, "legacy numeric prefixes accept units and decimals");
    config = Config{};
    const auto invalid = "audio_volume = 0.25\n" + std::string(128 * 1024 + 1, 'x');
    Write(path, invalid);
    Check(!config.Load(path.string()) && config.audioVolume == 1, "long line rejects before mutation");
    const auto backup = std::filesystem::path(config.RecoveryBackup());
    Check(backup == path.wstring() + L".bak" && Read(backup) == invalid, "failed load creates bounded backup");
    const auto stamp = std::filesystem::last_write_time(backup);
    for (unsigned i = 0; i < 6; ++i)
        Check(!config.Load(path.string()) && config.RecoveryBackup() == backup.wstring(), "same rejected bytes reuse backup");
    Check(std::filesystem::last_write_time(backup) == stamp, "unchanged rejected config does not rewrite backup");
    std::string last = invalid;
    for (unsigned i = 0; i < 4; ++i) {
        const auto next = invalid + std::to_string(i);
        Write(path, next);
        Check(!config.Load(path.string()) && Read(backup) == next &&
              Read(path.wstring() + L".bak.1") == last, "two-copy rotation retains distinct content");
        last = next;
    }
    size_t backupCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
        if (entry.path().filename().wstring().starts_with(L"config.ini.bak")) {
            ++backupCount;
            Check(entry.file_size() <= 1024 * 1024, "backup size is capped");
        }
    Check(backupCount == 2, "rejected loads retain only two managed backups");
    Write(path, std::string(1024 * 1024 + 1, 'x'));
    Check(!config.Load(path.string()) && config.RecoveryBackup().empty(), "oversized config is not copied");
    const auto rejected = Read(path);
    for (unsigned i = 0; i < 3; ++i) Check(!config.Load(path.string()), "oversized reload stays rejected");
    Check(!config.Save(path.string()) && Read(path) == rejected && Read(backup) == last,
          "oversized failed load preserves original without growing backups");
    Config firstLaunch;
    const auto absent = dir / "absent-parent" / "nitlink.json";
    Check(!firstLaunch.Load(absent.string()) &&
          firstLaunch.GetLoadIssue() == Config::LoadIssue::FolderNotWritable &&
          firstLaunch.LastSaveFailed() && firstLaunch.RecoveryBackup().empty(),
          "failed initial creation identifies folder without claiming a backup");
    Write(path, "window_width = 2560\npreferred_device = legacy\x14name\naudio_volume = 0.25\n");
    Check(config.Load(path.string()) && config.windowWidth == 2560 && config.audioVolume == 0.25f &&
          config.preferredDevice.empty(), "legacy control byte skips only affected line");
    Check(config.Save(path.string()), "successful load re-enables saving");
    Write(path, "preferred_device = Card\ncapture_override.0.device = Card\ncapture_override.0.width = -1\n"
        "capture_override.0.height = 4000000000\ncapture_override.0.fps_numerator = 1000000000\n"
        "capture_override.0.fps_denominator = 1\ncapture_override.0.format = unknown\n"
        "capture_override.-1.device = Bad\ncapture_override.-1.width = 2\n"
        "capture_override.999999.device = Bad2\ncapture_override.999999.width = 2\n"
        "capture_override.1junk.device = Bad3\ncapture_override.1junk.width = 2\n"
        "no_signal_image = C:\\foo\rbar.png\ngame.test.unrecognized = true\n");
    Check(config.Load(path.string()), "override input load");
    auto format = config.GetOverride(L"Card");
    Check(format.width == 0 && format.height == 16384 && format.fpsNumerator == 0 && format.format.empty() &&
        config.captureFormatOverrides.size() == 1 && config.gameSettings.empty() && config.noSignalImage.empty(), "override bounds");
    config.preferredDevice = L"Capture \u6e2c\u8a66 \u010a";
    config.captureFormatOverrides.clear();
    config.captureFormatOverrides[config.preferredDevice] = {1920, 1080, 60, L"NV12", 60000, 1001};
    Check(config.Save(path.string()), "unicode save");
    Config restored;
    Check(restored.Load(path.string()) && restored.preferredDevice == config.preferredDevice &&
        restored.GetOverride(config.preferredDevice).fpsNumerator == 60000, "unicode round trip");
    const HANDLE shared = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(shared != INVALID_HANDLE_VALUE, "hold config as sync reader");
    config.audioVolume = 0.37f;
    const bool fallbackSaved = config.Save(path.string());
    CloseHandle(shared);
    Check(fallbackSaved && restored.Load(path.string()) && restored.audioVolume == 0.37f,
          "sharing-compatible save persists final settings");
    const HANDLE transient = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(transient != INVALID_HANDLE_VALUE, "hold transient scanner handle");
    std::thread scanner([transient] { Sleep(30); CloseHandle(transient); });
    config.audioVolume = 0.42f;
    const bool retried = config.Save(path.string());
    scanner.join();
    Check(retried && restored.Load(path.string()) && restored.audioVolume == 0.42f,
          "brief rename retry survives transient reader");
    const auto saved = Read(path);
    const auto linked = dir / "linked-config.ini";
    Check(CreateHardLinkW(linked.c_str(), path.c_str(), nullptr) != FALSE, "hard-link config fixture");
    const HANDLE linkedReader = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(linkedReader != INVALID_HANDLE_VALUE, "hold linked config reader");
    const bool linkedSaved = config.Save(path.string());
    CloseHandle(linkedReader);
    Check(!linkedSaved && Read(path) == saved && Read(linked) == saved,
          "in-place fallback refuses multiply linked files");
    std::filesystem::remove(linked);
    const HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "hold exclusive config reader");
    const bool lockedSaved = config.Save(path.string());
    CloseHandle(locked);
    Check(!lockedSaved && Read(path) == saved, "unwritable destination preserves original");
    const HANDLE laterScanner = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(laterScanner != INVALID_HANDLE_VALUE, "scanner after prior save failure");
    std::thread laterRelease([laterScanner] { Sleep(35); CloseHandle(laterScanner); });
    const bool laterSaved = config.Save(path.string());
    laterRelease.join();
    Check(laterSaved && !config.LastSaveFailed() && Read(path) == saved,
          "later saves retain rename retries after an earlier failure");

    const auto recovery = path.wstring() + L".save-recovery";
    Write(recovery, "window_width = 1234\n");
    Config recoveryLoad, independentSave;
    Check(recoveryLoad.Load(path.string()) && recoveryLoad.audioVolume == 0.42f &&
          recoveryLoad.windowWidth == config.windowWidth &&
          recoveryLoad.GetLoadIssue() == Config::LoadIssue::None &&
          recoveryLoad.SaveRecovery() == std::filesystem::absolute(recovery).wstring(),
          "leftover recovery warns with full path without ignoring saved settings");
    Check(recoveryLoad.Save(path.string()) && !recoveryLoad.LastSaveFailed() &&
          independentSave.Load(path.string()) && independentSave.Save(path.string()) &&
          Read(recovery) == "window_width = 1234\n",
          "existing different recovery copy survives successful saves from multiple instances");
    const auto beforeRecoveryFallback = Read(path);
    const HANDLE recoveryReader = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(recoveryReader != INVALID_HANDLE_VALUE, "hold reader with existing recovery copy");
    const bool recoveryFallback = recoveryLoad.Save(path.string());
    CloseHandle(recoveryReader);
    Check(!recoveryFallback && recoveryLoad.GetLoadIssue() == Config::LoadIssue::None &&
          Read(path) == beforeRecoveryFallback && Read(recovery) == "window_width = 1234\n",
          "fallback preserves an existing different recovery copy without latching saves off");
    Check(recoveryLoad.Save(path.string()) && !recoveryLoad.LastSaveFailed(),
          "saving resumes after sharing failure with recovery copy still present");
    Config anotherLaunch;
    Check(anotherLaunch.Load(path.string()) && anotherLaunch.audioVolume == 0.42f &&
          anotherLaunch.SaveRecovery() == std::filesystem::absolute(recovery).wstring(),
          "later launch loads settings and retains the nonblocking full-path recovery warning");
    Write(recovery, Read(path));
    Check(anotherLaunch.Load(path.string()) && anotherLaunch.SaveRecovery().empty() &&
          !std::filesystem::exists(recovery),
          "byte-identical restored recovery copy is removed silently");
    Write(recovery, Read(path));
    const HANDLE recoveryScanner = CreateFileW(recovery.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(recoveryScanner != INVALID_HANDLE_VALUE, "hold scanner on identical recovery copy");
    std::thread recoveryRelease([recoveryScanner] { Sleep(35); CloseHandle(recoveryScanner); });
    const bool recovered = anotherLaunch.Load(path.string());
    recoveryRelease.join();
    Check(recovered && anotherLaunch.SaveRecovery().empty() && !std::filesystem::exists(recovery),
          "recovery deletion retries a transient scanner handle");
    Write(recovery, Read(path));
    const HANDLE blockedRecovery = CreateFileW(recovery.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(blockedRecovery != INVALID_HANDLE_VALUE, "hold persistent recovery scanner");
    const bool stillLoaded = anotherLaunch.Load(path.string());
    const bool stillSaved = anotherLaunch.Save(path.string());
    CloseHandle(blockedRecovery);
    Check(stillLoaded && stillSaved && anotherLaunch.SaveRecovery().empty() &&
          !anotherLaunch.LastSaveFailed() && std::filesystem::exists(recovery),
          "blocked identical-copy deletion is silent and does not disable loading or saving");
    Check(anotherLaunch.Load(path.string()) && !std::filesystem::exists(recovery),
          "identical recovery is cleaned after scanner releases it");
    Check(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY), "read-only config fixture");
    const auto beforeDenied = GetTickCount64();
    bool denied = true;
    for (unsigned i = 0; i < 20; ++i) {
        Config fresh;
        denied &= fresh.Load(path.string()) && !fresh.Save(path.string()) && fresh.LastSaveFailed();
    }
    const auto deniedDuration = GetTickCount64() - beforeDenied;
    Check(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL), "restore test file attributes");
    Check(denied && deniedDuration < 700,
          "permanent read-only failure skips repeated render-thread retry sleeps");
    for (const auto& entry : std::filesystem::directory_iterator(dir))
        Check(!entry.path().filename().wstring().starts_with(L"config.ini.tmp-"),
              "failed saves leave no temporary siblings");
    // An exited child supplies a real stale PID without assuming a free PID.
    wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, MAX_PATH);
    std::wstring child = std::wstring(system) + L"\\cmd.exe";
    std::wstring command = L"\"" + child + L"\" /d /c exit 0";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    Check(CreateProcessW(child.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
          nullptr, nullptr, &startup, &process), "stale-temp process fixture");
    const auto stale = path.wstring() + L".tmp-" + std::to_wstring(process.dwProcessId) + L"-1-1";
    const auto active = path.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-1-1";
    const auto unrelated = path.wstring() + L".tmp-user-notes";
    Write(stale, "stale"); Write(active, "active"); Write(unrelated, "unrelated");
    Config cleaned;
    const bool livePreserved = cleaned.Load(path.string()) && std::filesystem::exists(stale);
    ResumeThread(process.hThread);
    Check(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0, "fixture child exits");
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    Check(livePreserved, "startup preserves another live process temporary file");
    const bool cleanLoad = cleaned.Load(path.string());
    Check(cleanLoad && !std::filesystem::exists(stale) && std::filesystem::exists(active) &&
          std::filesystem::exists(unrelated), "startup cleans only inactive application temporary files");
    std::filesystem::remove(active); std::filesystem::remove(unrelated);
    const auto beforeInvalidSave = Read(path);
    config.noSignalImage = "safe.png\naudio_volume = 0";
    Check(!config.Save(path.string()) && Read(path) == beforeInvalidSave, "invalid save preserves existing file");
    config.noSignalImage.clear();
    config.preferredDevice = L"bad\nwindow_width = 999";
    Check(!config.Save(path.string()) && Read(path) == beforeInvalidSave, "device cannot inject lines");
    config.preferredDevice.clear();
    config.language = config.aspectRatio = config.noSignalMode = config.noSignalFit =
        config.panelSide = config.noSignalImage = std::string(128 * 1024 - 128, 'a');
    config.captureFormatOverrides.clear();
    for (int i = 0; i < 128; ++i)
        config.captureFormatOverrides[std::wstring(4000, L'a') + std::to_wstring(i)] = {};
    Check(!config.Save(path.string()) && Read(path) == beforeInvalidSave, "oversized serialization preserves prior file");
    Check(ParseAspectRatio(" 4 : 3 # classic") > 1.33f && ParseAspectRatio("16:9") > 1.77f && ParseAspectRatio("stretch") == -1 &&
        ParseAspectRatio("nan:1") == 0 && ParseAspectRatio("1:inf") == 0 &&
        ParseAspectRatio("1 6:9") > 0.11f && ParseAspectRatio("16:9junk") > 1.77f && ParseAspectRatio("16:0") == 0, "aspect parser");
}

static void FrameTests() {
    uint32_t row = 0, bytes = 0;
    Check(FrameLayout(3840, 2160, PixelLayout::P010, row, bytes) && row == 7680 && bytes == 24883200, "4K P010 layout");
    Check(FrameLayout(7680, 4320, PixelLayout::Bgra, row, bytes) && bytes == 132710400, "8K BGRA layout");
    Check(!FrameLayout(UINT32_MAX, UINT32_MAX, PixelLayout::Bgra, row, bytes) && !row && !bytes, "overflow dimensions");
    Check(!FrameLayout(1921, 1080, PixelLayout::Nv12, row, bytes) &&
        !FrameLayout(16384, 16384, PixelLayout::Bgra, row, bytes) &&
        !FrameCapacity(1920, 1080, UINT32_MAX), "planar and allocation limits");
    FrameBuffer invalid(UINT32_MAX, UINT32_MAX, UINT32_MAX);
    Check(!invalid.IsValid(), "invalid buffer avoids allocation");
    FrameBuffer buffer(4, 4, 16);
    Check(buffer.IsValid(), "normal buffer");
    uint8_t data[65]{};
    FrameBuffer::FrameData frame;
    buffer.Write(nullptr, 64, 1); buffer.Write(data, 0, 1);
    Check(buffer.GetFramesWritten() == 0 && !buffer.Read(frame), "bad samples not published");
    data[0] = 17; buffer.Write(data, 65, 1);
    Check(buffer.Read(frame) && frame.size == 64 && frame.data[0] == 17,
          "padded BGRA frame clamps to owned capacity");
    invalid.Write(data, 65, 1);
    Check(invalid.GetFramesWritten() == 0, "invalid buffer remains unpublished");
    data[0] = 23; buffer.Write(data, 64, 2);
    Check(buffer.Read(frame) && frame.size == 64 && frame.data[0] == 23, "valid frame after bad samples");
}

static void MakeImage(IWICImagingFactory* factory, const std::filesystem::path& path, REFGUID container, UINT width = 2, UINT height = 2) {
    ComPtr<IWICStream> stream; ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame; ComPtr<IPropertyBag2> properties;
    Check(SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(container, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, &properties)) &&
        SUCCEEDED(frame->Initialize(properties.Get())) && SUCCEEDED(frame->SetSize(width, height)), "image encoder init");
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    Check(SUCCEEDED(frame->SetPixelFormat(&format)) && format == GUID_WICPixelFormat24bppBGR, "test image format");
    std::vector<BYTE> row(width * 3, 127);
    for (UINT y = 0; y < height; ++y)
        Check(SUCCEEDED(frame->WritePixels(1, width * 3, static_cast<UINT>(row.size()), row.data())), "image row write");
    Check(SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()), "image encoder commit");
}

static void ImageTests(const std::filesystem::path& dir) {
    ComPtr<IWICImagingFactory> factory;
    Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "WIC factory");
    const GUID containers[] = {GUID_ContainerFormatPng, GUID_ContainerFormatJpeg, GUID_ContainerFormatBmp};
    DecodedImage image;
    for (const auto& format : containers) {
        auto path = dir / L"image-\u6e2c\u8a66.dat";
        MakeImage(factory.Get(), path, format);
        Check(DecodeNoSignalImage(factory.Get(), path.wstring(), image) && image.width == 2 &&
            image.height == 2 && image.pixels.size() == 16, "supported image decoded by signature");
        const auto bytes = Read(path);
        Write(path, bytes.substr(0, 8));
        Check(!DecodeNoSignalImage(factory.Get(), path.wstring(), image) && image.pixels.empty(), "truncated image clears output");
    }
    for (const auto& container : {GUID_ContainerFormatJpeg, GUID_ContainerFormatBmp}) {
        const auto large = dir / "large-photo.dat";
        MakeImage(factory.Get(), large, container, 8000, 6000);
        if (container == GUID_ContainerFormatBmp)
            Check(std::filesystem::file_size(large) > 64 * 1024 * 1024, "encoded fixture exceeds former cap");
        Check(DecodeNoSignalImage(factory.Get(), large.wstring(), image) && image.width < 8000 &&
              image.height < 6000 && image.pixels.size() <= kMaxImagePixelBytes,
              "48 MP photo scales within pixel budget");
    }
    const auto path = dir / "oversized.bmp";
    MakeImage(factory.Get(), path, GUID_ContainerFormatBmp, 16385, 1);
    ImageLoadError error{};
    Check(!DecodeNoSignalImage(factory.Get(), path.wstring(), image, &error) &&
          error == ImageLoadError::TooLarge, "complete oversized image reports TooLarge");
    ImageLoadDiagnostic diagnostic;
    Check(!DecodeNoSignalImage(factory.Get(), (dir / "missing.png").wstring(), image, &error, &diagnostic) &&
          error == ImageLoadError::FileUnavailable && FAILED(diagnostic.result) &&
          std::wstring(diagnostic.operation) == L"SHCreateStreamOnFileEx",
          "missing file reports FileUnavailable and failing call/HRESULT");
    Check(NoSignalImageDrivePolicy(DRIVE_REMOTE) == ImageLoadError::NetworkLocation &&
          NoSignalImageDrivePolicy(DRIVE_NO_ROOT_DIR) == ImageLoadError::FileUnavailable &&
          NoSignalImageDrivePolicy(DRIVE_CDROM) == ImageLoadError::None,
          "mapped network, unplugged and optical drive policy");
    Write(path, "RIFF....WEBP");
    Check(!DecodeNoSignalImage(factory.Get(), path.wstring(), image, &error) &&
          error == ImageLoadError::UnsupportedFormat, "unsupported content reports exact reason");
    Check(!DecodeNoSignalImage(factory.Get(), L"\\\\server\\share\\photo.png", image, &error) &&
          error == ImageLoadError::NetworkLocation, "network image reports local-copy guidance");
    { std::ofstream file(path, std::ios::binary); file.seekp(kMaxImageFileBytes); file.put('x'); }
    Check(!DecodeNoSignalImage(factory.Get(), path.wstring(), image, &error) &&
          error == ImageLoadError::TooLarge, "large encoded file reports TooLarge");
    Check(!DecodeNoSignalImage(factory.Get(), L"\\\\server\\share\\photo.png", image) &&
        !DecodeNoSignalImage(factory.Get(), L"\\\\?\\C:\\photo.png", image) &&
        !DecodeNoSignalImage(factory.Get(), L"C:\\photo.png:payload", image) &&
        !DecodeNoSignalImage(factory.Get(), L"relative.png", image), "nonlocal and device paths rejected");
}

int main() {
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    const auto dir = std::filesystem::path(temp) / (L"NitLink-native-security-" + std::to_wstring(GetCurrentProcessId()));
    if (!CreateDirectoryW(dir.c_str(), nullptr)) return 1;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int result = 0;
    try { Check(SUCCEEDED(initialized), "COM init"); ConfigTests(dir); FrameTests(); ImageTests(dir);
        std::cout << "Configuration, frame, and image security tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    if (SUCCEEDED(initialized)) CoUninitialize();
    std::filesystem::remove_all(dir);
    return result;
}
