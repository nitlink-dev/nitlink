#include "config.h"
#include <fstream>
#include <filesystem>
#include <sstream>
#include <cmath>
#include <windows.h>
#include <algorithm>
#include <memory>
#include <atomic>

// Minimal key=value parser. NOT real JSON despite the .json file extension:
// the format is flat "key = value" lines with one extension: dotted keys
// for per-game settings, like:
//
//   game.spider-man-2.nis_enabled = true
//   game.spider-man-2.color_expansion = false

namespace NitLink {

// Walk past leading whitespace and return what's left.
static std::string LStrip(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// Strip trailing whitespace.
static std::string RStrip(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

static std::string Trim(const std::string& s) {
    return RStrip(LStrip(s));
}

static bool ParseBool(const std::string& v) {
    return v == "true" || v == "1" || v == "yes" || v == "on";
}

// Hand-edited numeric values accept a leading number and ignore trailing text.
// Invalid/non-finite values retain the default; valid numbers clamp to [lo,hi].
// Unsigned fields reject a leading '-' rather than accepting stoul's wraparound.
static std::string NumericText(const std::string& value) {
    return Trim(value.substr(0, value.find('#')));
}

static uint32_t ParseU32(const std::string& value, uint32_t def, uint32_t lo, uint32_t hi) {
    const auto v = NumericText(value);
    try {
        if (!v.empty() && v[0] == '-') return def;
        unsigned long n = std::stoul(v);
        if (n < lo) return lo;
        if (n > hi) return hi;
        return static_cast<uint32_t>(n);
    } catch (...) { return def; }
}

static int ParseI32(const std::string& value, int def, int lo, int hi) {
    const auto v = NumericText(value);
    try {
        int n = std::stoi(v);
        if (n < lo) return lo;
        if (n > hi) return hi;
        return n;
    } catch (...) { return def; }
}

static float ParseFloatClamped(const std::string& value, float def, float lo, float hi) {
    const auto v = NumericText(value);
    try {
        float n = std::stof(v);
        if (!std::isfinite(n)) return def;
        if (n < lo) return lo;
        if (n > hi) return hi;
        return n;
    } catch (...) { return def; }
}

namespace {
constexpr size_t kMaxConfigBytes = 1024 * 1024;
constexpr size_t kMaxConfigLine = 128 * 1024;

std::wstring UniqueFileSuffix() {
    static std::atomic<unsigned> sequence{0};
    return std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) +
        L"-" + std::to_wstring(++sequence);
}

void RemoveStaleConfigTemps(const std::filesystem::path& target) {
    std::error_code ec;
    const auto directory = target.has_parent_path() ? target.parent_path() : std::filesystem::path(".");
    const auto prefix = target.filename().wstring() + L".tmp-";
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().wstring();
        if (!name.starts_with(prefix)) continue;
        auto suffix = name.substr(prefix.size());
        if (suffix.ends_with(L".bak")) suffix.resize(suffix.size() - 4);
        const auto dash = suffix.find(L'-');
        if (dash == std::wstring::npos || dash == 0 ||
            suffix.find_first_not_of(L"0123456789-") != std::wstring::npos) continue;
        DWORD pid = 0;
        try { pid = std::stoul(suffix.substr(0, dash)); } catch (...) { continue; }
        if (!pid || pid == GetCurrentProcessId()) continue;
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (process) {
            const bool active = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
            CloseHandle(process);
            if (active) continue;
        } else if (GetLastError() != ERROR_INVALID_PARAMETER) {
            continue;
        }
        const DWORD attributes = GetFileAttributesW(it->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        // DeleteFile removes the temporary entry itself, including cloud placeholders.
        // Directories and active processes are never touched.
        DeleteFileW(it->path().c_str());
    }
}

bool ReadSmallConfig(const std::filesystem::path& path, std::string& contents) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const auto size = input.tellg();
    if (size < 0 || size > static_cast<std::streamoff>(kMaxConfigBytes)) return false;
    contents.resize(static_cast<size_t>(size));
    input.seekg(0);
    return !size || bool(input.read(contents.data(), size));
}

std::wstring PreserveConfigBackup(const std::filesystem::path& target) {
    // Two bounded copies replace the per-launch archive. Oversized or unreadable
    // originals stay in place, protected by the failed-load save guard.
    std::string contents, previous;
    if (!ReadSmallConfig(target, contents)) return {};
    const auto newest = target.wstring() + L".bak";
    const auto older = newest + L".1";
    for (const auto& backup : {newest, older})
        if (ReadSmallConfig(backup, previous) && contents == previous) return backup;
    const auto temporary = target.wstring() + L".tmp-" + UniqueFileSuffix() + L".bak";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    DWORD written = 0;
    const bool copied = WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) &&
                        written == contents.size();
    const bool closed = CloseHandle(file) != FALSE;
    bool rotated = false;
    if (copied && closed) {
        rotated = MoveFileExW(newest.c_str(), older.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
        if (!rotated) {
            const auto error = GetLastError();
            rotated = error == ERROR_FILE_NOT_FOUND;
        }
        if (rotated && MoveFileExW(temporary.c_str(), newest.c_str(), MOVEFILE_REPLACE_EXISTING))
            return newest;
    }
    DeleteFileW(temporary.c_str());
    return {};
}

bool IsConfigText(const std::string& value) {
    return value.size() <= kMaxConfigLine - 128 && std::none_of(value.begin(), value.end(),
        [](unsigned char c) { return c < 0x20 && c != '\t'; });
}
bool IsGameId(const std::string& id) {
    return id.size() <= 64 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
std::wstring ConfigWide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(count, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count)) return {};
    return result;
}
std::string ConfigUtf8(const std::wstring& value) {
    if (value.empty() || value.size() > kMaxConfigLine) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(count, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count, nullptr, nullptr)) return {};
    return result;
}
bool IsFormat(const std::wstring& value) {
    return value.empty() || value == L"NV12" || value == L"P010" ||
        value == L"BGRA" || value == L"RGB32";
}
void DeleteRecoveryCopy(const std::wstring& recovery) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (DeleteFileW(recovery.c_str())) return;
        const auto error = GetLastError();
        if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) return;
        if (attempt < 4) Sleep(20);
    }
}

// The fallback is used only after sharing-related rename failures. A disk file
// with one link and no reparse point is required before any in-place write.
bool SaveInPlace(const std::wstring& target, const std::string& contents, bool& recoveryRequired) {
    recoveryRequired = false;
    const auto recovery = target + L".save-recovery";
    const HANDLE raw = CreateFileW(target.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return false;
    const std::unique_ptr<void, decltype(&CloseHandle)> file(raw, &CloseHandle);
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(raw) != FILE_TYPE_DISK || !GetFileInformationByHandle(raw, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        info.nNumberOfLinks != 1 || info.nFileSizeHigh || info.nFileSizeLow > kMaxConfigBytes)
        return false;
    std::string previous(info.nFileSizeLow, '\0');
    DWORD read = 0;
    if (!ReadFile(raw, previous.data(), info.nFileSizeLow, &read, nullptr) || read != info.nFileSizeLow)
        return false;
    const HANDLE backup = CreateFileW(recovery.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (backup == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool backedUp = WriteFile(backup, previous.data(), static_cast<DWORD>(previous.size()), &written, nullptr) &&
        written == previous.size();
    const bool closed = CloseHandle(backup) != FALSE;
    if (!backedUp || !closed) {
        DeleteFileW(recovery.c_str());
        return false;
    }
    const auto replace = [&](const std::string& bytes) {
        DWORD count = 0;
        return SetFilePointerEx(raw, {}, nullptr, FILE_BEGIN) &&
            WriteFile(raw, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) &&
            count == bytes.size() && SetEndOfFile(raw);
    };
    if (replace(contents)) {
        DeleteRecoveryCopy(recovery);
        OutputDebugStringW(L"[NitLink/Config] Saved through sharing-compatible fallback.\n");
        return true;
    }
    const bool restored = replace(previous);
    recoveryRequired = !restored;
    if (restored) DeleteRecoveryCopy(recovery);
    OutputDebugStringW((L"[NitLink/Config] In-place save failed; recovery copy: " + recovery +
        (restored ? L" (original restored)\n" : L" (restore failed)\n")).c_str());
    return false;
}
} // namespace

void Config::CheckRecovery(const std::string& path) {
    m_saveRecovery.clear();
    const auto target = std::filesystem::path(path);
    const auto recovery = target.wstring() + L".save-recovery";
    if (GetFileAttributesW(recovery.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::string current, previous;
    if (ReadSmallConfig(target, current) && ReadSmallConfig(recovery, previous) && current == previous) {
        DeleteRecoveryCopy(recovery);
        return; // An identical copy needs no user action, even if deletion is blocked.
    }
    std::error_code error;
    const auto absolute = std::filesystem::absolute(recovery, error);
    m_saveRecovery = error ? recovery : absolute.wstring();
}

bool Config::Load(const std::string& path)
{
    RemoveStaleConfigTemps(std::filesystem::path(path));
    m_loadFailed = true;
    m_loadIssue = LoadIssue::ReadFailed;
    m_recoveryBackup.clear();
    CheckRecovery(path);
    bool backupAttempted = false;
    const auto preserve = [&]() {
        if (backupAttempted) return;
        backupAttempted = true;
        m_recoveryBackup = PreserveConfigBackup(std::filesystem::path(path));
        if (m_recoveryBackup.empty())
            OutputDebugStringW(L"[NitLink/Config] Backup unavailable or above size cap; original is retained.\n");
    };
    const auto failed = [&]() {
        preserve();
        OutputDebugStringW(L"[NitLink/Config] Load failed; saves are disabled to preserve the original.\n");
        return false;
    };
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) return failed();
    if (!exists) {
        manualHdmiSource = "auto";
        manualHdmiSourceCustom.clear();
        m_loadFailed = false;
        m_loadIssue = LoadIssue::None;
        if (Save(path)) return true;
        m_loadIssue = LoadIssue::FolderNotWritable;
        OutputDebugStringW(L"[NitLink/Config] Initial settings file could not be created in this folder.\n");
        return false;
    }

    // Read a bounded snapshot before parsing so oversized files and lines cannot
    // grow allocations or partly overwrite settings before rejection.
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return failed();
    const auto size = input.tellg();
    if (size < 0 || size > static_cast<std::streamoff>(kMaxConfigBytes)) return failed();
    std::string contents(static_cast<size_t>(size), '\0');
    input.seekg(0);
    if (size && !input.read(contents.data(), size)) return failed();
    size_t lineSize = 0;
    for (unsigned char c : contents) {
        if (c == '\n') lineSize = 0;
        else if (++lineSize > kMaxConfigLine) return failed();
    }
    std::istringstream file(contents);
    // Older files also restore Auto when this Config instance is reused.
    manualHdmiSource = "auto";
    manualHdmiSourceCustom.clear();

    // Pre-loop accumulators for capture-format override parsing. Two
    // schemas are accepted: the legacy flat keys from rc2 (one anonymous
    // override) and the new per-device indexed keys (rc3+). Both feed
    // into the post-loop reconciliation below.
    // present_pacing replaced the vrr_present_pacing boolean. Both are
    // accepted here and reconciled after the loop so key order in the file
    // does not decide the winner.
    bool sawPresentPacing  = false;
    bool sawLegacyVrrPacing = false;
    bool legacyVrrPacing    = false;
    bool sawLegacyOverride = false;
    CaptureFormatOverride legacyOverride;
    std::map<int, std::wstring>         indexedOverrideDevice;
    std::map<int, CaptureFormatOverride> indexedOverrides;

    std::string line;
    while (std::getline(file, line)) {
        line = Trim(line);
        if (!IsConfigText(line)) {
            preserve();
            continue;
        }
        if (line.empty() || line[0] == '#' || line[0] == '/') continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));

        // Per-game settings: "game.<id>.<setting> = value"
        // Split on dots and route into Config::gameSettings.
        if (key.rfind("game.", 0) == 0) {
            // After "game.", find the next dot to split id from setting name.
            auto dot = key.find('.', 5);
            if (dot == std::string::npos) continue;
            std::string gameId  = key.substr(5, dot - 5);
            std::string setting = key.substr(dot + 1);
            if (gameId.empty() || !IsGameId(gameId) ||
                (setting != "nis_enabled" && setting != "color_expansion")) continue;
            if (gameSettings.size() >= 1024 && !gameSettings.contains(gameId)) continue;

            // Auto-create the entry if needed
            auto& gs = gameSettings[gameId];

            if      (setting == "nis_enabled")     gs.nisEnabled     = ParseBool(val);
            else if (setting == "color_expansion") gs.colorExpansion = ParseBool(val);
            continue;
        }

        // Global settings (existing schema)
        if (key == "window_width")    windowWidth  = ParseU32(val, windowWidth, 320, 16384);
        if (key == "window_height")   windowHeight = ParseU32(val, windowHeight, 240, 16384);
        if (key == "language") {
            if (val == "system" || val == "en-US" || val == "zh-TW") language = val;
        }
        if (key == "manual_hdmi_source")
            manualHdmiSource = NormalizeManualHdmiSource(val);
        if (key == "manual_hdmi_source_custom")
            manualHdmiSourceCustom = NormalizeManualHdmiSourceCustom(val).value_or("");
        if (key == "pip_width")       pipWidth     = ParseU32(val, pipWidth, 80, 16384);
        if (key == "pip_height")      pipHeight    = ParseU32(val, pipHeight, 45, 16384);
        if (key == "pip_opacity")     pipOpacity   = ParseFloatClamped(val, pipOpacity, 0.0f, 1.0f);
        if (key == "pip_x")           pipX         = ParseI32(val, pipX, -100000, 100000);
        if (key == "pip_y")           pipY         = ParseI32(val, pipY, -100000, 100000);
        if (key == "audio_volume")    audioVolume  = ParseFloatClamped(val, audioVolume, 0.0f, 1.0f);
        if (key == "audio_muted")     audioMuted   = ParseBool(val);
        if (key == "color_expansion") colorExpansion = ParseBool(val);
        if (key == "nis_enabled")     nisEnabled   = ParseBool(val);
        if (key == "nis_scale_mode")  nisScaleMode = ParseI32(val, nisScaleMode, 0, 2);
        if (key == "nis_sharpness")   nisSharpness = ParseFloatClamped(val, nisSharpness, 0.0f, 1.0f);
        if (key == "hdr_enabled")     hdrEnabled   = ParseBool(val);
        if (key == "hdr_auto_from_source") hdrAutoFromSource = ParseBool(val);
        if (key == "present_pacing") {
            if      (val == "captured") presentPacing = kPacingCaptured;
            else if (val == "unique")   presentPacing = kPacingUnique;
            else                        presentPacing = kPacingRefresh;
            sawPresentPacing = true;
        }
        if (key == "vrr_present_pacing") {
            legacyVrrPacing    = ParseBool(val);
            sawLegacyVrrPacing = true;
        }
        if (key == "vsync")           vsync = ParseBool(val);
        if (key == "low_latency")     lowLatency = ParseBool(val);
        if (key == "prevent_sleep")  preventSleep = ParseBool(val);
        if (key == "present_cap_hz") presentCapHz = ParseI32(val, presentCapHz, -1, 1000);
        if (key == "aspect_ratio") {
            auto ratio = NumericText(val);
            const auto colon = ratio.find(':');
            if (colon != std::string::npos)
                ratio = Trim(ratio.substr(0, colon)) + ":" + Trim(ratio.substr(colon + 1));
            aspectRatio = ratio.substr(0, 16);
        }
        if (key == "no_signal_mode") {
            noSignalMode = (val == "image") ? "image" : "default";
        }
        if (key == "no_signal_image") noSignalImage = val;
        if (key == "no_signal_fit") {
            noSignalFit = (val == "cover" || val == "stretch") ? val : "contain";
        }
        if (key == "no_signal_dim_image") noSignalDimImage = ParseBool(val);
        if (key == "panel_side")     panelSide    = val.substr(0, 8);
        if (key == "panel_width")    panelWidth   = ParseI32(val, panelWidth, 320, 1200);
        if (key == "enable_shaders")  enableShaders = ParseBool(val);
        if (key == "show_overlay")    showOverlay   = ParseBool(val);
        if (key == "current_game" && IsGameId(val)) currentGameId = val;
        if (key == "preferred_device") {
            if (val.size() <= 4096) preferredDevice = ConfigWide(val);
        }

        // Capture format overrides. Two schemas accepted:
        //   Legacy (rc2): flat capture_override_{width,height,fps,format}
        //     keys, one anonymous override for the whole config.
        //   New (rc3+): indexed capture_override.<N>.{device,width,
        //     height,fps,format} keys per saved device. Each card
        //     remembers its own pick.
        // Both accumulate into pre-loop temps and are reconciled below.
        if (key == "capture_override_width") {
            legacyOverride.width = ParseU32(val, 0, 0, 16384);
            sawLegacyOverride = true;
        } else if (key == "capture_override_height") {
            legacyOverride.height = ParseU32(val, 0, 0, 16384);
            sawLegacyOverride = true;
        } else if (key == "capture_override_fps") {
            legacyOverride.fps = ParseU32(val, 0, 0, 1000);
            sawLegacyOverride = true;
        } else if (key == "capture_override_fps_numerator") {
            legacyOverride.fpsNumerator = ParseU32(val, 0, 0, 1000000000);
            sawLegacyOverride = true;
        } else if (key == "capture_override_fps_denominator") {
            legacyOverride.fpsDenominator = ParseU32(val, 1, 1, 1000000000);
            sawLegacyOverride = true;
        } else if (key == "capture_override_format") {
            const auto format = ConfigWide(val);
            if (IsFormat(format)) legacyOverride.format = format;
            sawLegacyOverride = true;
        } else if (key.rfind("capture_override.", 0) == 0) {
            // capture_override.<N>.<field> = <value>
            // capture_override.count is parsed but ignored: the map's
            // contents post-loop are the source of truth for size.
            const auto remainder = key.substr(17);  // length of "capture_override."
            const auto dot = remainder.find('.');
            if (dot != std::string::npos) {
                try {
                    size_t consumed = 0;
                    const int idx = std::stoi(remainder.substr(0, dot), &consumed);
                    if (consumed != dot || idx < 0 || idx >= 128) continue;
                    const std::string field = remainder.substr(dot + 1);
                    if (field == "device") {
                        if (val.size() <= 4096) indexedOverrideDevice[idx] = ConfigWide(val);
                    } else if (field == "width") {
                        indexedOverrides[idx].width = ParseU32(val, 0, 0, 16384);
                    } else if (field == "height") {
                        indexedOverrides[idx].height = ParseU32(val, 0, 0, 16384);
                    } else if (field == "fps") {
                        indexedOverrides[idx].fps = ParseU32(val, 0, 0, 1000);
                    } else if (field == "fps_numerator") {
                        indexedOverrides[idx].fpsNumerator =
                            ParseU32(val, 0, 0, 1000000000);
                    } else if (field == "fps_denominator") {
                        indexedOverrides[idx].fpsDenominator =
                            ParseU32(val, 1, 1, 1000000000);
                    } else if (field == "format") {
                        const auto format = ConfigWide(val);
                        if (IsFormat(format)) indexedOverrides[idx].format = format;
                    }
                } catch (...) {
                    // Malformed index, skip the line silently.
                }
            }
        }
    }

    auto normalizeOverrideRate = [](CaptureFormatOverride& ov) {
        if (static_cast<uint64_t>(ov.fpsNumerator) > 1000ull * ov.fpsDenominator) {
            ov.fpsNumerator = 0;
            ov.fps = 0;
        }
        if (ov.fpsNumerator > 0) {
            if (ov.fpsDenominator == 0) ov.fpsDenominator = 1;
            ov.fps = ov.fpsNumerator / ov.fpsDenominator;
        } else {
            // Legacy configs stored only the integer display FPS. Keep the
            // rational unspecified so capture negotiation can resolve e.g.
            // 59 against a native 60000/1001 mode instead of inventing 59/1.
            ov.fpsDenominator = 1;
        }
    };

    normalizeOverrideRate(legacyOverride);
    for (auto& [idx, ov] : indexedOverrides) {
        normalizeOverrideRate(ov);
    }

    // Post-loop: transpose indexed overrides into the per-device map.
    // Entries without a device name are dropped (incomplete record).
    for (const auto& [idx, ov] : indexedOverrides) {
        const auto deviceIt = indexedOverrideDevice.find(idx);
        if (deviceIt == indexedOverrideDevice.end() || deviceIt->second.empty()) continue;
        if (captureFormatOverrides.size() >= 128 && !captureFormatOverrides.contains(deviceIt->second)) continue;
        captureFormatOverrides[deviceIt->second] = ov;
    }

    // Legacy migration: if pre-rc3 flat keys were present AND the new
    // indexed schema was empty, attribute the legacy override to the
    // currently preferred device. preferred_device is parsed earlier in
    // the file so it is already set by this point. If preferred_device
    // is empty, the legacy override is discarded (no device to attribute
    // it to).
    if (sawLegacyOverride && captureFormatOverrides.empty() && !preferredDevice.empty()) {
        captureFormatOverrides[preferredDevice] = legacyOverride;
    }

    // Legacy migration: vrr_present_pacing was a boolean that meant "gate
    // Present on the frame differ", which is kPacingUnique here. It only
    // applies when the file carried no present_pacing key of its own.
    if (!sawPresentPacing && sawLegacyVrrPacing && legacyVrrPacing) {
        presentPacing = kPacingUnique;
    }

    m_loadFailed = false;
    m_loadIssue = LoadIssue::None;
    m_lastSaveFailed = false;
    return true;
}

bool Config::Save(const std::string& path)
{
    m_lastSaveFailed = true;
    if (m_loadFailed) {
        OutputDebugStringW(L"[NitLink/Config] Save skipped because the configuration failed to load.\n");
        return false;
    }
    const auto preferredUtf8 = ConfigUtf8(preferredDevice);
    if ((!preferredDevice.empty() && preferredUtf8.empty()) ||
        preferredUtf8.size() > 4096 || !IsConfigText(preferredUtf8) ||
        !IsGameId(currentGameId) || !IsConfigText(noSignalImage) ||
        !IsConfigText(language) || !IsConfigText(aspectRatio) ||
        !IsConfigText(noSignalMode) || !IsConfigText(noSignalFit) || !IsConfigText(panelSide) ||
        gameSettings.size() > 1024 || captureFormatOverrides.size() > 128 ||
        !std::isfinite(audioVolume) || !std::isfinite(pipOpacity) || !std::isfinite(nisSharpness))
        return false;
    for (const auto& [id, settings] : gameSettings) if (!IsGameId(id)) return false;
    for (const auto& [device, format] : captureFormatOverrides) {
        const auto name = ConfigUtf8(device);
        if (device.empty() || name.empty() || name.size() > 4096 ||
            !IsConfigText(name) || !IsFormat(format.format)) return false;
    }

    std::ostringstream file;

    file << "# NitLink Configuration\n";
    file << "# https://github.com/nitlink-dev/nitlink\n\n";

    file << "# Interface language: system | en-US | zh-TW\n";
    file << "language = " << language << "\n\n";

    file << "# Window\n";
    file << "window_width = "  << windowWidth  << "\n";
    file << "window_height = " << windowHeight << "\n\n";

    file << "# Picture-in-Picture\n";
    file << "pip_width = "   << pipWidth   << "\n";
    file << "pip_height = "  << pipHeight  << "\n";
    file << "pip_opacity = " << pipOpacity << "\n";
    file << "pip_x = "       << pipX       << "\n";
    file << "pip_y = "       << pipY       << "\n\n";

    file << "# Audio\n";
    file << "audio_volume = " << audioVolume << "\n";
    file << "audio_muted = "  << (audioMuted ? "true" : "false") << "\n\n";

    file << "# Capture\n";
    file << "# Friendly name of the preferred capture device, e.g. \"Elgato 4K Pro\".\n";
    file << "# Empty falls back to the first Elgato device when present, otherwise to\n";
    file << "# the first device Media Foundation enumerates.\n";
    file << "preferred_device = " << preferredUtf8 << "\n\n";

    file << "# HDMI IN source identity only; does not change capture or HDR policy.\n";
    file << "# auto | ps5 | ps4 | switch2 | switch | xbox_series | xbox_one | pc | other\n";
    file << "manual_hdmi_source = " << NormalizeManualHdmiSource(manualHdmiSource) << "\n";
    file << "# Custom name for Other, at most 64 Unicode characters, no control characters.\n";
    file << "manual_hdmi_source_custom = "
         << NormalizeManualHdmiSourceCustom(manualHdmiSourceCustom).value_or("") << "\n\n";

    file << "# Capture format overrides (per device, F1 Source picker)\n";
    file << "# Schema: capture_override.<N>.<field> = <value>\n";
    file << "# Fields per entry: device, width, height, fps, fps_numerator,\n";
    file << "# fps_denominator, format\n";
    file << "# Numeric fields at 0 (or empty for format) mean Auto.\n";
    file << "# Format value: NV12 / P010 / BGRA / (empty for Auto).\n";
    if (!captureFormatOverrides.empty()) {
        file << "capture_override.count = " << captureFormatOverrides.size() << "\n";
        int idx = 0;
        for (const auto& [device, ov] : captureFormatOverrides) {
            const std::string narrowDevice = ConfigUtf8(device);
            const std::string narrowFormat = ConfigUtf8(ov.format);
            file << "capture_override." << idx << ".device = " << narrowDevice << "\n";
            file << "capture_override." << idx << ".width = "  << ov.width  << "\n";
            file << "capture_override." << idx << ".height = " << ov.height << "\n";
            file << "capture_override." << idx << ".fps = "    << ov.fps    << "\n";
            file << "capture_override." << idx << ".fps_numerator = "
                 << ov.fpsNumerator << "\n";
            file << "capture_override." << idx << ".fps_denominator = "
                 << ov.fpsDenominator << "\n";
            file << "capture_override." << idx << ".format = " << narrowFormat << "\n";
            ++idx;
        }
    }
    file << "\n";

    file << "# Display\n";
    file << "color_expansion = " << (colorExpansion ? "true" : "false") << "\n\n";

    file << "# Image Upscaling (NIS)\n";
    file << "nis_enabled = "    << (nisEnabled ? "true" : "false") << "\n";
    file << "nis_scale_mode = " << nisScaleMode << "\n";
    file << "nis_sharpness = "  << nisSharpness << "\n\n";

    file << "# HDR\n";
    file << "hdr_enabled = " << (hdrEnabled ? "true" : "false") << "\n";
    file << "# When true, auto-detect HDR pipeline based on detected HDMI source\n";
    file << "# identifier on Elgato 4K S (e.g. PS5 -> assume HDR-capable, default to HDR).\n";
    file << "# Set false to keep classic config-driven behavior (hdr_enabled alone decides).\n";
    file << "hdr_auto_from_source = " << (hdrAutoFromSource ? "true" : "false") << "\n\n";

    file << "# Present pacing: refresh | captured | unique\n";
    file << "# refresh  = Present every loop iteration, so the present rate\n";
    file << "#            tracks the display refresh rate. Lowest latency.\n";
    file << "# captured = Present once per frame the card delivers, so the\n";
    file << "#            present rate follows the HDMI cadence, usually 60.\n";
    file << "# unique   = Present only on frames the GPU differ classifies as\n";
    file << "#            new content, so the present rate follows the real\n";
    file << "#            source frame rate. This is what a variable refresh\n";
    file << "#            display and an external frame-generation tool both\n";
    file << "#            need, and it removes 30 fps judder against a\n";
    file << "#            present rate that is not a multiple of the content.\n";
    file << "# Both paced modes add up to one capture interval of latency and\n";
    file << "# turn the present cap off. On a fixed refresh display, unique\n";
    file << "# drops low-motion content to the safety floor. Cycle from the F1\n";
    file << "# settings panel.\n";
    file << "present_pacing = "
         << (presentPacing == kPacingUnique   ? "unique"
           : presentPacing == kPacingCaptured ? "captured"
                                              : "refresh") << "\n\n";

    file << "# Low-latency present mode (Alt+L, default true)\n";
    file << "# When true, present each frame the instant it arrives for the\n";
    file << "# lowest input lag. When false, the frame is held after capture and\n";
    file << "# the swap-chain wait moves before present, so the picture is up to\n";
    file << "# one refresh older. VSync controls tearing independently.\n";
    file << "# Toggle from the F1 panel or with Alt+L.\n";
    file << "low_latency = " << (lowLatency ? "true" : "false") << "\n\n";

    file << "# VSync (Alt+V, default false): synchronize presentation to avoid\n";
    file << "# tearing. May add input delay; Low-Latency can stay enabled.\n";
    file << "vsync = " << (vsync ? "true" : "false") << "\n\n";

    file << "# Keep the display and PC awake while video is visible.\n";
    file << "# Disabled while minimized, hidden or showing No signal.\n";
    file << "prevent_sleep = " << (preventSleep ? "true" : "false") << "\n\n";

    file << "# Present-rate cap in Hz for the low-latency present (default 0)\n";
    file << "# 0 = automatic: monitor refresh minus 3, when that is at least the\n";
    file << "# source frame rate. 30-1000 = fixed cap. -1 = no cap.\n";
    file << "# Bypassed while VSync is enabled.\n";
    file << "present_cap_hz = " << presentCapHz << "\n\n";

    file << "# Display aspect ratio: auto (source ratio), stretch (fill the\n";
    file << "# window), or a fixed ratio such as 4:3, 16:9, 16:10, 21:9.\n";
    file << "# Cycle with Alt+A or from the F1 panel.\n";
    file << "aspect_ratio = " << aspectRatio << "\n\n";

    file << "# No Signal presentation: default uses NitLink's branded page;\n";
    file << "# image uses a local PNG, JPEG/JPG, or BMP file. The path is UTF-8.\n";
    file << "no_signal_mode = " << noSignalMode << "\n";
    file << "no_signal_image = " << noSignalImage << "\n";
    file << "# Image fit: contain | cover | stretch\n";
    file << "no_signal_fit = " << noSignalFit << "\n";
    file << "no_signal_dim_image = " << (noSignalDimImage ? "true" : "false") << "\n\n";

    file << "# F1 panel placement: right or left docks it beside the picture,\n";
    file << "# full covers the window. panel_width is in device-independent pixels.\n";
    file << "panel_side = " << panelSide << "\n";
    file << "panel_width = " << panelWidth << "\n\n";

    file << "# Shaders\n";
    file << "enable_shaders = " << (enableShaders ? "true" : "false") << "\n\n";

    file << "# Overlay\n";
    file << "show_overlay = " << (showOverlay ? "true" : "false") << "\n\n";

    // ===== Game state =====
    file << "# Current Game (empty if none selected)\n";
    file << "current_game = " << currentGameId << "\n\n";

    if (!gameSettings.empty()) {
        file << "# Per-Game Settings\n";
        file << "# Format: game.<game_id>.<setting> = value\n";
        for (const auto& [id, gs] : gameSettings) {
            file << "game." << id << ".nis_enabled = "     << (gs.nisEnabled ? "true" : "false") << "\n";
            file << "game." << id << ".color_expansion = " << (gs.colorExpansion ? "true" : "false") << "\n";
        }
        file << "\n";
    }

    const std::string contents = file.str();
    if (!file.good() || contents.size() > kMaxConfigBytes) return false;
    // Replace only a fully written sibling file. A failed write leaves the
    // previous configuration intact, and CREATE_NEW avoids following a link.
    const auto target = std::filesystem::path(path).wstring();
    const DWORD attributes = GetFileAttributesW(target.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY)) {
        OutputDebugStringW(L"[NitLink/Config] Read-only config: save skipped without retry.\n");
        return false;
    }
    const auto temporary = target + L".tmp-" + UniqueFileSuffix();
    const auto complete = [&]() {
        m_lastSaveFailed = false;
        m_loadIssue = LoadIssue::None;
        CheckRecovery(path);
        return true;
    };
    HANDLE output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool saved = WriteFile(output, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) &&
        written == contents.size();
    const bool closed = CloseHandle(output) != FALSE;
    if (saved && closed) {
        DWORD error = ERROR_SUCCESS;
        // Brief retries cover transient scanner/sync handles without forcing a
        // disk flush on every render-thread toggle.
        constexpr int attempts = 5;
        for (int attempt = 0; attempt < attempts; ++attempt) {
            if (MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) return complete();
            error = GetLastError();
            if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) break;
            if (attempt + 1 < attempts) Sleep(20);
        }
        bool recoveryRequired = false;
        if ((error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION) &&
            SaveInPlace(target, contents, recoveryRequired)) {
            DeleteFileW(temporary.c_str());
            return complete();
        }
        if (recoveryRequired) {
            m_loadFailed = true;
            m_loadIssue = LoadIssue::RecoveryRequired;
            std::error_code recoveryError;
            const auto recovery = std::filesystem::absolute(target + L".save-recovery", recoveryError);
            m_recoveryBackup = recoveryError ? target + L".save-recovery" : recovery.wstring();
        } else {
            CheckRecovery(path);
        }
        DeleteFileW(temporary.c_str());
        OutputDebugStringW(L"[NitLink/Config] Save failed; original settings and any recovery copy retained.\n");
        return false;
    }
    DeleteFileW(temporary.c_str());
    OutputDebugStringW(L"[NitLink/Config] Temporary write failed; original settings retained.\n");
    return false;
}

} // namespace NitLink
