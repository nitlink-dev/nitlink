#pragma once

#include "hdmi_source.h"

#include <map>
#include <string>
#include <cstdint>

namespace NitLink {

// Per-game settings overlay. When a user picks a game from the dropdown,
// THIS subset of the main Config is saved under that game's id. Next time
// they select the same game, these values override the global defaults.
//
// Why only a subset and not the full Config? Because window size,
// audio device, hotkey state, etc. are per-user-machine, not per-game.
// What changes per game is the picture pipeline.
struct GameSettings {
    bool        nisEnabled     = false;
    bool        colorExpansion = false;
};

// Capture format override: user-selected resolution / fps / pixel format
// from the F1 Source picker. Each numeric field at 0 means "Auto" for that
// dimension; empty format string means "Auto" for format. All-Auto means
// full automatic negotiation (the default).
//
// The override is best-effort: if the requested combination is unavailable
// for the live source, CaptureDevice falls back to Auto and surfaces a
// toast notification via the JSON state push. The override is preserved in
// config across that fallback so the next source change retries it; the
// user has to explicitly switch a dropdown back to Auto to remove it.
struct CaptureFormatOverride {
    uint32_t     width  = 0;
    uint32_t     height = 0;
    uint32_t     fps    = 0;
    std::wstring format;  // "NV12" / "P010" / "BGRA" / "" for Auto
    // Preserve the native Media Foundation rational rate. `fps` remains the
    // integer display/backward-compatibility value; a non-zero numerator is
    // the wire/config identity used for exact manual selection.
    uint32_t     fpsNumerator = 0;
    uint32_t     fpsDenominator = 1;

    bool isFullAuto() const {
        return width == 0 && height == 0 && fps == 0 &&
               fpsNumerator == 0 && format.empty();
    }
};

// How often the render loop presents a frame. See Config::presentPacing.
enum PresentPacing {
    kPacingRefresh  = 0,
    kPacingCaptured = 1,
    kPacingUnique   = 2,
};

struct Config {
    // UI language preference: system, en-US, or zh-TW. The effective locale
    // is resolved by Localization; unsupported system locales use en-US.
    std::string  language = "system";

    // Window
    uint32_t windowWidth  = 1920;
    uint32_t windowHeight = 1080;
    bool preventSleep = true;
    
    // PiP mode
    uint32_t pipWidth   = 480;
    uint32_t pipHeight  = 270;
    float    pipOpacity = 0.9f;
    // Persisted PiP top-left screen coordinates. -1 / -1 means "no saved
    // position": Window::SetPiP falls back to the monitor's bottom-right
    // default. Updated each time the user nudges the PiP window via the
    // Ctrl/Ctrl+Shift+Arrow hotkeys; written to disk on Application::Shutdown
    // through the existing Config::Save path.
    int32_t  pipX       = -1;
    int32_t  pipY       = -1;
    
    // Audio
    std::wstring audioOutputDevice = L""; // Empty = system default
    float        audioVolume       = 1.0f;
    bool         audioMuted        = false;

    // Capture
    std::wstring preferredDevice = L""; // Empty = first available

    // Global identity of the device connected to HDMI IN, independent of
    // the capture card, per-device format overrides, and HDR signal state.
    std::string manualHdmiSource = "auto";
    std::string manualHdmiSourceCustom;
    bool SetManualHdmiSource(std::string_view value) {
        if (!IsValidManualHdmiSource(value)) return false;
        manualHdmiSource = value;
        return true;
    }
    bool SetManualHdmiSourceCustom(std::string_view value) {
        const auto normalized = NormalizeManualHdmiSourceCustom(value);
        if (!normalized) return false;
        manualHdmiSourceCustom = *normalized;
        return true;
    }
    bool SetManualHdmiSourceCustom(std::wstring_view value) {
        const auto normalized = NormalizeManualHdmiSourceCustom(value);
        if (!normalized) return false;
        manualHdmiSourceCustom = *normalized;
        return true;
    }

    // Manual capture format overrides, keyed by capture device name.
    // Each card remembers its own pick (4K Pro at 4K, 4K S at 1080p+240,
    // etc.). Missing entry = all-Auto = full automatic negotiation.
    // Lookup via GetOverride() below; write via captureFormatOverrides[name].
    std::map<std::wstring, CaptureFormatOverride> captureFormatOverrides;

    // Read-only lookup for the per-device override map. Returns the
    // saved override for `deviceName`, or all-Auto defaults if no entry
    // exists. Does NOT insert. Use captureFormatOverrides[name]
    // directly when writing.
    CaptureFormatOverride GetOverride(const std::wstring& deviceName) const {
        auto it = captureFormatOverrides.find(deviceName);
        if (it == captureFormatOverrides.end()) return {};
        return it->second;
    }

    // Display pipeline knobs
    bool         colorExpansion  = false;  // Limited (16-235) -> full (0-255). Default OFF: PS5 over HDMI typically sends full range, and applying expansion to full-range data crushes blacks. Toggle ON only if your blacks look gray.

    // HDR output: when enabled and the display supports HDR, the renderer
    // switches to FP16 backbuffer with scRGB colorspace.
    bool         hdrEnabled      = false;

    // HDR auto-from-source: fallback heuristic when direct HDR signal
    // detection is unavailable (HID error, MCU non-responsive, or a
    // device family without a detection path). When the connected
    // source identifier matches a known HDR-capable console (PS5,
    // future Xbox Series X|S, Switch 2), default the pipeline to HDR
    // even without a direct signal-state confirmation. User can still
    // Alt+H to switch to SDR for the current session. Set to false to
    // keep purely config-driven behavior (hdrEnabled alone decides).
    bool         hdrAutoFromSource = true;

    // Present pacing: how often the render loop presents a frame.
    //
    //   kPacingRefresh   Present every loop iteration, so the present rate
    //                    tracks the display refresh rate. Lowest latency,
    //                    and the default.
    //   kPacingCaptured  Present once per frame the card delivers. The loop
    //                    blocks on the capture buffer, so the present rate
    //                    follows the HDMI delivery cadence, typically 60 Hz.
    //   kPacingUnique    Present only when the GPU frame differ classifies a
    //                    delivered frame as new content, so the present rate
    //                    follows the real source frame rate.
    //
    // kPacingUnique is what a variable refresh display needs in order to
    // follow the source instead of the card's constant delivery rate
    // (verified on an LG C3 OLED via its Game Dashboard refresh-rate
    // indicator), and what an external frame-generation tool needs in order
    // to read the real frame rate: such tools derive it from the present
    // rate, so presenting every refresh reports the panel rate to them and
    // leaves nothing to interpolate. It also removes the judder a 30 fps
    // source shows against a present rate that is not a multiple of it.
    //
    // Both paced modes cost up to one capture interval of added latency and
    // turn the present cap off, because the source cadence already sits
    // below the panel rate. On a fixed refresh display, kPacingUnique drops
    // low-motion content such as a game intro to the safety floor, because
    // the differ-classified rate there falls to near zero.
    //
    // Persisted as present_pacing = refresh | captured | unique. Configs
    // written before that key existed are migrated from vrr_present_pacing.
    int          presentPacing = kPacingRefresh;

    // Low-latency present mode (Alt+L, default ON). When ON: wait the swap
    // chain at the top of the loop, then read the freshest captured frame and
    // present it on arrival for the lowest input lag. When OFF: the frame is
    // read first and the swap-chain wait moves into BeginFrame, so the held
    // frame ages up to one refresh before it is presented. The sync
    // interval is controlled separately by vsync. Default ON keeps the
    // capture read as late as possible.
    bool         lowLatency = true;

    // Alt+V / F1: synchronize presentation to avoid tearing on fixed-refresh
    // displays. Kept independent of capture pacing and low-latency reads.
    bool         vsync = false;

    // Present-rate cap for the low-latency tearing-allowed present, in Hz.
    // 0 = automatic: the window's monitor refresh rate minus 3, applied only
    // when that stays at or above the source frame rate, so a variable
    // refresh display engages VRR and a fixed refresh display never drops
    // frames. 30 to 1000 = fixed cap. -1 = no cap. A VRR_CAP.txt file next
    // to the exe overrides these rates. VSync bypasses the tearing-mode cap.
    int          presentCapHz = 0;

    // Display aspect ratio. "auto" shows the source at the ratio the card
    // reports. A fixed ratio ("4:3", "16:9", "16:10", "21:9", or any "W:H")
    // squeezes or letterboxes the picture to that shape, for sources the
    // card delivers stretched, such as 4:3 consoles inside a 16:9 frame.
    // "stretch" fills the window and ignores the ratio entirely.
    std::string  aspectRatio = "auto";

    // Presentation-only replacement for NitLink's branded No Signal page.
    // The path is persisted as UTF-8 and converted to UTF-16 only at the
    // Windows/WIC boundary. These settings never alter capture aspect ratio.
    std::string  noSignalMode = "default"; // default | image
    std::string  noSignalImage = {};        // UTF-8 filesystem path
    std::string  noSignalFit = "contain";  // contain | cover | stretch
    bool         noSignalDimImage = true;

    // Where the F1 panel opens. "right" and "left" dock it as a strip beside
    // the picture, which keeps playing underneath. "full" covers the whole
    // window with the wide layout and holds a black frame while it is open.
    std::string  panelSide  = "right";
    // Width of the docked panel in device-independent pixels.
    int          panelWidth = 420;

    // (hdrMode string field stripped: Reference/Vibrant was an earlier
    //  fake-HDR pipeline. The current path negotiates real HDR10 via
    //  P010 capture, with the Elgato hardware tone-map disabled at
    //  startup through IKsPropertySet on the 4K Pro and a vendor HID
    //  Output Report on the 4K S.)

    // Image upscaling (NIS)
    bool         nisEnabled      = false;
    int          nisScaleMode    = 2;     // 0=1.5x, 1=2x, 2=Match window (NIS supports 1x..2x only)
    float        nisSharpness    = 0.5f;  // 0..1
    
    // Shader pipeline
    bool         enableShaders  = false;
    std::wstring shaderProfile  = L"default";
    
    // Performance overlay
    bool showOverlay = false;
    
    // PSN (for game detection)
    std::string psnNpsso = ""; // Auth token
    
    // Hotkeys
    bool enableGlobalHotkeys = true;

    // ===== Per-game settings =====
    //
    // The currently selected game id (matches a GameEntry::id from
    // game_database.h), or empty string if no game is selected.
    // When non-empty AND gameSettings[currentGameId] exists, those values
    // override the global nisEnabled/colorExpansion above when
    // applied at startup or game-change time.
    std::string  currentGameId = "";

    // Per-game settings keyed by game id. Persisted in the config file as
    // flat keys: "game.spider-man-2.nis_enabled = true", etc.
    std::map<std::string, GameSettings> gameSettings;

    bool Load(const std::string& path);
    bool Save(const std::string& path);
    enum class LoadIssue { None, ReadFailed, FolderNotWritable, RecoveryRequired };
    LoadIssue GetLoadIssue() const { return m_loadIssue; }
    bool LastSaveFailed() const { return m_lastSaveFailed; }
    const std::wstring& RecoveryBackup() const { return m_recoveryBackup; }
    const std::wstring& SaveRecovery() const { return m_saveRecovery; }

private:
    LoadIssue m_loadIssue = LoadIssue::None;
    bool m_lastSaveFailed = false;
    std::wstring m_recoveryBackup;
    std::wstring m_saveRecovery;
    // A rejected input must survive later routine and shutdown saves.
    bool m_loadFailed = false;
    void CheckRecovery(const std::string& path);
};

} // namespace NitLink
