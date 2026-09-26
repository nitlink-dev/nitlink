#include "application.h"
#include "capture_device_names.h"
#include "WebViewSettings.h"
#include "game_database.h"
#include "localization.h"
#include "capture_output_policy.h"
#include "capture/elgato_hdr_control.h"
#include "capture/elgato_device_identity.h"
#include "capture/elgato_hid_4ks.h"
#include <chrono>
#include <thread>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>       // FrameLevels histograms (HDR levels readout)
#include <cstdint>      // fixed-width pixel-code types
#include <cassert>      // assert: frame-buffer teardown invariant (worker joined)
#include <debugapi.h>
#include <shlobj.h>
#include <shellapi.h>   // ShellExecuteW: openScreenshotFolder dispatch
#pragma comment(lib, "shell32.lib")

namespace NitLink {

static void AppLog(const std::wstring& msg) {
    OutputDebugStringW((L"[NitLink/App] " + msg + L"\n").c_str());
}

static void PlaceholderLog(const std::wstring& msg) {
    OutputDebugStringW((L"[NitLink/Placeholder] " + msg + L"\n").c_str());
}

static const wchar_t* PlaceholderFormatName(
    PlaceholderDetector::CaptureFormatKind format)
{
    switch (format) {
    case PlaceholderDetector::CaptureFormatKind::P010: return L"P010";
    case PlaceholderDetector::CaptureFormatKind::NV12: return L"NV12";
    case PlaceholderDetector::CaptureFormatKind::BGRA: return L"BGRA";
    }
    return L"Unknown";
}

static const wchar_t* PresentationStateName(PresentationState state)
{
    switch (state) {
    case PresentationState::WaitingForCapture: return L"WaitingForCapture";
    case PresentationState::Capture: return L"Capture";
    case PresentationState::NoSignal: return L"NoSignal";
    case PresentationState::Transition: return L"Transition";
    }
    return L"Unknown";
}

static const wchar_t* PlaceholderClassificationName(
    PlaceholderDetector::FrameClassification classification)
{
    switch (classification) {
    case PlaceholderDetector::FrameClassification::Real: return L"Real";
    case PlaceholderDetector::FrameClassification::CandidatePlaceholder:
        return L"CandidatePlaceholder";
    case PlaceholderDetector::FrameClassification::ConfirmedPlaceholder:
        return L"ConfirmedPlaceholder";
    case PlaceholderDetector::FrameClassification::InvalidTransitionalFrame:
        return L"InvalidTransitionalFrame";
    }
    return L"Unknown";
}

static PlaceholderDetector::PlaceholderDeviceFamily PlaceholderFamilyForDevice(
    const std::wstring& deviceName)
{
    if (GetCaptureDevicePolicy(deviceName).family ==
        CaptureDeviceFamily::AverMediaGC553Pro) {
        return PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro;
    }
    if (IsElgatoDevice(deviceName)) {
        return PlaceholderDetector::PlaceholderDeviceFamily::Elgato;
    }
    return PlaceholderDetector::PlaceholderDeviceFamily::Unknown;
}

static bool TryPlaceholderFormatForSubtype(
    const GUID& subtype, PlaceholderDetector::CaptureFormatKind& format)
{
    if (IsEqualGUID(subtype, MFVideoFormat_P010)) {
        format = PlaceholderDetector::CaptureFormatKind::P010;
        return true;
    }
    if (IsEqualGUID(subtype, MFVideoFormat_NV12)) {
        format = PlaceholderDetector::CaptureFormatKind::NV12;
        return true;
    }
    if (IsEqualGUID(subtype, MFVideoFormat_RGB32) ||
        IsEqualGUID(subtype, MFVideoFormat_ARGB32)) {
        format = PlaceholderDetector::CaptureFormatKind::BGRA;
        return true;
    }
    return false;
}

static std::wstring Tr(const wchar_t* key) {
    return Localization::Instance().Get(key);
}

static std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), count) != count) {
        return {};
    }
    return result;
}

static std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), count,
            nullptr, nullptr) != count) {
        return {};
    }
    return result;
}

const wchar_t* FormatGuidToString(const GUID& g);

static std::wstring P010UnavailableWarning(const CaptureFormat& format)
{
    return Localization::Instance().Format(
        L"toast.p010Unavailable",
        {{L"format", FormatGuidToString(format.subtype)}});
}

static std::wstring FormatNoticeFrameRate(const P010SelectionNotice& notice)
{
    if (notice.fpsNumerator == 0 || notice.fpsDenominator == 0) {
        return std::to_wstring(notice.fps);
    }

    if (notice.fpsNumerator % notice.fpsDenominator == 0) {
        return std::to_wstring(notice.fpsNumerator / notice.fpsDenominator);
    }

    // Keep the native rational internally, but show a compact user-facing
    // decimal (for example, 60000/1001 as 59.94 rather than a raw fraction).
    const uint64_t hundredths =
        (static_cast<uint64_t>(notice.fpsNumerator) * 100 +
         notice.fpsDenominator / 2) / notice.fpsDenominator;
    const uint64_t whole = hundredths / 100;
    const uint64_t fraction = hundredths % 100;
    if (fraction == 0) return std::to_wstring(whole);

    std::wstring result = std::to_wstring(whole) + L".";
    if (fraction < 10) result += L"0";
    result += std::to_wstring(fraction);
    if (result.back() == L'0') result.pop_back();
    return result;
}

static std::wstring P010SelectionWarning(const P010SelectionNotice& notice)
{
    return Localization::Instance().Format(
        L"toast.p010SelectionFallback",
        {{L"width", std::to_wstring(notice.width)},
         {L"height", std::to_wstring(notice.height)},
         {L"fps", FormatNoticeFrameRate(notice)}});
}

static CaptureFormatPreference FormatPreferenceForOverride(
    const CaptureFormatOverride& formatOverride)
{
    if (formatOverride.format.empty()) {
        return CaptureFormatPreference::Auto;
    }
    if (formatOverride.format == L"NV12") {
        return CaptureFormatPreference::ManualNV12;
    }
    if (formatOverride.format == L"P010") {
        return CaptureFormatPreference::ManualP010;
    }
    return CaptureFormatPreference::ManualOther;
}

static std::wstring ManualFormatHDRWarning(const std::wstring& format)
{
    return Localization::Instance().Format(
        L"toast.manualFormatHdrRequiresP010",
        {{L"format", format}});
}

void Application::UpdateCaptureColorInterpretation(const std::wstring& deviceName) {
    if (!m_renderer) return;
    const bool fullRange = EffectiveSourceFullRange();
    const bool limitedChroma = UsesStandardLimitedP010Chroma(
        deviceName, m_lastCaptureIsP010, fullRange);
    m_renderer->SetSourceFullRange(fullRange);
    m_renderer->SetP010LimitedChroma(limitedChroma);

    std::wstringstream ss;
    const bool gc553ProSdrP010 = m_isGC553Pro && m_config &&
        m_config->hdrAutoFromSource && m_lastCaptureIsP010 &&
        m_gc553ProSourceState == Gc553ProSourceHdrState::Sdr;
    ss << L"Capture interpretation: "
       << (gc553ProSdrP010 ? L"P010 (GC553Pro SDR transfer unverified)"
           : m_lastCaptureIsP010 ? L"P010 PQ / BT.2020" : L"non-P010 path")
       << L"; device='" << deviceName << L"'"
       << L"; MF range or fallback=" << (m_lastMfFullRange ? L"FULL" : L"LIMITED")
       << L"; effective luma range=" << (fullRange ? L"FULL" : L"LIMITED")
       << L"; P010 chroma=" << (limitedChroma ? L"STANDARD LIMITED (MK.2 correction ON)"
                                            : L"existing behavior (MK.2 correction OFF)")
       << L"; override=" << m_sourceRangeOverride << L" (0 Auto, 1 Full, 2 Limited)";
    AppLog(ss.str());
}

// ---- HDR levels readout (Ctrl+F6) -------------------------------------
// Measures the captured frame's code range so the correct per-card color
// range can be read off the signal instead of eyeballed. NitLink's decode
// uses the luma range plus a model-specific limited-chroma policy for MK.2
// P010 capture. The luma floor indicates the source luma range: ~64 (10-bit) / ~16 (8-bit)
// means a limited-range source, ~0 means full range. Chroma min/max is
// reported too (informational; needs saturated content to be meaningful).
struct FrameLevels {
    bool valid      = false;
    bool eightBit   = false;   // NV12 (8-bit codes) vs P010 (10-bit codes)
    int  yFloor = 0, yCeil = 0;
    bool haveChroma = false;
    int  cbLo = 0, cbHi = 0, crLo = 0, crHi = 0;
};

// Robust floor/ceiling from a code histogram: ignore the bottom/top 0.1% so
// a handful of stuck pixels cannot flip the verdict. binCount = 256 or 1024.
static void HistFloorCeil(const uint32_t* hist, int binCount,
                          uint64_t total, int& floorOut, int& ceilOut) {
    floorOut = 0;
    ceilOut  = binCount - 1;
    if (total == 0) return;
    const uint64_t cut = total / 1000;   // 0.1% tail on each end
    uint64_t acc = 0;
    for (int i = 0; i < binCount; ++i) {
        acc += hist[i];
        if (acc > cut) { floorOut = i; break; }
    }
    acc = 0;
    for (int i = binCount - 1; i >= 0; --i) {
        acc += hist[i];
        if (acc > cut) { ceilOut = i; break; }
    }
}

static FrameLevels ComputeFrameLevels(const uint8_t* data, uint32_t size,
                                      uint32_t width, uint32_t height,
                                      bool isP010, bool isNV12) {
    FrameLevels lv;
    if (!data || width < 16 || height < 16) return lv;
    if (!isP010 && !isNV12) return lv;   // BGRA is an RGB path; no YUV range

    // Subsample to ~250k luma samples regardless of resolution so the 4 Hz
    // scan stays well under a millisecond even at 4K.
    const uint64_t pixels = static_cast<uint64_t>(width) * height;
    uint32_t step = static_cast<uint32_t>(pixels / 250000);
    if (step < 1) step = 1;

    const int bins = isP010 ? 1024 : 256;
    std::vector<uint32_t> yh(bins, 0), cbh(bins, 0), crh(bins, 0);
    uint64_t yCount = 0, cCount = 0;

    if (isP010) {
        // Y plane: pixels * 16-bit LE samples, 10-bit code in the high bits.
        const uint64_t yBytes = pixels * 2;
        for (uint64_t i = 0; i < pixels; i += step) {
            const uint64_t off = i * 2;
            if (off + 1 >= size) break;
            const uint32_t v = (data[off] | (data[off + 1] << 8)) >> 6;
            yh[v & 1023]++; ++yCount;
        }
        // UV plane: interleaved (Cb,Cr) 16-bit pairs at half resolution.
        const uint64_t chromaBytes = (size > yBytes) ? (size - yBytes) : 0;
        const uint64_t pairs = chromaBytes / 4;
        for (uint64_t p = 0; p < pairs; p += step) {
            const uint64_t off = yBytes + p * 4;
            if (off + 3 >= size) break;
            const uint32_t cb = (data[off]     | (data[off + 1] << 8)) >> 6;
            const uint32_t cr = (data[off + 2] | (data[off + 3] << 8)) >> 6;
            cbh[cb & 1023]++; crh[cr & 1023]++; cCount += 2;
        }
    } else {
        // NV12: 8-bit Y plane (pixels bytes), then interleaved (Cb,Cr) 8-bit.
        for (uint64_t i = 0; i < pixels; i += step) {
            if (i >= size) break;
            yh[data[i]]++; ++yCount;
        }
        const uint64_t chromaBytes = (size > pixels) ? (size - pixels) : 0;
        const uint64_t pairs = chromaBytes / 2;
        for (uint64_t p = 0; p < pairs; p += step) {
            const uint64_t off = pixels + p * 2;
            if (off + 1 >= size) break;
            cbh[data[off]]++; crh[data[off + 1]]++; cCount += 2;
        }
    }

    if (yCount == 0) return lv;
    lv.eightBit = !isP010;
    HistFloorCeil(yh.data(), bins, yCount, lv.yFloor, lv.yCeil);
    if (cCount > 0) {
        HistFloorCeil(cbh.data(), bins, cCount / 2, lv.cbLo, lv.cbHi);
        HistFloorCeil(crh.data(), bins, cCount / 2, lv.crLo, lv.crHi);
        lv.haveChroma = true;
    }
    lv.valid = true;
    return lv;
}

// One-line readout: measured signal range (sig:) next to the shader's active
// decode (dec:), so signal-truth and the Alt+R setting compare at a glance.
static std::wstring FormatLevelsText(const FrameLevels& lv,
                                     bool effectiveFullRange) {
    if (!lv.valid) {
        return Tr(L"diagnostic.levelsNoRange");
    }
    // Verdict from the luma floor. Limited-range black sits at 64 (10-bit) /
    // 16 (8-bit); full range bottoms at 0. The gap is wide, so a midpoint
    // split is unambiguous unless the scene carries no true black.
    const int limBlack = lv.eightBit ? 16 : 64;
    const int midpoint = limBlack / 2;                 // 8 (8b) / 32 (10b)
    const int hiTol    = lv.eightBit ? 28 : 112;       // limited-black + slack
    std::wstring sig;
    if      (lv.yFloor <= midpoint) sig = Tr(L"value.full");
    else if (lv.yFloor <= hiTol)    sig = Tr(L"value.limited");
    else                            sig = Tr(L"value.needBlack");

    std::wstringstream ss;
    ss << Tr(L"diagnostic.levelsY") << L" " << lv.yFloor << L"-" << lv.yCeil
       << L"  " << Tr(L"diagnostic.levelsSignal") << L":" << sig
       << L"  " << Tr(L"diagnostic.levelsDecode") << L":"
       << (effectiveFullRange ? Tr(L"value.full") : Tr(L"value.limited"));
    if (lv.haveChroma) {
        ss << L"  " << Tr(L"diagnostic.levelsCb") << L" " << lv.cbLo << L"-" << lv.cbHi
           << L" " << Tr(L"diagnostic.levelsCr") << L" " << lv.crLo << L"-" << lv.crHi;
    }
    if (lv.eightBit) ss << L" (" << Tr(L"diagnostic.bits8") << L")";
    return ss.str();
}

// Strip vendor-specific suffix from a video device name so the result
// matches the paired audio endpoint's substring. AverMedia exposes
// "Live Gamer 4K 2.1-Video" for video and "HDMI (Live Gamer 4K 2.1-Audio)"
// for audio; stripping the suffix leaves "Live Gamer 4K 2.1" which appears
// in both. Elgato uses no suffix so this is a no-op for Elgato. Empty
// input falls back to "Elgato" as a sensible default for the most
// common card.
static std::wstring DeriveAudioHint(const std::wstring& videoDeviceName)
{
    std::wstring hint = videoDeviceName;
    const wchar_t* suffixesToStrip[] = { L"-Video", L"-VIDEO", L" Video" };
    for (const wchar_t* sfx : suffixesToStrip) {
        const size_t sfxLen = wcslen(sfx);
        if (hint.size() >= sfxLen &&
            hint.compare(hint.size() - sfxLen, sfxLen, sfx) == 0) {
            hint.resize(hint.size() - sfxLen);
            break;
        }
    }
    if (hint.empty()) hint = L"Elgato";
    return hint;
}

// Escape a wide string for safe inclusion inside a JSON string literal.
// Handles the JSON-required escapes (" and \), the seven standard
// control-character escapes (\b \f \n \r \t), and any other control
// codepoint below 0x20 via \uXXXX. Non-control Unicode codepoints
// >= 0x20 pass through unchanged: the JSON spec accepts those directly
// in string content. WebView2's PostWebMessageAsJson handles the
// UTF-16 -> UTF-8 transcode on the wire.
//
// Used for capture-device friendly names pushed to the settings panel.
// MF-reported names are almost always ASCII, but escaping
// unconditionally keeps the JSON valid against any future driver that
// includes a quote, backslash, or non-Latin character.
static std::wstring JsonEscapeWide(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size() + 4);
    for (wchar_t c : s) {
        switch (c) {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\b': out += L"\\b";  break;
            case L'\f': out += L"\\f";  break;
            case L'\n': out += L"\\n";  break;
            case L'\r': out += L"\\r";  break;
            case L'\t': out += L"\\t";  break;
            default:
                if (c < 0x20) {
                    // Control codepoint outside the seven named
                    // escapes. Codepoints 0x00..0x1F always fit in
                    // two hex digits prefixed with "00".
                    static const wchar_t hex[] = L"0123456789abcdef";
                    out += L"\\u00";
                    out.push_back(hex[(c >> 4) & 0xF]);
                    out.push_back(hex[c & 0xF]);
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}


Application::Application() = default;
Application::~Application() { Shutdown(); }

bool Application::Initialize(HINSTANCE hInstance, int nCmdShow)
{
    AppLog(L"Initialize: begin");

    // Load configuration. Tracks whether this was a fresh install (no
    // config file yet) so the settings menu can be opened automatically
    // on first launch: guides new users to the feature toggles instead
    // of leaving them on a black "NO SIGNAL" screen wondering what to do.
    m_firstLaunch = !std::filesystem::exists("nitlink.json");
    m_config = std::make_unique<Config>();
    m_config->Load("nitlink.json");
    Localization::Instance().SetPreference(m_config->language);
    AppLog(m_firstLaunch ? L"Initialize: config loaded (first launch)"
                             : L"Initialize: config loaded");

    m_window = std::make_unique<Window>();
    Window::Desc windowDesc{};
    windowDesc.title    = L"NitLink";
    windowDesc.width    = m_config->windowWidth;
    windowDesc.height   = m_config->windowHeight;
    windowDesc.darkMode = true;

    if (!m_window->Create(hInstance, windowDesc)) {
        AppLog(L"Initialize: Window::Create FAILED");
        return false;
    }
    m_window->SetPreventSleep(m_config->preventSleep);

    auto [actualW, actualH] = m_window->GetClientSize();
    // Window may not have processed WM_SIZE yet after Show(), so GetClientSize()
    // can return garbage. Reject anything outside a sane monitor size range
    // and fall back to the requested size.
    if (actualW < 8 || actualH < 8 || actualW > 16384 || actualH > 16384) {
        actualW = windowDesc.width  >= 8 ? windowDesc.width  : 1280;
        actualH = windowDesc.height >= 8 ? windowDesc.height : 720;
    }
    {
        std::wstringstream ss;
        ss << L"Initialize: client size " << actualW << L"x" << actualH;
        AppLog(ss.str());
    }

    m_renderer = std::make_unique<DX11Renderer>();
    if (!m_renderer->Initialize(m_window->GetHWND(), actualW, actualH)) {
        AppLog(L"Initialize: Renderer::Initialize FAILED");
        return false;
    }
    AppLog(L"Initialize: renderer initialized");

    // Keep the HWND hidden until the renderer and a minimal D2D/DirectWrite
    // overlay are ready. This prevents the uninitialized client area from
    // flashing white during potentially slow capture-device bring-up.
    // Present an explicit startup state before making the window visible.
    m_overlay = std::make_unique<Overlay>();
    const bool startupOverlayOk = m_overlay->Initialize(
        m_renderer->GetDevice(), m_renderer->GetContext(),
        m_renderer->GetSwapChain(), m_window->GetHWND());
    if (!startupOverlayOk) {
        AppLog(L"Initialize: startup Overlay::Initialize FAILED (showing dark renderer clear)");
    } else {
        m_renderer->BeginFrame(false);
        m_overlay->DrawStatusMessage(actualW, actualH,
                                     L"overlay.initializingCapture");
        if (m_overlay->IsUsingOffscreen()) {
            m_renderer->CompositeUI(m_overlay->GetOffscreenSRV());
        }
        m_renderer->EndFrame();
    }
    m_showOverlay = startupOverlayOk && m_config->showOverlay;
    m_window->Show(nCmdShow);
    AppLog(L"Initialize: startup frame presented and window shown");
    if (startupOverlayOk) ApplyNoSignalSettings();

    auto devices = DeviceEnumerator::FindCaptureDevices();
    m_cachedCaptureDeviceNames.clear();
    m_cachedCaptureDeviceNames.reserve(devices.size());
    for (const auto& device : devices) {
        m_cachedCaptureDeviceNames.push_back(device.name);
    }
    {
        std::wstringstream ss;
        ss << L"Initialize: found " << devices.size() << L" capture device(s)";
        AppLog(ss.str());
    }
    if (devices.empty()) {
        AppLog(L"Initialize: no capture devices found");
        return false;
    }

    // Pick which device to open. The user's preferred_device field in
    // nitlink.json takes priority through PickPreferredDevice's exact-
    // then-substring match. Empty preferred name (first run) or a no-
    // match (saved device unplugged since last launch) falls back to a
    // small bias toward Elgato cards: when a laptop has both an
    // integrated webcam and an Elgato connected, the webcam tends to
    // win MF enumeration order without this bias and the user ends up
    // viewing their own face instead of the capture source.
    int chosenIdx = DeviceEnumerator::PickPreferredDevice(
        devices, m_config ? m_config->preferredDevice : std::wstring{});
    if (chosenIdx < 0 || static_cast<size_t>(chosenIdx) >= devices.size()) {
        chosenIdx = 0;
    }
    const DeviceInfo& chosen = devices[chosenIdx];
    {
        std::wstringstream ss;
        ss << L"Initialize: selected capture device '" << chosen.name
           << L"' (index " << chosenIdx << L" of " << devices.size() << L")";
        if (m_config && !m_config->preferredDevice.empty()) {
            ss << L" [preferred='" << m_config->preferredDevice << L"']";
        }
        AppLog(ss.str());
    }

    // Gate the Elgato-specific control calls (IKsPropertySet HDR
    // tonemap toggle, HDR InfoFrame property read, 4K S vendor HID
    // Output Report) on whether the selected device is actually an
    // Elgato card. Without this gate, those calls would self-no-op on
    // webcams and third-party capture cards (their internal device-
    // name and VID/PID filters reject non-matches) but would still
    // generate "property not supported" log lines for every launch
    // against a non-Elgato source.
    const bool isElgato = IsElgatoDevice(chosen.name);
    AppLog(isElgato
        ? L"Initialize: device name recognized for Elgato controls; HDR query support still requires a successful probe"
        : L"Initialize: device name does not match an Elgato control identity");
    std::wstring chosenLower = chosen.name;
    std::transform(chosenLower.begin(), chosenLower.end(), chosenLower.begin(), ::towlower);
    m_is4KS = isElgato && chosenLower.find(L"4k s") != std::wstring::npos;
    m_isGC553Pro = GetCaptureDevicePolicy(chosen.name).family ==
                   CaptureDeviceFamily::AverMediaGC553Pro;
    if (m_isGC553Pro) {
        AppLog(L"Initialize: AVerMedia GC553Pro recognized; probing source HDR via XU mailbox");
    }
    if (!isElgato) {
        AppLog(L"Initialize: selected device is not Elgato; skipping Elgato-specific HDR controls");
    }

    // Before opening the device for Media Foundation capture, talk to
    // the Elgato driver via DirectShow's IKsPropertySet and disable
    // the internal HDR->SDR tonemapping. The setting persists in
    // driver state across the DS->MF transition, so when
    // CaptureDevice::Open negotiates a Media Foundation session right
    // after this, the card will pass raw HDR10 data (P010-encoded,
    // BT.2020 PQ) instead of pre-tonemapped SDR.
    if (isElgato) {
        if (DisableElgatoTonemap(chosen.name)) {
            AppLog(L"Initialize: Elgato hardware tonemap disabled (raw HDR10 mode)");
        } else {
            AppLog(L"Initialize: Elgato tonemap toggle skipped or unsupported");
        }
    }

    // Read the HDR InfoFrame the source is sending the Elgato.
    // The pipeline uses this for auto-detect: if the source is HDR10, P010
    // will be negotiated from MF; if it's SDR, BGRA is used.
    //
    // Auto-detect may fail on some Elgato device classes (e.g. 4K S over
    // USB doesn't expose the property GUID the 4K Pro uses for this query).
    // When detection is unavailable, the user's hdrEnabled config preference
    // is trusted instead. When hdr_auto_from_source is enabled in
    // config and a known HDR-capable HDMI source is detected via the 4K S
    // vendor HID (e.g. PlayStation 5), the source identity becomes the
    // proxy for "user probably wants HDR". P010 negotiation can still fail
    // at the MF level, in which case the fallback path below catches it.
    if (isElgato) {
        HDRSourceInfo srcInfo = ReadElgatoHDRSource(chosen.name);
        m_sourceIsHDR10        = srcInfo.isHDR10;
        m_hdrDetectionAvailable = srcInfo.propertyAccessible;
        if (srcInfo.propertyAccessible) {
            // 4K Pro path: IKsPropertySet GUID supplied direct HDR state.
            AppLog(srcInfo.isHDR10
                ? L"Initialize: source detected as HDR10 (PQ), will request P010 capture"
                : L"Initialize: source detected as SDR, will use BGRA capture");
        } else {
            // 4K S path: IKsPropertySet GUID is not supported. Fall
            // through to the 4K S vendor HID flow below which provides
            // both the source label and direct HDR signal state.
            AppLog(L"Initialize: 4K Pro property GUID unavailable on this device; trying 4K S vendor HID path");
            m_sourceIsHDR10 = false;
        }

        // 4K S vendor HID flow: source identifier + direct HDR signal
        // state. Runs only when the 4K Pro property GUID was unavailable
        // (4K S, and any other Elgato variant that lacks the GUID).
        // On the 4K Pro the IKsPropertySet path above already settled
        // m_sourceIsHDR10 and m_hdrDetectionAvailable, so this block
        // is skipped.
        //
        //   1. Detect4KSHdmiSource: 2 HID ops, returns source label for
        //                           window title and heuristic fallback.
        //   2. Probe4KSHdrMetadata: 3 HID ops (SET 0x13 refresh +
        //                           1.5s wait + QUERY 0x09 + GET),
        //                           returns direct HDR signal-state
        //                           truth from the MCU's HDR Metadata
        //                           register.
        //   3. Set4KSTonemap:       1 HID op, fires below.
        //
        // Total 6 HID transactions per process lifetime, with the
        // probe's required 1.5s mid-sleep as built-in pacing between
        // bursts. The 4K S firmware tolerates this op count when the
        // pacing is preserved.
        //
        // When the probe succeeds, m_hdrDetectionAvailable is promoted
        // to true (overriding the false set above by ReadElgatoHDRSource)
        // and m_sourceIsHDR10 reflects probe.hdrActive. This bypasses
        // the source-ID heuristic block further down and lets the
        // signal-state probe drive useP010 directly.
        //
        // When the probe fails (HID error, MCU non-responsive), the
        // source-ID heuristic remains the fallback.
        if (!srcInfo.propertyAccessible && m_is4KS) {
            static bool s_4ksVendorHidFired = false;
            if (!s_4ksVendorHidFired) {
                s_4ksVendorHidFired = true;

                const HdmiSourceInfo info = Detect4KSHdmiSource();
                if (info.detected) {
                    m_detectedHdmiSource = info.label;
                    AppLog(L"Initialize: Detected HDMI source: " + info.label);
                } else {
                    AppLog(L"Initialize: HDMI source not identified via vendor HID");
                }

                const HdrMetadataProbeResult probe = Probe4KSHdrMetadata();
                if (probe.queryOk) {
                    m_sourceIsHDR10         = probe.hdrActive;
                    m_hdrDetectionAvailable = true;
                    // Probe success is a 4K-S-specific signal: the
                    // probe matches VID 0x0FD9 + PID 0x00AE/0x00AF only.
                    // Used below to gate the 4K-S-specific resolution
                    // clamp and the userWantsHDR-gated wantP010 logic.
                    m_is4KS = true;
                    std::wstringstream ss;
                    ss << L"Initialize: 4K S vendor HID HDR probe -> "
                       << (probe.hdrActive ? L"HDR ACTIVE" : L"SDR")
                       << L" (byte[1]=0x"
                       << std::hex << std::setw(2) << std::setfill(L'0')
                       << static_cast<unsigned>(probe.raw[1])
                       << L", EOTF=0x"
                       << std::setw(2) << std::setfill(L'0')
                       << static_cast<unsigned>(probe.eotf)
                       << L")";
                    AppLog(ss.str());
                } else {
                    AppLog(L"Initialize: 4K S vendor HID HDR probe unavailable; falling back to source-ID heuristic / hdr_enabled config");
                }
            }
        }

        // Note: title bar update is deferred until AFTER CaptureDevice::Open
        // below. UpdateWindowTitle reads the actual negotiated subtype to
        // decide between [HDR] and [SDR], so it has to wait for format
        // negotiation. See the UpdateWindowTitle call further down.
    } else if (m_isGC553Pro) {
        Gc553ProHdrReader reader(chosen.name);
        const auto probe = reader.Read();
        m_gc553ProSourceState = probe.state;
        m_hdrDetectionAvailable = probe.state != Gc553ProSourceHdrState::Unknown;
        m_sourceIsHDR10 = probe.state == Gc553ProSourceHdrState::Hdr10Pq;
        AppLog(m_hdrDetectionAvailable
            ? L"Initialize: GC553Pro valid source EOTF received"
            : L"Initialize: GC553Pro source state unknown; retaining manual startup policy");
    } else {
        // Non-Elgato source has no InfoFrame property. Default to SDR
        // detection state; the user's hdrEnabled config can still
        // force P010 negotiation at the MF level if they explicitly
        // request it, and MF will reject P010 cleanly on a webcam
        // through the existing retry-to-NV12 fallback below.
        m_sourceIsHDR10 = false;
        m_hdrDetectionAvailable = false;
    }

    // Cache the chosen device so ReconcileCaptureFormat can re-Open
    // the same device when a format swap is needed at runtime (Alt+H or
    // source SDR<->HDR transitions).
    m_currentDeviceInfo = chosen;

    m_captureDevice = std::make_unique<CaptureDevice>();

    // Decide capture format BEFORE opening. P010 is requested when:
    //   1. Auto-detect found an HDR10 source (4K Pro path), AND user has HDR
    //      enabled in config; OR
    //   2. Auto-detect was unavailable (4K S: no property GUID) but user
    //      has HDR enabled in config: trust the user's preference.
    //
    // Either case false -> standard BGRA/NV12 SDR pipeline. The same decision
    // is reapplied at runtime by ReconcileCaptureFormat when the user
    // toggles HDR (Alt+H) or the source's HDR state changes (PS5 launching
    // an HDR game from an SDR dashboard). Reconcile handles the teardown
    // and re-Open; this initial pass just gets the pipeline to a working state.
    //
    // Elgato auto-detection remains unchanged. GC553Pro has its own decoded
    // source EOTF; Unknown falls back to the existing manual preference.
    const bool userWantsHDR = m_config && m_config->hdrEnabled;
    const CaptureFormatOverride configuredOverride =
        m_config ? m_config->GetOverride(chosen.name) : CaptureFormatOverride{};
    const CaptureFormatPreference formatPreference =
        FormatPreferenceForOverride(configuredOverride);

    // Source-ID auto-tonemap heuristic: when direct HDR signal detection
    // isn't available AND the connected source is identified as a known
    // HDR-capable console AND the user has hdr_auto_from_source enabled
    // (default true), default the pipeline to HDR. Overrides hdrEnabled=
    // false for the session. User can Alt+H to switch back to SDR.
    // The 4K Pro path (m_hdrDetectionAvailable=true) is NOT affected by
    // this heuristic; it always uses its direct HDR property readout.
    const bool sourceImpliesHDR =
        isElgato && !m_hdrDetectionAvailable &&
        m_config && m_config->hdrAutoFromSource &&
        Is4KSHdmiSourceHdrCapable(m_detectedHdmiSource);

    // useP010 selection rules (4K S vs 4K Pro split):
    //
    // 4K S (m_is4KS == true): the card cannot deliver 4K HDR; P010 is
    //   only published at 1080p/720p. HDR pipeline therefore costs the
    //   user resolution (4K override gets clamped to 1080p below). So
    //   the user must explicitly opt in via hdrEnabled (Alt+H or config).
    //   Source-being-HDR alone is not enough: when source is HDR but
    //   the user has not asked for HDR rendering, stay on NV12 at the
    //   user's preferred resolution (4K is fine) and let the card's
    //   tonemap (Set4KSTonemap ON below) handle the HDR-to-SDR
    //   conversion.
    //
    // 4K Pro (m_hdrDetectionAvailable && !m_is4KS): IKsPropertySet path
    //   exposed direct HDR detection AND supports 4K P010 over PCIe.
    //   P010 whenever the source is HDR (regardless of userWantsHDR);
    //   the shader handles the SDR-from-HDR tonemap when userWantsHDR
    //   is false. No resolution clamp needed.
    //
    // GC553Pro source EOTF chooses the requested mode when auto detection is
    // enabled; MF enumeration remains the authority on native P010 support.
    //
    // No direct detection (other non-Elgato, or 4K S with a failed probe):
    //   fall back to the source-ID heuristic combined with the user's
    //   hdr_enabled config.
    bool useP010;
    if (m_isGC553Pro) {
        const auto startupPolicy = DecideGc553ProSourceOutputPolicy(
            NegotiatedCaptureFormatKind::Other,
            userWantsHDR,
            formatPreference,
            m_config->hdrAutoFromSource,
            m_gc553ProSourceState);
        useP010 = startupPolicy.desiredCaptureIsP010;
        if (startupPolicy.hdrRejected) {
            m_config->hdrEnabled = false;
            const std::wstring warning = ManualFormatHDRWarning(
                configuredOverride.format);
            AppLog(L"Initialize: " + warning);
            ShowToast(warning, std::chrono::milliseconds(8000));
        }
    } else if (m_is4KS) {
        useP010 = isElgato && m_sourceIsHDR10 && userWantsHDR;
    } else {
        useP010 = isElgato &&
                  (m_sourceIsHDR10 ||
                   sourceImpliesHDR ||
                   (userWantsHDR && !m_hdrDetectionAvailable));
    }
    if (useP010) {
        if (m_isGC553Pro) {
            AppLog(L"Initialize: GC553Pro P010 requested; enumerating native P010 modes");
        } else if (m_is4KS) {
            AppLog(L"Initialize: P010 HDR10 capture pipeline selected (4K S + source HDR + user wants HDR; resolution will be clamped to 1080p)");
        } else if (sourceImpliesHDR && !userWantsHDR) {
            AppLog(L"Initialize: P010 HDR10 capture pipeline selected (auto-enabled from detected source: " +
                   m_detectedHdmiSource +
                   L" via hdr_auto_from_source heuristic; press Alt+H for SDR)");
        } else {
            AppLog(L"Initialize: P010 HDR10 capture pipeline selected");
        }
        m_captureDevice->RequestP010(true);
    } else {
        if (m_isGC553Pro) {
            AppLog(L"Initialize: GC553Pro SDR capture pipeline selected (NV12 preferred)");
        } else if (userWantsHDR && !isElgato) {
            AppLog(L"Initialize: hdr_enabled is true but the selected device is not a validated HDR capture device; forcing SDR capture");
        } else if (m_is4KS && m_sourceIsHDR10 && !userWantsHDR) {
            AppLog(L"Initialize: 4K S source is HDR but user prefers SDR (hdrEnabled=false); using card-side tonemap to deliver clean SDR at full resolution");
        }
        AppLog(L"Initialize: SDR BGRA capture pipeline selected");
    }

    // Elgato 4K S vendor HID command: pair the card's internal tonemap
    // state with the upcoming Media Foundation capture format.
    //   useP010 (HDR pipeline)   -> tonemap OFF: card passes raw HDR10 PQ
    //                                            through; the shader does
    //                                            the HDR rendering.
    //   !useP010 (SDR pipeline)  -> tonemap ON:  card converts an HDR
    //                                            HDMI source to SDR
    //                                            internally so the NV12
    //                                            frames are clean SDR.
    // Sending OFF unconditionally before NV12/BGRA capture leaves the
    // card in HDR-passthrough mode while NitLink is set up to render
    // SDR, which makes the SDR picture look washed (HDR codes
    // interpreted as 100-nit-paper-white SDR). Always pair the two.
    // Sent only when the selected device is a 4K S, since the HID protocol is
    // 4K S-specific (VID 0x0FD9 + PID 0x00AE/0x00AF).
    // Detect4KSHdmiSource already fired earlier in Initialize (before
    // the useP010 decision so the source-ID heuristic can feed into
    // it); no second call here.
    if (m_is4KS && Set4KSTonemap(/*enableTonemap=*/ !useP010)) {
        AppLog(useP010
            ? L"Initialize: 4K S HID tonemap OFF sent for raw HDR/P010"
            : L"Initialize: 4K S HID tonemap ON sent for SDR/NV12");
    }

    // F1 Source picker manual override. Applied to the capture device
    // before Open so the negotiator inside CaptureDevice tries the
    // user's exact resolution / fps / format combination first.
    // Unachievable combinations fall back to native-best + standard
    // attempts and set CaptureDevice::m_fallbackNotice, which the JSON
    // state push surfaces as a toast on the JS side. The config field
    // stays set across the fallback so the override re-applies on
    // subsequent source changes.
    {
        CaptureDevice::OverrideSpec ov;
        if (m_config) {
            ov.width  = configuredOverride.width;
            ov.height = configuredOverride.height;
            ov.fps    = configuredOverride.fps;
            ov.fpsNumerator = configuredOverride.fpsNumerator;
            ov.fpsDenominator = configuredOverride.fpsDenominator;
            ov.format = configuredOverride.format;
        }

        // 4K S HDR resolution clamp: when the HDR pipeline is selected
        // on the 4K S, override any >1080p resolution preference down
        // to 1920x1080 so P010 negotiation actually succeeds. The 4K S
        // only publishes P010 at 1080p/720p over USB; without this
        // clamp the user's 4K override would force P010 to fall back
        // to NV12 inside CaptureDevice::Open, defeating the HDR
        // pipeline. The user's saved config is NOT modified: when they
        // Alt+H back to SDR, useP010 goes false, this branch is skipped,
        // and the next reconcile reopens at their preferred 4K NV12
        // (with card-side tonemap engaged for HDR sources).
        if (m_is4KS && useP010 && ov.height > 1080) {
            AppLog(L"Initialize: 4K S HDR pipeline, clamping capture override "
                   L"from " + std::to_wstring(ov.width) + L"x" +
                   std::to_wstring(ov.height) + L" down to 1920x1080 "
                   L"(4K S publishes P010 only at 1080p/720p)");
            ov.width  = 1920;
            ov.height = 1080;
            // fps stays as requested (60fps works at 1080p P010). Format
            // stays as requested too ("" = Auto picks P010 first via the
            // attemptsP010 list when m_requestP010 is true).
        }

        m_captureDevice->SetFormatOverride(ov);
    }

    if (!m_captureDevice->Open(chosen)) {
        // If P010 was requested and failed, retry once without it. The
        // device might publish P010 in its capability list but reject the
        // negotiation at runtime (driver quirk, video processor limit, etc).
        if (useP010) {
            AppLog(L"Initialize: P010 negotiation failed, retrying with BGRA");
            m_captureDevice->RequestP010(false);
            // Tonemap was sent OFF above for the P010 attempt; that
            // attempt failed and the pipeline is falling back to SDR
            // NV12. Flip the 4K S HID tonemap state to ON so the card
            // converts any HDR source to SDR for the upcoming capture.
            // Same selected-4K-S gating as the initial call above.
            if (m_is4KS && Set4KSTonemap(/*enableTonemap=*/ true)) {
                AppLog(L"Initialize: 4K S HID tonemap ON sent for SDR retry");
            }
            if (!m_captureDevice->Open(chosen)) {
                AppLog(L"Initialize: CaptureDevice::Open FAILED");
                return false;
            }
        } else {
            AppLog(L"Initialize: CaptureDevice::Open FAILED");
            return false;
        }
    }
    AppLog(L"Initialize: capture device opened");
    m_nonGcP010Fallback.CompleteOpen(
        !m_isGC553Pro && useP010,
        IsEqualGUID(m_captureDevice->GetOutputFormat().subtype, MFVideoFormat_P010));
    ApplyPresentCap();
    ApplyAspectRatio();

    // 4K Pro source-mode readout. Reads the connected HDMI source's
    // resolution + fps from the Elgato custom property set (props 210
    // and 208), which populate only after the capture filter has been
    // opened. Stores the result in m_source4KProMode for later use by
    // the window-title composer. With no live HDMI signal, the registers can
    // still be empty after Open; Run reads them again on the first real frame
    // of the next signal lock. A successful startup read consumes that initial
    // edge so a source already live at launch needs no duplicate query.
    // Silent no-op on devices without the Elgato IKsPropertySet GUID
    // (4K S and non-Elgato sources).
    if (isElgato && m_hdrDetectionAvailable && !m_is4KS) {
        m_source4KProMode = Detect4KProSourceMode(chosen.name);
        m_prevSourceNoSignal = !m_source4KProMode.detected;
    }

    // 4K X source mode uses the UVC XU command mailbox. The destructive probe
    // that disturbed the card is gone; Detect4KXSourceMode issues exact-sized
    // request/response transfers whose writes go only to the XU command port,
    // never the HID/processing path. The XU read needs a LIVE signal, so it
    // does not run at init: the device is flagged by name and Run() fires the
    // read on each no-signal to signal (re)lock edge.
    m_is4KX = isElgato && (chosen.name.find(L"4K X") != std::wstring::npos);

    // Log every native format this device exposes. Pure diagnostic: does
    // NOT change the running pipeline. Useful when triaging: confirms
    // whether the Elgato publishes P010 (10-bit BT.2020 PQ) for the HDR10
    // path. Output goes to the [NitLink/Formats] debug channel.
    m_captureDevice->LogAvailableFormats();

    auto format = m_captureDevice->GetOutputFormat();

    // Freeze device-family and negotiated-format identity before the capture
    // worker starts so the first startup sample uses authoritative metadata.
    m_placeholderDeviceFamily = PlaceholderFamilyForDevice(m_currentDeviceInfo.name);
    m_placeholderCaptureMetadataReady =
        TryPlaceholderFormatForSubtype(format.subtype, m_placeholderCaptureFormat);
    if (!m_placeholderCaptureMetadataReady) {
        m_placeholderDeviceFamily =
            PlaceholderDetector::PlaceholderDeviceFamily::Unknown;
    }

    if (const P010SelectionNotice notice =
            m_captureDevice->ConsumeP010SelectionNotice();
        notice.available) {
        const std::wstring warning = P010SelectionWarning(notice);
        AppLog(L"Initialize: " + warning);
        ShowToast(warning, std::chrono::milliseconds(8000));
    }

    if (m_isGC553Pro && useP010 &&
        !IsEqualGUID(format.subtype, MFVideoFormat_P010)) {
        if (m_config->hdrAutoFromSource &&
            m_gc553ProSourceState == Gc553ProSourceHdrState::Hdr10Pq &&
            formatPreference == CaptureFormatPreference::Auto)
            m_gc553ProAutoP010Rejected = true;
        const std::wstring warning = P010UnavailableWarning(format);
        AppLog(L"Initialize: " + warning);
        m_config->hdrEnabled = false;
        ShowToast(warning, std::chrono::milliseconds(8000));
    }

    // Post-Open tonemap reconciliation. CaptureDevice::Open() can fall
    // back from P010 to NV12 within a single call when the requested
    // resolution doesn't publish P010 (most commonly the 4K S manual
    // override at 3840x2160, where P010 is only available at
    // 1080p/720p). Open() returns success in that case, so the outer
    // retry-with-BGRA block above doesn't fire, and the Set4KSTonemap
    // OFF call earlier (sent because useP010 was true) is now
    // misaligned with the actual NV12 capture: the card keeps passing
    // raw BT.2020 PQ codes packed into the 8-bit NV12 container, which
    // produces a washed picture in SDR rendering AND a near-black
    // picture in HDR rendering (the SDR-shaped 8-bit codes interpreted
    // as PQ light up at near-zero nits).
    //
    // Detect the fallback by comparing requested vs actual subtype.
    // When P010 was requested but NV12 (or any non-P010) was
    // negotiated, flip the 4K S tonemap to ON so the card converts
    // the HDR source to clean SDR before delivering NV12. The shader
    // path then renders correctly regardless of Alt+H state.
    if (m_is4KS && useP010) {
        const bool actualIsP010 = IsEqualGUID(format.subtype, MFVideoFormat_P010);
        if (!actualIsP010) {
            if (Set4KSTonemap(/*enableTonemap=*/ true)) {
                AppLog(L"Initialize: P010 requested but Open negotiated a non-HDR "
                       L"format (likely 4K manual override; 4K S only publishes "
                       L"P010 at 1080p/720p); flipped 4K S tonemap ON so NV12 "
                       L"frames carry clean SDR instead of raw HDR codes");
            }
        }
    }

    // Now safe to update the title: source identifier and HDR detection
    // were settled by the 4K S vendor HID block above, AND the actual
    // capture format is known (post-Open). UpdateWindowTitle reads
    // GetOutputFormat().subtype to distinguish [HDR] (P010 negotiated)
    // from [SDR] (NV12 negotiated, e.g. 4K override fallback).
    UpdateWindowTitle();

    m_frameBuffer = std::make_unique<FrameBuffer>(format.width, format.height, format.stride);
    AppLog(L"Initialize: frame buffer created");

    // Tell the renderer what row order the capture source is delivering.
    // The renderer's BGRA shader will conditionally flip V to handle either
    // orientation correctly. P010 / NV12 paths are unaffected (they have
    // their own dedicated shaders that already assume top-down planar data).
    if (m_renderer) m_renderer->SetSourceRowOrder(format.topDown);

    // Tell the renderer the pixel value range of the captured frames.
    // Most HDMI sources come in as limited range (16-235); the BGRA and
    // NV12 shaders apply a 16->0 / 235->255 expansion to recover full
    // dynamic range. Some drivers (notably the Elgato 4K S NV12 path)
    // deliver full-range pixels (0-255) directly; for those the shader
    // is told to skip the expansion. Without this, full-range sources
    // render with crushed blacks.
    if (m_renderer) {
        m_lastMfFullRange   = format.fullRange;
        m_lastCaptureIsP010 = IsEqualGUID(format.subtype, MFVideoFormat_P010);
        UpdateCaptureColorInterpretation(m_currentDeviceInfo.name);
    }

    // Keep source EOTF and negotiated capture format separate. GC553Pro Auto
    // uses its XU/EOTF source state for the renderer's HDR10 input flag;
    // other paths retain their existing subtype-driven behavior:
    //
    //   m_sourceIsHDR10  = "did source detection report HDR10 from the
    //                       upstream HDMI signal?" (always false on 4K S)
    //   captureIsP010    = "is the capture buffer a 10-bit P010 container?"
    //
    // On the 4K Pro they match because the poller drives format selection.
    // On the 4K S they diverge:
    // m_sourceIsHDR10 is always false but the user can still ask for P010
    // capture via Alt+H, and when they do the shader must pick the BT.2020
    // PQ path or BT.2020 reds get misinterpreted as BT.709 reds (orange to
    // red shift).
    if (m_renderer) {
        const bool captureIsP010 = IsEqualGUID(format.subtype, MFVideoFormat_P010);
        m_renderer->SetSourceIsHDR10(RendererInputIsHdr10(
            m_isGC553Pro, m_config && m_config->hdrAutoFromSource,
            m_sourceIsHDR10, captureIsP010));

        // Thread the negotiated subtype GUID through to the renderer as a
        // stable enum, replacing the renderer's old per-frame byte-count
        // inference. Without this, partial frames from HDMI signal-loss
        // windows could land in the wrong format's tolerance band and get
        // uploaded as the wrong format (the green-frame failure mode).
        DX11Renderer::CaptureFormatKind rkind;
        if (IsEqualGUID(format.subtype, MFVideoFormat_P010)) {
            rkind = DX11Renderer::CaptureFormatKind::P010;
        } else if (IsEqualGUID(format.subtype, MFVideoFormat_NV12)) {
            rkind = DX11Renderer::CaptureFormatKind::NV12;
        } else {
            // MFVideoFormat_RGB32 / MFVideoFormat_ARGB32 both land here:
            // the renderer's BGRA path handles both (they share byte layout).
            rkind = DX11Renderer::CaptureFormatKind::BGRA;
        }
        m_renderer->SetSourceFormat(rkind);
    }

    // The negotiated placeholder identity and renderer source format are now
    // complete. Start capture before optional audio/UI/upscaler initialization
    // so those subsystems cannot delay the first usable capture callback.
    m_placeholderDetector = std::make_unique<PlaceholderDetector>();
    m_sessionStartTime = std::chrono::system_clock::now();
    m_lastGoodFrameTime = std::chrono::steady_clock::now();
    m_lastMotionTime = m_lastGoodFrameTime;
    m_lastContentTime = m_lastGoodFrameTime;
    m_captureFrameValidForSession = false;
    m_gc553ProPlaceholderTransitionGuardConsumed = false;
    m_gc553ProPreviousFreshFrameWasPlaceholder = false;
    m_gc553ProStartupBlackHint.Reset();
    if (m_renderer) m_renderer->InvalidateCaptureFrame();

    const bool captureStarted = m_captureDevice->StartCapture(
        [this](const uint8_t* data, uint32_t size, int64_t timestamp,
               int64_t arrivalWallNs, uint64_t deviceTimestamp) {
            if (DropPlaceholderFrame(data, size)) return;
            if (m_frameBuffer) {
                m_frameBuffer->Write(data, size, timestamp,
                                     arrivalWallNs, deviceTimestamp);
            }
        });
    if (!captureStarted) {
        AppLog(L"Initialize: capture start failed");
    } else {
        AppLog(L"Initialize: capture started before optional subsystems");
    }

    m_audioRouter = std::make_unique<AudioRouter>();
    // Route audio from the selected capture card to the default playback device.
    //
    // Pass the selected video device's full name as the audio endpoint hint
    // rather than the generic "Elgato" substring. Reasons:
    //   1) On systems with Elgato Wave Link / Stream Deck audio routing, the
    //      "Elgato Virtual Audio" string appears in multiple endpoints (Wave
    //      Link mixer channels: Headphones Mix, Voice Chat, Stream Mix, etc.).
    //      A generic "Elgato" hint can match one of those software mixer
    //      channels instead of the real hardware capture endpoint.
    //   2) On multi-device setups (e.g. Facecam + 4K X), the generic hint
    //      could match the wrong card's audio.
    // Using the specific device name as the hint narrows the substring search
    // to endpoints containing e.g. "Elgato 4K Pro" or "Elgato 4K X", which
    // matches only the paired hardware audio endpoint per Elgato's naming
    // convention. Wave Link channels don't carry the specific device name.
    const std::wstring audioHint = DeriveAudioHint(m_currentDeviceInfo.name);
    if (!m_audioRouter->Initialize(audioHint)) {
        AppLog(L"Initialize: AudioRouter has no endpoints yet (worker keeps retrying)");
        if (m_audioRouter->LastCaptureError() == E_ACCESSDENIED) {
            // The Windows Microphone privacy switch blocks every capture
            // endpoint, the card's audio included, and reports it as a plain
            // access denial. Name the actual switch so the fix is one toggle.
            ShowToast(Tr(L"toast.noAudio"),
                      std::chrono::milliseconds(8000));
        }
    }

    // NIS upscaler: compile compute shader, upload coefficient tables.
    // Init is best-effort: if it fails (e.g. user doesn't have compute shader
    // support), the rest of the app works fine, just no image upscaling.
    m_nisUpscaler = std::make_unique<NisUpscaler>();
    if (!m_nisUpscaler->Initialize(m_renderer->GetDevice())) {
        AppLog(L"Initialize: NisUpscaler::Initialize FAILED (continuing without NIS)");
        m_nisUpscaler.reset();
    } else {
        AppLog(L"Initialize: NIS upscaler ready");
    }

    // Frame differ: detects "is this captured frame new content vs a duplicate?"
    // Needed for real game-FPS reporting. Best-effort init.
    m_frameDiffer = std::make_unique<FrameDiffer>();
    if (!m_frameDiffer->Initialize(m_renderer->GetDevice())) {
        AppLog(L"Initialize: FrameDiffer::Initialize FAILED (continuing without)");
        m_frameDiffer.reset();
    } else {
        AppLog(L"Initialize: frame differ ready");
    }


    m_webviewSettings = std::make_unique<WebViewSettings>();
    m_webviewSettings->Initialize(m_window->GetHWND(), actualW, actualH);
    ApplyPanelLayout();
    m_webviewSettings->SetMessageHandler([this](const std::wstring& msg) {
        // Length gate first. Every legitimate message in the schema below is
        // small (a few hundred chars at most); a multi-megabyte string only
        // means a malformed or hostile sender, and the repeated find() scans
        // further down would turn it into needless O(n) work. Reject it before
        // any parsing. This is the bridge's outer trust boundary; treat the
        // WebView2 sender as untrusted in case navigation is ever not locked
        // down (see WebViewSettings nav allow-list).
        if (msg.size() > 8192) {
            AppLog(L"WebView2 msg: oversized payload, ignoring");
            return;
        }
        // Messages from JS arrive as JSON strings of the form:
        //   {"action":"toggleHDR","value":true}
        //   {"action":"setVolume","value":0.75}
        //   {"action":"ready"}
        // No full JSON library is pulled in: instead the "action" field
        // and an optional "value" field are extracted via tiny string
        // search. Robust enough for the small fixed schema; if more
        // complex messages land later, nlohmann_json can be vendored.
        auto extractStr = [&](const wchar_t* key) -> std::wstring {
            std::wstring needle = std::wstring(L"\"") + key + L"\":\"";
            auto p = msg.find(needle);
            if (p == std::wstring::npos) return L"";
            p += needle.size();
            auto end = msg.find(L'"', p);
            return (end == std::wstring::npos) ? L"" : msg.substr(p, end - p);
        };
        auto extractRaw = [&](const wchar_t* key) -> std::wstring {
            // Pull a value that's a number/bool (no quotes). Returns the
            // raw token; caller parses.
            std::wstring needle = std::wstring(L"\"") + key + L"\":";
            auto p = msg.find(needle);
            if (p == std::wstring::npos) return L"";
            p += needle.size();
            auto end = msg.find_first_of(L",}", p);
            return (end == std::wstring::npos) ? L"" : msg.substr(p, end - p);
        };

        const std::wstring action = extractStr(L"action");
        if (action.empty()) return;

        AppLog(L"WebView2 msg: " + action);

        if (action == L"ready") {
            // JS finished loading and asked for an initial state dump.
            PushSettingsState();
            // First-launch experience: if there was no config file on
            // startup (fresh install), open the settings menu now so the
            // user sees the controls right away. Reset the flag so this
            // only fires once per session.
            if (m_firstLaunch) {
                m_firstLaunch = false;
                if (!m_settingsVisible) ToggleSettings();
                AppLog(L"first launch: opened settings menu automatically");
            }
            return;
        }
        if (action == L"setLanguage" && m_config) {
            const std::wstring requested = extractStr(L"value");
            if (requested == L"system" || requested == L"en-US" || requested == L"zh-TW") {
                m_config->language = requested == L"en-US" ? "en-US"
                    : requested == L"zh-TW" ? "zh-TW" : "system";
                Localization::Instance().SetPreference(m_config->language);
                if (m_overlay && !m_overlay->RefreshTextFormats()) {
                    AppLog(L"setLanguage: failed to refresh overlay text formats");
                }
                m_config->Save("nitlink.json");
                UpdateWindowTitle();
                PushSettingsState();
            }
            return;
        }
        if (action == L"setNoSignalMode" && m_config) {
            const std::wstring requested = extractStr(L"value");
            if (requested == L"default" || requested == L"image") {
                m_config->noSignalMode =
                    requested == L"image" ? "image" : "default";
                ApplyNoSignalSettings();
                m_config->Save("nitlink.json");
                PushSettingsState();
            }
            return;
        }
        if (action == L"cycleNoSignalMode") {
            CycleNoSignalMode();
            return;
        }
        if (action == L"chooseNoSignalImage") {
            ChooseNoSignalImage();
            return;
        }
        if (action == L"setNoSignalFit" && m_config) {
            const std::wstring requested = extractStr(L"value");
            if (requested == L"contain" || requested == L"cover" ||
                requested == L"stretch") {
                m_config->noSignalFit = WideToUtf8(requested);
                ApplyNoSignalSettings();
                m_config->Save("nitlink.json");
                PushSettingsState();
            }
            return;
        }
        if (action == L"cycleNoSignalFit") {
            CycleNoSignalFit();
            return;
        }
        if (action == L"cycleNoSignalDimImage") {
            CycleNoSignalDimImage();
            return;
        }
        if (action == L"toggleHDR" && m_config) {
            m_config->hdrEnabled = !m_config->hdrEnabled;
            // Render loop's HDR-sync block picks this up next iteration.
            m_config->Save("nitlink.json");
            return;
        }
        if (action == L"toggleColorExpansion" && m_config && m_renderer) {
            m_config->colorExpansion = !m_config->colorExpansion;
            m_renderer->SetColorExpansion(m_config->colorExpansion);
            m_config->Save("nitlink.json");
            return;
        }
        if (action == L"toggleNIS" && m_config) {
            m_config->nisEnabled = !m_config->nisEnabled;
            // Render loop reads m_config->nisEnabled each frame.
            m_config->Save("nitlink.json");
            return;
        }
        if (action == L"toggleMute" && m_config && m_audioRouter) {
            m_config->audioMuted = !m_config->audioMuted;
            m_audioRouter->SetMuted(m_config->audioMuted);
            m_config->Save("nitlink.json");
            return;
        }
        if (action == L"cyclePresentPacing" && m_config) {
            CyclePresentPacing();
            return;
        }
        if (action == L"toggleLowLatency" && m_config) {
            // Low-latency present mode on/off. The run loop reads m_lowLatency
            // each iteration to choose present-on-arrival vs vsync pacing, so
            // the next frame picks up the change with no pipeline rebuild.
            // m_config->lowLatency is the persisted mirror.
            m_lowLatency = !m_lowLatency;
            m_config->lowLatency = m_lowLatency;
            m_config->Save("nitlink.json");
            AppLog(m_lowLatency ? L"Low-Latency ON (F1): present-on-arrival"
                                : L"Low-Latency OFF (F1): VRR/Smooth pacing");
            return;
        }
        if (action == L"togglePreventSleep" && m_config && m_window) {
            m_config->preventSleep = !m_config->preventSleep;
            m_window->SetPreventSleep(m_config->preventSleep);
            m_config->Save("nitlink.json");
            return;
        }
        if (action == L"setVolume" && m_config && m_audioRouter) {
            const std::wstring raw = extractRaw(L"value");
            if (!raw.empty()) {
                float v = std::wcstof(raw.c_str(), nullptr);
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                m_config->audioVolume = v;
                m_audioRouter->SetVolume(v);
                // Don't Save() on every slider tick: that would hammer
                // disk during a drag. Save happens on exit and on
                // discrete toggles instead.
            }
            return;
        }
        if (action == L"setPiPOpacity") {
            const std::wstring raw = extractRaw(L"value");
            wchar_t* end = nullptr;
            const float opacity = std::wcstof(raw.c_str(), &end);
            if (end != raw.c_str() && *end == L'\0') SetPiPOpacity(opacity);
            return;
        }
        if (action == L"cycleAspect") {
            CycleAspectRatio();
            return;
        }
        if (action == L"cyclePanelSide") {
            CyclePanelSide();
            return;
        }
        if (action == L"cycleScaler") {
            // Only Catmull-Rom is wired today; this is a placeholder for
            // when more scalers exist (bilinear, Lanczos, etc).
            AppLog(L"cycleScaler: only one option available today");
            return;
        }
        if (action == L"setGame" && m_config) {
            // User picked a game from the dropdown. Extract the id, load the
            // saved settings for it (or seed defaults), apply them, push the
            // game to Discord, and persist.
            const std::wstring wid = extractStr(L"value");
            std::string id;
            id.reserve(wid.size());
            for (wchar_t wc : wid) id.push_back(static_cast<char>(wc));

            m_config->currentGameId = id;
            if (!id.empty()) {
                ApplyGameSettings(id);
            }
            UpdateDiscordForCurrentGame();
            m_config->Save("nitlink.json");
            PushSettingsState();
            return;
        }
        if (action == L"clearGame" && m_config) {
            // "Clear game": go back to global defaults, drop the per-game
            // entry's link from Discord. The stored GameSettings for that
            // id are NOT deleted; if the user picks it again later,
            // their saved prefs come back.
            m_config->currentGameId = "";
            UpdateDiscordForCurrentGame();
            m_config->Save("nitlink.json");
            PushSettingsState();
            return;
        }
        if (action == L"saveGameSettings" && m_config) {
            // "Save for this game": capture the CURRENT live settings into
            // gameSettings[currentGameId]. Next time the user selects this
            // game, ApplyGameSettings loads these values back.
            if (!m_config->currentGameId.empty()) {
                auto& gs = m_config->gameSettings[m_config->currentGameId];
                gs.nisEnabled     = m_config->nisEnabled;
                gs.colorExpansion = m_config->colorExpansion;
                m_config->Save("nitlink.json");
                AppLog(L"Saved settings for game");
            }
            return;
        }

        // ===== Capture-device picker actions =====
        //
        // Backend entry points for switching capture devices at runtime.
        // The F1 settings panel's Source row (rendered by nitlink-menu.html
        // from the captureDevices and activeDevice fields pushed via
        // PushSettingsState) is the primary consumer. The bridge stays
        // UI-agnostic so a hotkey, a tray menu, or external tooling that
        // drives WebView2 directly can dispatch the same messages.
        if (action == L"getDeviceList") {
            // Opening the Source list is an explicit request for fresh
            // devices. Ordinary settings pushes stay on the cached list.
            PushSettingsState(/*refreshCaptureDevices=*/true);
            return;
        }
        if (action == L"refreshDevices") {
            // Same effect as getDeviceList but logs the action as a
            // user-initiated refresh. Lets DebugView distinguish a UI
            // refresh-button click from an automatic state push.
            AppLog(L"WebView2: user requested device refresh");
            PushSettingsState(/*refreshCaptureDevices=*/true);
            return;
        }
        if (action == L"setPreferredDevice") {
            // User picked a device from the picker. Route to the
            // strict SwitchCaptureDevice resolver: if the requested
            // name does not match a currently-connected device, the
            // switch fails and the current device stays active. Push
            // state either way so the JS dropdown re-syncs to reality:
            // on success the new device is highlighted; on failure
            // the dropdown reverts to whatever is actually open.
            const std::wstring requested = extractStr(L"value");
            if (requested.empty()) {
                AppLog(L"setPreferredDevice: empty value, ignoring");
                PushSettingsState();
                return;
            }
            const bool ok = SwitchCaptureDevice(requested);
            if (!ok) {
                AppLog(L"setPreferredDevice: switch failed for '" + requested
                       + L"'; UI will revert to the current device");
            }
            PushSettingsState();
            return;
        }
        if (action == L"setCaptureFormatOverride" && m_config) {
            // User picked (or cleared) a manual capture format from the
            // F1 Source picker. The wire payload is a nested object:
            //   {"action":"setCaptureFormatOverride",
            //    "value":{"width":1920,"height":1080,"fps":60,"format":"NV12"}}
            //
            // The extractStr / extractRaw helpers scan by key name and
            // would happily pull "width" out of any place in the
            // message; safe here because no other message
            // schema uses those four keys, so they only appear inside
            // the nested object. Each field 0 / empty = Auto for that
            // axis: the dropdown's "Auto" option emits the empty value.
            const std::wstring rawW   = extractRaw(L"width");
            const std::wstring rawH   = extractRaw(L"height");
            const std::wstring rawFps = extractRaw(L"fps");
            const std::wstring rawFpsNumerator = extractRaw(L"fpsNumerator");
            const std::wstring rawFpsDenominator = extractRaw(L"fpsDenominator");
            const std::wstring fmtStr = extractStr(L"format");

            // Clamp each axis to a sane ceiling. These are untrusted bridge
            // values; the capture negotiator already rejects nonsensical modes
            // and falls back to Auto, but clamping here keeps a bogus number
            // (or a negative, which std::stoul silently wraps) out of the
            // persisted config in the first place. 0 / empty = Auto per axis.
            auto parseUint = [](const std::wstring& s, uint32_t hi) -> uint32_t {
                if (s.empty() || s[0] == L'-') return 0;
                try {
                    unsigned long n = std::stoul(s);
                    return static_cast<uint32_t>(n > hi ? hi : n);
                } catch (...) { return 0; }
            };

            auto& cv = m_config->captureFormatOverrides[m_currentDeviceInfo.name];
            cv.width  = parseUint(rawW, 16384);
            cv.height = parseUint(rawH, 16384);
            cv.fps    = parseUint(rawFps, 1000);
            cv.fpsNumerator = parseUint(rawFpsNumerator, 1000000000);
            cv.fpsDenominator = parseUint(rawFpsDenominator, 1000000000);
            if (cv.fpsNumerator > 0) {
                if (cv.fpsDenominator == 0) cv.fpsDenominator = 1;
                cv.fps = cv.fpsNumerator / cv.fpsDenominator;
            } else {
                // Integer-only payloads are legacy/compatibility input. Keep
                // the rational unresolved so native-mode negotiation can map
                // 59 back to 60000/1001 instead of inventing 59/1.
                cv.fpsDenominator = 1;
            }
            cv.format = fmtStr;
            m_config->Save("nitlink.json");

            AppLog(L"setCaptureFormatOverride [" + m_currentDeviceInfo.name + L"]: "
                   + std::to_wstring(cv.width) + L"x"
                   + std::to_wstring(cv.height) + L" @ "
                   + std::to_wstring(cv.fps) + L"fps "
                   + (cv.format.empty() ? L"(format=Auto)" : cv.format));

            // Force a reconcile so the negotiator picks up the new
            // override on the next Open. ReconcileCaptureFormat handles
            // the full teardown / re-Open / downstream rebuild cycle
            // (renderer textures, frame buffer, differ, placeholder
            // detector). force=true bypasses the "format already
            // matches" early-out so the override always takes effect.
            // If the requested combination is unavailable for the live
            // source, the negotiator inside CaptureDevice will fall
            // back to Auto and set the fallback notice; the next
            // PushSettingsState below picks it up and surfaces the
            // toast on the JS side.
            ReconcileCaptureFormat(/*force=*/true);
            PushSettingsState();
            return;
        }
        if (action == L"openScreenshotFolder") {
            // User clicked the screenshot toast's path link. Open Windows
            // Explorer with the file pre-selected (/select switch).
            const std::wstring path = extractStr(L"value");
            // path is untrusted bridge input that gets interpolated into a
            // quoted explorer.exe argument. An embedded double-quote could
            // break out of the quoting and inject extra switches (e.g.
            // /root,<dir>), so reject those outright. Also require the path to
            // actually exist on disk, since this action only ever opens a file
            // NitLink itself just wrote, so a non-existent path is bogus.
            std::error_code existsEc;
            if (!path.empty() &&
                path.find(L'"') == std::wstring::npos &&
                std::filesystem::exists(path, existsEc)) {
                const std::wstring args = L"/select,\"" + path + L"\"";
                ShellExecuteW(nullptr, nullptr, L"explorer.exe",
                              args.c_str(), nullptr, SW_SHOWNORMAL);
            }
            return;
        }
    });
    m_webviewSettings->NavigateToFile(L"nitlink-menu.html");
    m_webviewSettings->Show(false);
    AppLog(L"Initialize: WebView2 settings ready");

    // Discord Rich Presence. Connection is optional: if Discord isn't
    // running on the user's machine, Connect() returns false and RPC updates
    // are simply skipped. Application keeps working normally. The app ID is
    // hardcoded to the NitLink application registered at
    // discord.com/developers/applications.
    m_discord = std::make_unique<DiscordRPC>();
    if (m_discord->Connect("1505211372286246944")) {
        AppLog(L"Initialize: Discord RPC connected");
        // Set initial "idle" presence. This will be overwritten the moment
        // the user selects a game from the settings menu.
        m_discord->SetActivity(
            L"In NitLink",
            L"PS5 Capture Viewer",
            std::chrono::system_clock::now(),
            "nitlink-logo",
            L"NitLink"
        );
    } else {
        AppLog(L"Initialize: Discord RPC unavailable (Discord not running or RPC disabled)");
    }

    // If config restored a previously-selected game, apply its per-game
    // settings to the live pipeline and reflect on Discord. Do this AFTER
    // Discord init so UpdateDiscordForCurrentGame has somewhere to push.
    if (m_config && !m_config->currentGameId.empty()) {
        ApplyGameSettings(m_config->currentGameId);
        UpdateDiscordForCurrentGame();
        std::wstring wid;
        for (char c : m_config->currentGameId) wid.push_back(static_cast<wchar_t>(c));
        AppLog(L"Initialize: restored game " + wid);
    }

    m_hotkeyManager = std::make_unique<HotkeyManager>(m_window->GetHWND());
    m_hotkeyManager->Register("toggle_fullscreen", {VK_MENU, VK_RETURN},
        [this]() { ToggleFullscreen(); });
    m_hotkeyManager->Register("toggle_fullscreen_f11", {VK_F11},
        [this]() { ToggleFullscreen(); });
    m_hotkeyManager->Register("toggle_pip", {VK_MENU, 'O'},
        [this]() { TogglePiP(); });

    // PiP nudge hotkeys.
    //
    // Ctrl + arrow         : small step (20 px)
    // Ctrl + Shift + arrow : large step (80 px)
    //
    // Repeating PiP controls allow held keys to cover larger distances.
    // Config retains each applied position for the shutdown save.
    auto nudgePiP = [this](int dx, int dy) {
        if (!m_window) return;
        if (!m_window->NudgePiP(dx, dy)) return;
        int32_t x = 0, y = 0;
        if (m_window->GetPiPPosition(x, y) && m_config) {
            m_config->pipX = x;
            m_config->pipY = y;
        }
    };
    constexpr int kPiPNudgeSmall = 20;
    constexpr int kPiPNudgeLarge = 80;
    m_hotkeyManager->Register("pip_nudge_left",
        {VK_CONTROL, VK_LEFT},
        [nudgePiP]() { nudgePiP(-kPiPNudgeSmall, 0); }, true);
    m_hotkeyManager->Register("pip_nudge_right",
        {VK_CONTROL, VK_RIGHT},
        [nudgePiP]() { nudgePiP( kPiPNudgeSmall, 0); }, true);
    m_hotkeyManager->Register("pip_nudge_up",
        {VK_CONTROL, VK_UP},
        [nudgePiP]() { nudgePiP(0, -kPiPNudgeSmall); }, true);
    m_hotkeyManager->Register("pip_nudge_down",
        {VK_CONTROL, VK_DOWN},
        [nudgePiP]() { nudgePiP(0,  kPiPNudgeSmall); }, true);
    m_hotkeyManager->Register("pip_nudge_left_big",
        {VK_CONTROL, VK_SHIFT, VK_LEFT},
        [nudgePiP]() { nudgePiP(-kPiPNudgeLarge, 0); }, true);
    m_hotkeyManager->Register("pip_nudge_right_big",
        {VK_CONTROL, VK_SHIFT, VK_RIGHT},
        [nudgePiP]() { nudgePiP( kPiPNudgeLarge, 0); }, true);
    m_hotkeyManager->Register("pip_nudge_up_big",
        {VK_CONTROL, VK_SHIFT, VK_UP},
        [nudgePiP]() { nudgePiP(0, -kPiPNudgeLarge); }, true);
    m_hotkeyManager->Register("pip_nudge_down_big",
        {VK_CONTROL, VK_SHIFT, VK_DOWN},
        [nudgePiP]() { nudgePiP(0,  kPiPNudgeLarge); }, true);

    // Alt separates resizing from nudging under exact modifier matching.
    // The callback also persists mouse resizing before PiP can be toggled off.
    m_window->SetPiPResizeCallback([this]() {
        if (!m_window || !m_config) return;
        const auto [width, height] = m_window->GetClientSize();
        if (width < 80 || width > 16384 || height < 45 || height > 16384) return;
        m_config->pipWidth = width;
        m_config->pipHeight = height;
        int32_t x = 0, y = 0;
        if (m_window->GetPiPPosition(x, y)) {
            m_config->pipX = x;
            m_config->pipY = y;
        }
    });
    auto resizePiP = [this](int dw, int dh, bool keepAspect = false) {
        if (m_window) m_window->ResizePiP(dw, dh, keepAspect);
    };
    constexpr int kPiPResizeSmall = 20;
    constexpr int kPiPResizeLarge = 80;
    m_hotkeyManager->Register("pip_resize_narrower",
        {VK_CONTROL, VK_MENU, VK_LEFT},
        [resizePiP]() { resizePiP(-kPiPResizeSmall, 0); }, true);
    m_hotkeyManager->Register("pip_resize_wider",
        {VK_CONTROL, VK_MENU, VK_RIGHT},
        [resizePiP]() { resizePiP( kPiPResizeSmall, 0); }, true);
    m_hotkeyManager->Register("pip_resize_shorter",
        {VK_CONTROL, VK_MENU, VK_UP},
        [resizePiP]() { resizePiP(0, -kPiPResizeSmall); }, true);
    m_hotkeyManager->Register("pip_resize_taller",
        {VK_CONTROL, VK_MENU, VK_DOWN},
        [resizePiP]() { resizePiP(0,  kPiPResizeSmall); }, true);
    m_hotkeyManager->Register("pip_resize_narrower_big",
        {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LEFT},
        [resizePiP]() { resizePiP(-kPiPResizeLarge, 0); }, true);
    m_hotkeyManager->Register("pip_resize_wider_big",
        {VK_CONTROL, VK_MENU, VK_SHIFT, VK_RIGHT},
        [resizePiP]() { resizePiP( kPiPResizeLarge, 0); }, true);
    m_hotkeyManager->Register("pip_resize_shorter_big",
        {VK_CONTROL, VK_MENU, VK_SHIFT, VK_UP},
        [resizePiP]() { resizePiP(0, -kPiPResizeLarge); }, true);
    m_hotkeyManager->Register("pip_resize_taller_big",
        {VK_CONTROL, VK_MENU, VK_SHIFT, VK_DOWN},
        [resizePiP]() { resizePiP(0,  kPiPResizeLarge); }, true);

    m_hotkeyManager->Register("pip_scale_up",
        {VK_MENU, VK_UP},
        [resizePiP]() { resizePiP(kPiPResizeSmall, 0, true); }, true);
    m_hotkeyManager->Register("pip_scale_down",
        {VK_MENU, VK_DOWN},
        [resizePiP]() { resizePiP(-kPiPResizeSmall, 0, true); }, true);
    m_hotkeyManager->Register("pip_scale_up_big",
        {VK_MENU, VK_SHIFT, VK_UP},
        [resizePiP]() { resizePiP(kPiPResizeLarge, 0, true); }, true);
    m_hotkeyManager->Register("pip_scale_down_big",
        {VK_MENU, VK_SHIFT, VK_DOWN},
        [resizePiP]() { resizePiP(-kPiPResizeLarge, 0, true); }, true);

    auto changePiPOpacity = [this](float delta) {
        if (m_isPiP && m_config) SetPiPOpacity(m_config->pipOpacity + delta);
    };
    m_hotkeyManager->Register("pip_opacity_down",
        {VK_MENU, VK_LEFT},
        [changePiPOpacity]() { changePiPOpacity(-0.05f); }, true);
    m_hotkeyManager->Register("pip_opacity_up",
        {VK_MENU, VK_RIGHT},
        [changePiPOpacity]() { changePiPOpacity(0.05f); }, true);
    m_hotkeyManager->Register("pip_opacity_down_big",
        {VK_MENU, VK_SHIFT, VK_LEFT},
        [changePiPOpacity]() { changePiPOpacity(-0.1f); }, true);
    m_hotkeyManager->Register("pip_opacity_up_big",
        {VK_MENU, VK_SHIFT, VK_RIGHT},
        [changePiPOpacity]() { changePiPOpacity(0.1f); }, true);

    // Use Ctrl+S instead of F12 -- F12 is hooked by Xbox Game Bar / Game DVR
    // which can inject DLLs into this process and cause heap corruption.
    m_hotkeyManager->Register("screenshot", {VK_CONTROL, 'S'},
        [this]() { m_screenshotRequested = true; });
    // Use Ctrl+F3 (not bare F3) because when the WebView2 settings overlay
    // has keyboard focus, F3 is interpreted by the Edge runtime as "Find
    // next in page" and opens an HTML search bar, overriding the toggle.
    // Ctrl+F3 has no browser meaning so it routes cleanly back here.
    m_hotkeyManager->Register("toggle_overlay", {VK_CONTROL, VK_F3},
        [this]() { ToggleOverlay(); });
    m_hotkeyManager->Register("toggle_settings", {VK_F1},
        [this]() { ToggleSettings(); });
    // ALT+H toggles HDR instantly (works even when settings panel is hidden in HDR).
    // The actual capture-format switch happens inside the run-loop's reconcile
    // pass on the next iteration: this handler only flips the config flag.
    // ReconcileCaptureFormat picks up the divergence between desired and live
    // state and does the full StopCapture -> Close -> Open -> StartCapture cycle
    // on the main thread (the run loop's thread), which is the only place
    // joining the capture worker is safe.
    m_hotkeyManager->Register("toggle_hdr", {VK_MENU, 'H'},
        [this]() {
            if (m_config) {
                m_config->hdrEnabled = !m_config->hdrEnabled;
                AppLog(m_config->hdrEnabled ? L"HDR ON (ALT+H)" : L"HDR OFF (ALT+H)");
            }
            // Refresh the window title so the [HDR]/[SDR] suffix
            // tracks the user's preference. On the 4K S, Reconcile
            // also fires a format swap which calls UpdateWindowTitle
            // again post-Open; on the 4K Pro, capture stays P010 and
            // Reconcile is a no-op, so this is the only place the
            // title gets updated on Alt+H. UpdateWindowTitle is cheap
            // and idempotent (SetWindowTextW with the same string is
            // a no-op), so calling here on both paths is safe.
            UpdateWindowTitle();
            // No source-state re-probe here: the probe's 1.5s MCU
            // re-parse wait would block this main-thread hotkey handler
            // and stall the render loop (contentFps drops to 0 and
            // dropped-frame count spikes during the probe). Source HDR
            // state changes (PS5 HDR toggled while NitLink is open) are
            // handled automatically via ReconcileCaptureFormat's
            // force-reopen path: the MF reader chokes on the media-type
            // change underneath it, the capture worker exits and sets
            // the needs-reopen flag, and the next render-loop iteration
            // reconciles with force=true, which re-probes and updates
            // the title bar. Alt+H here only flips the user's
            // hdrEnabled preference.
        });
    // Ctrl+F4: HDR color-fidelity diagnostic overlay. Draws calibrated
    // reference patches over the bottom of the screen with KNOWN scRGB
    // values, enabling an A/B against a PS5-direct-to-TV signal showing
    // the same patches. If the orange patch shifts red vs native, that's a
    // measurable pipeline bias.
    m_hotkeyManager->Register("toggle_hdr_diag", {VK_CONTROL, VK_F4},
        [this]() {
            if (m_renderer) {
                const bool on = !m_renderer->IsHDRDiagModeOn();
                m_renderer->SetHDRDiagMode(on);
                AppLog(on ? L"HDR diag overlay ON (Ctrl+F4)"
                          : L"HDR diag overlay OFF (Ctrl+F4)");
            }
        });

    // Alt+L: toggle Low-Latency present mode. ON = present-on-arrival (wait for
    // the swap chain at the TOP of the loop, then grab the freshest frame and
    // present it). OFF = VRR/Smooth pacing (skip dup frames, panel follows the
    // content rate). Lets us A/B the two back-to-back on the rig in one keypress.
    m_hotkeyManager->Register("toggle_low_latency", {VK_MENU, 'L'},
        [this]() {
            m_lowLatency = !m_lowLatency;
            if (m_config) {
                m_config->lowLatency = m_lowLatency;
                m_config->Save("nitlink.json");
            }
            AppLog(m_lowLatency ? L"Low-Latency ON (Alt+L): present-on-arrival"
                                : L"Low-Latency OFF (Alt+L): VRR/Smooth pacing");
            PushSettingsState();
        });

    // Alt+R: override the source color-range expansion. MF reports HDR10 as
    // "assume limited", which over-expands a full-range source and pushes skin
    // tones orange. Cycles Auto (MF) -> force Full -> force Limited so the
    // correct range can be found by eye, then locked in.
    m_hotkeyManager->Register("cycle_aspect", {VK_MENU, 'A'},
        [this]() { CycleAspectRatio(); });

    m_hotkeyManager->Register("cycle_color_range", {VK_MENU, 'R'},
        [this]() {
            m_sourceRangeOverride = (m_sourceRangeOverride + 1) % 3;
            UpdateCaptureColorInterpretation(m_currentDeviceInfo.name);
            const std::wstring rmsg =
                m_sourceRangeOverride == 1 ? Tr(L"toast.colorRangeFull")
              : m_sourceRangeOverride == 2 ? Tr(L"toast.colorRangeLimited")
              :                              Tr(L"toast.colorRangeAuto");
            AppLog(rmsg);
            ShowToast(rmsg, std::chrono::milliseconds(1800));
        });

    // Ctrl+F5: capture the current frame's placeholder fingerprint to the
    // debug-output channel. Use this while the Elgato NO SIGNAL placeholder
    // is on screen (HDMI unplugged / source powered off) to obtain the live
    // 9-byte signature, then paste the result into kKnownPlaceholders inside
    // placeholder_detector.cpp. Once baked in, the detector starts
    // suppressing matching placeholder frames automatically.
    m_hotkeyManager->Register("capture_placeholder_fingerprint",
        {VK_CONTROL, VK_F5},
        [this]() {
            m_dumpFingerprintRequested = true;
            AppLog(L"Placeholder fingerprint capture queued (Ctrl+F5), next captured frame will log its signature");
        });

    // Ctrl+F6: HDR levels readout. Measures the captured frame's luma (and
    // chroma) code range and shows it on-screen, so the correct per-card
    // color range can be read off the signal instead of eyeballed. On a
    // reference black/white pattern (the PS5 HDR calibration screen is
    // ideal), a luma floor near 64 (10-bit) means a limited-range source,
    // near 0 means full range. The readout pairs sig: (measured signal) with
    // dec: (how the shader is currently decoding) so the two can be compared
    // and the Alt+R override dialed to match.
    m_hotkeyManager->Register("toggle_levels_diag", {VK_CONTROL, VK_F6},
        [this]() {
            m_levelsDiagOn = !m_levelsDiagOn;
            AppLog(m_levelsDiagOn ? L"HDR levels readout ON (Ctrl+F6)"
                                  : L"HDR levels readout OFF (Ctrl+F6)");
            // Drop any lingering readout immediately on toggle-off.
            if (!m_levelsDiagOn) m_toastText.clear();
        });


    // Apply persisted audio + display state so the live subsystems reflect
    // whatever the user had configured when they last closed the app.
    if (m_audioRouter) {
        m_audioRouter->SetVolume(m_config->audioVolume);
        m_audioRouter->SetMuted(m_config->audioMuted);
    }
    if (m_renderer) {
        m_renderer->SetColorExpansion(m_config->colorExpansion);
    }
    // Apply the persisted low-latency preference (Alt+L / F1 toggle). The run
    // loop reads m_lowLatency to choose present-on-arrival vs vsync pacing.
    m_lowLatency = m_config->lowLatency;

    // Spin up the HDR source poller iff the device class supports the
    // InfoFrame property query. The 4K Pro does; the 4K S over USB does
    // not (different driver, no GUID). When unsupported, leave m_hdrPoller
    // null: the run loop's HasUpdate() check is gated on it.
    //
    // Seed the poller with the init-time source state already captured
    // a few hundred lines up so the FIRST transition the worker reports
    // is a real change vs. the init state, not a redundant copy of it.
    if (m_isGC553Pro) {
        m_hdrPoller = std::make_unique<HDRSourcePoller>();
        m_hdrPoller->StartGc553Pro(m_currentDeviceInfo.name, m_gc553ProSourceState);
    } else if (m_hdrDetectionAvailable && !m_is4KS && !m_is4KX) {
        m_hdrPoller = std::make_unique<HDRSourcePoller>();
        m_hdrPoller->Start(m_currentDeviceInfo.name, m_sourceIsHDR10, m_hdrDetectionAvailable);
    } else {
        AppLog(L"Initialize: HDR source poller skipped (property unsupported on this device)");
    }

    AppLog(L"Initialize: entering run loop");


    m_running = true;
    return true;
}

void Application::Run()
{
    using Clock = std::chrono::high_resolution_clock;

    // A stalled capture stream still needs periodic presents to keep the
    // swap chain fed and make no-signal and settings changes visible.
    constexpr auto kPresentKeepaliveInterval = std::chrono::milliseconds(250);
    auto lastPresentTime = std::chrono::steady_clock::now();
    
    auto lastFpsUpdate = Clock::now();
    uint64_t lastFramesWritten    = 0;
    uint64_t lastUniqueFrameCount = 0;
    uint64_t lastPresentCount     = 0;

    MSG msg{};
    while (m_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                m_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!m_running) break;

        m_hotkeyManager->Poll();

        // Handle window resize
        // CRITICAL: order matters here.
        //   1. Overlay must release its D2D bitmap (which holds a ref to the
        //      old backbuffer) BEFORE the swap chain can resize.
        //   2. Renderer resizes the swap chain.
        //   3. Overlay recreates its D2D bitmap to wrap the new backbuffer.
        // Without step 1 first, DXGI's ResizeBuffers will block forever
        // waiting for the dangling D2D reference to release.
        // Null guard on m_renderer: the device-loss gate below this block only
        // rebuilds the renderer later in the same iteration, so a resize event
        // that lands while the renderer is torn down (failed rebuild pending
        // retry) would otherwise dereference a null renderer here. Skipping the
        // resize is safe: the pending rebuild recreates the swap chain at the
        // current client size, and any later resize event repeats this block.
        if (m_window->WasResized() && m_renderer) {
            auto [w, h] = m_window->GetClientSize();
            AppLog(L"Resize event: handling resize");
            if (m_overlay) m_overlay->OnResizeBegin();
            m_renderer->Resize(w, h);
            ApplyPresentCap();
            // Update the WebView2 child controller bounds so it stays
            // sized to the new client rect. The control is always sized
            // to the full client area; only its visibility toggles.
            if (m_webviewSettings) {
                m_webviewSettings->Resize();
            }
            if (m_overlay) {
                // Always recreate overlay D2D resources on resize. In SDR
                // this rebinds to the new backbuffer; in HDR this recreates
                // the BGRA8 offscreen at the new size. Either way the
                // overlay stays functional after window resize.
                m_overlay->OnResizeEnd();
            }
            m_window->AcknowledgeResize();
            AppLog(L"Resize event: done");
        }

        // HDR source auto-detect drain. Once per second, the HDR poller's
        // worker thread reads the Elgato InfoFrame and flips m_hasUpdate
        // when it sees an EOTF transition (e.g. PS5 dashboard -> HDR game
        // launch). Pull the new state here, push it to the renderer, and
        // let ReconcileCaptureFormat (below) swap the capture format if
        // needed. AcceptUpdate is atomic test-and-clear and a cheap no-op
        // when no update is pending.
        if (m_hdrPoller) {
            if (m_isGC553Pro) {
                Gc553ProSourceHdrState newState{};
                if (m_hdrPoller->AcceptGc553ProUpdate(&newState)) {
                    m_gc553ProSourceState = KeepLastGc553ProSourceState(
                        m_gc553ProSourceState, newState);
                    m_hdrDetectionAvailable = true;
                    m_sourceIsHDR10 = newState == Gc553ProSourceHdrState::Hdr10Pq;
                    m_gc553ProAutoP010Rejected = false;
                    if (newState == Gc553ProSourceHdrState::Sdr)
                        m_gc553ProP010ReopenToastShown = false;
                    if (m_config && m_config->hdrAutoFromSource &&
                        m_lastCaptureIsP010 && m_captureDevice &&
                        m_captureDevice->IsCapturing()) {
                        const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        m_gc553ProSourceFrameSync.Begin(nowNs);
                    } else {
                        m_gc553ProSourceFrameSync.Reset();
                    }
                    AppLog(L"HDR auto-detect: GC553Pro valid source state changed");
                    UpdateWindowTitle();
                }
            } else {
                bool newSourceIsHDR10 = false;
                if (m_hdrPoller->AcceptUpdate(&newSourceIsHDR10)) {
                    AppLog(newSourceIsHDR10
                        ? L"HDR auto-detect: source went HDR10"
                        : L"HDR auto-detect: source went SDR");
                    m_sourceIsHDR10 = newSourceIsHDR10;
                    // Don't push to renderer here: ReconcileCaptureFormat will
                    // re-negotiate capture format on the next line and set the
                    // renderer's HDR10 flag from the resulting actual format.
                    // Pushing m_sourceIsHDR10 here would create a one-iteration
                    // mismatch where the renderer thinks the capture is P010
                    // before the capture device has actually switched.
                }
            }
        }

        // Capture-side HDR reconcile. Runs every iteration; fast no-op when
        // the device is already in the right format. When Alt+H flipped
        // m_config->hdrEnabled, or the HDR poller's drain above flipped
        // m_sourceIsHDR10 in response to a source transition (PS5
        // dashboard -> HDR game launch etc), this is where the actual
        // teardown + re-Open happens. Must run BEFORE the renderer-sync
        // block below so the renderer's swap-chain format swap follows the
        // capture format swap, not the other way around: otherwise P010
        // pixels would briefly render through the BGRA backbuffer (or vice
        // versa) for one iteration.
        //
        // Force-reopen path: the capture worker sets a flag when it exits
        // due to a fatal stream condition (MF_SOURCE_READERF_ERROR, a
        // media-type change underneath the reader, or repeated ReadSample
        // failures exceeding the in-loop retry budget: common on PS5 boot
        // logo / SDR<->HDR boundaries). When the flag is set the format
        // may match the target, but the IMFSourceReader is dead and the
        // worker thread has already exited; the full
        // Stop->Close->Open->Start cycle must run regardless.
        const bool forceReopen = m_captureDevice && m_captureDevice->ConsumeNeedsReopen();
        if (forceReopen) {
            AppLog(L"Capture worker signaled needs-reopen, forcing format reconcile");
        }
        const bool force4KX = m_4kxForceReconcile;
        m_4kxForceReconcile = false;
        ReconcileCaptureFormat(forceReopen || force4KX);

        // HDR toggle sync: when the config flag flips, recreate the swap
        // chain in the new format. Same D2D-release / resize / D2D-recreate
        // dance as a window resize, because the backbuffer is being recreated.
        // If the renderer reports failure (display doesn't support HDR, etc)
        // the config flag is rolled back so the UI reflects reality.
        if (m_config && m_renderer && m_config->hdrEnabled != m_renderer->IsHDREnabled()) {
            const bool target = m_config->hdrEnabled;
            AppLog(target ? L"HDR: enabling..." : L"HDR: disabling...");
            if (m_overlay) m_overlay->OnResizeBegin();
            const bool ok = m_renderer->SetHDREnabled(target);
            if (m_overlay) m_overlay->OnResizeEnd();
            if (!ok && target) {
                AppLog(L"HDR: SetHDREnabled returned false, reverting config flag");
                m_config->hdrEnabled = false;
                // Surface the failure to the user: most users won't be
                // watching DebugView. The renderer's gate refuses when
                // Windows HDR isn't engaged for the active output, so
                // that's the overwhelmingly common reason this branch
                // fires. Keep the text short and tell them what to do.
                ShowToast(Tr(L"toast.windowsHdrDisabled"));
            } else {
                AppLog(target ? L"HDR: enabled" : L"HDR: disabled");
            }
        }

        // LOW-LATENCY (present-on-arrival): wait for the swap chain HERE, at the
        // top, while holding no frame; the Read below then grabs the freshest
        // frame and presents it without the ~one-refresh wait aging it. In
        // VRR/Smooth mode this is skipped and BeginFrame() does the wait as before.
        //
        // Gating this on FrameBuffer::WaitForFrame (arrival-driven present) to
        // kill the frameAge beat regressed hard (frameAge unchanged, ~60% photon
        // misses): the jitter is the capture(143Hz)-vs-display(144Hz) RATE beat;
        // the freshest frame at any display instant is already 0-7 ms old by
        // physics, so a second blocking wait only adds drops, it cannot make the
        // frame fresher. The frame-ready event infra stays in place but unused.
        // Graphics device loss recovery. EndFrame's Present (or the per-frame
        // capture-texture Map) latches device-removed; rebuild the renderer and
        // its dependents before this iteration touches them further. A failed
        // rebuild backs off briefly so the loop does not spin while the GPU is
        // still gone (long driver reinstall, sleep/resume window).
        //
        // A null renderer counts as still-lost: a failed rebuild leaves
        // m_renderer null and returns false, so treating null as recovered
        // would drop straight into the render path below and dereference a null
        // renderer within a frame. Gating on null instead retries the rebuild
        // every backoff interval until the GPU returns, and guarantees the
        // render path below runs only with a live renderer.
        {
            const auto iterStart = std::chrono::steady_clock::now();
            if (m_loopPrevStart.time_since_epoch().count() != 0) {
                m_loopPeriodSumMs += std::chrono::duration<double, std::milli>(iterStart - m_loopPrevStart).count();
                m_loopIterations++;
            }
            m_loopPrevStart = iterStart;
        }
        const bool deviceLost = !m_renderer || m_renderer->ConsumeDeviceLost();
        if (deviceLost) {
            m_window->SetVideoAvailable(false);
            if (!RecoverFromDeviceLost()) {
                Sleep(250);
                continue;
            }
        }

        // The paced modes advance the loop at the source cadence, so they
        // block on the capture buffer's frame-ready event rather than on the
        // swap chain. Limit the wait to the remaining present deadline so a
        // stall after duplicate frames cannot extend the keepalive interval.
        const int pacing = m_config ? m_config->presentPacing : kPacingRefresh;
        if (pacing != kPacingRefresh) {
            const auto remaining = lastPresentTime + kPresentKeepaliveInterval
                - std::chrono::steady_clock::now();
            const auto waitMs = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
            if (m_frameBuffer) {
                m_frameBuffer->WaitForFrame(waitMs > 0 ? static_cast<unsigned long>(waitMs) : 0);
            }
        } else if (m_lowLatency && m_renderer) {
            m_renderer->WaitForFrameReady();
        }

        auto captureStart = Clock::now();

        bool haveFreshFrame         = false;
        bool freshFrameIsRealSource = false;
        FrameBuffer::FrameData frame;
        m_lastPresentationFrameFresh = false;
        // Null guard: ReconcileCaptureFormat resets m_frameBuffer and only
        // rebuilds it if Open() succeeds. If both reopen attempts fail
        // (transient driver failure during a PS5 HDMI handshake, etc.) the
        // buffer stays null until a future forced-reopen succeeds. Skip
        // the read this iteration; the no-signal debounce handles the UX.
        if (m_frameBuffer && m_frameBuffer->Read(frame)) {
            haveFreshFrame = true;

            // Capture-device placeholder-frame detection.
            //
            // The card emits its own NO SIGNAL placeholder as a valid frame
            // stream during HDMI unplug / handshake gaps. Those frames need
            // to be suppressed so the existing 2.5 s no-signal debounce can
            // surface the NitLink no-signal page. The detector fingerprints
            // each frame (3x3 zone-luma signature, format-aware) and
            // declares "placeholder" only when the fingerprint matches a
            // baked-in Elgato signature for kRequiredConsecutiveMatches
            // frames in a row.
            //
            // Why this is safe against false positives on real static
            // content: a paused game / static menu / black scene transition
            // is content-shaped and will not match a known device placeholder's
            // specific zone pattern across all 9 zones. See
            // placeholder_detector.h for the fingerprint and matching rules.
            //
            PlaceholderDetector::Fingerprint fp{};
            auto fmt = PlaceholderDetector::CaptureFormatKind::BGRA;
            auto cls = PlaceholderDetector::FrameClassification::Real;
            auto placeholderFamily =
                PlaceholderDetector::PlaceholderDeviceFamily::Unknown;
            bool knownPlaceholder = false;
            bool shouldUpload = PlaceholderDetector::ShouldUploadCaptureFrame(cls);
            if (m_captureDevice) {
                // Use the metadata latched after Open and before
                // StartCapture. Re-reading the live capture object here can
                // briefly expose a default/old subtype while the first MF
                // sample is already in flight.
                if (m_placeholderCaptureMetadataReady) {
                    fmt = m_placeholderCaptureFormat;
                }
                fp = PlaceholderDetector::Compute(
                    frame.data, frame.size, frame.width, frame.height, fmt);

                placeholderFamily = m_placeholderCaptureMetadataReady
                    ? m_placeholderDeviceFamily
                    : PlaceholderDetector::PlaceholderDeviceFamily::Unknown;
                const bool invalidZeroFilled =
                    PlaceholderDetector::IsInvalidTransitionalFrame(
                        frame.data, frame.size, frame.width, frame.height, fmt);
                const bool neutralTransitional =
                    PlaceholderDetector::IsGc553ProNeutralTransitionalFrame(
                        frame.data, frame.size, frame.width, frame.height, fmt,
                        placeholderFamily, !EffectiveSourceFullRange(),
                        m_captureFrameValidForSession);
                const bool invalidTransitional =
                    invalidZeroFilled || neutralTransitional;
                if (invalidTransitional) {
                    // Do not call Process(): this frame must not advance or
                    // reset the placeholder detector's temporal streak. It
                    // also remains outside the Real path below, so no upload,
                    // last-good/content update, or FrameDiffer input occurs.
                    cls = PlaceholderDetector::FrameClassification::InvalidTransitionalFrame;
                }

                // Debug hotkey: dump this frame's fingerprint and clear the
                // request. The user presses Ctrl+F5 while the Elgato
                // placeholder is on screen to capture the live signature,
                // then pastes the result into kKnownPlaceholders.
                if (m_dumpFingerprintRequested) {
                    m_dumpFingerprintRequested = false;
                    const auto negotiated = m_captureDevice->GetOutputFormat();
                    const auto policy = GetCaptureDevicePolicy(m_currentDeviceInfo.name);
                    const wchar_t* family =
                        policy.family == CaptureDeviceFamily::AverMediaGC553Pro
                            ? L"GC553Pro"
                            : IsElgatoDevice(m_currentDeviceInfo.name)
                                ? L"Elgato"
                                : L"Generic/other";
                    std::wstringstream metadata;
                    metadata << L"Placeholder fingerprint capture format: "
                             << L"deviceFamily=" << family
                             << L", device=\"" << m_currentDeviceInfo.name << L"\""
                             << L", negotiatedSubtype=" << FormatGuidToString(negotiated.subtype)
                             << L", size=" << negotiated.width << L"x" << negotiated.height
                             << L", fps=" << negotiated.fps;
                    AppLog(metadata.str());
                    PlaceholderDetector::LogFingerprint(fp,
                        [](const std::wstring& m) {
                            OutputDebugStringW((L"[NitLink/Placeholder] " + m + L"\n").c_str());
                        });
                }

                if (!invalidTransitional && m_placeholderDetector) {
                    cls = m_placeholderDetector->Process(fp, fmt, placeholderFamily);
                }
                knownPlaceholder = m_placeholderDetector &&
                    m_placeholderDetector->IsKnownPlaceholder(fp, fmt, placeholderFamily);
                shouldUpload = PlaceholderDetector::ShouldUploadCaptureFrame(cls);

                const bool gc553Pro = placeholderFamily ==
                    PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro;
                const bool unstableKnownGc553Pro = gc553Pro &&
                    cls == PlaceholderDetector::FrameClassification::Real &&
                    knownPlaceholder;
                if (unstableKnownGc553Pro && m_captureFrameValidForSession &&
                    !m_gc553ProPlaceholderTransitionGuardConsumed) {
                    // Keep the #16 detector safeguard (unstable known match =>
                    // Real) intact, but do not expose the card's first
                    // transition frame after an already-presented real frame.
                    shouldUpload = false;
                    m_gc553ProPlaceholderTransitionGuardConsumed = true;
                    AppLog(L"GC553Pro: quarantined one unstable known-placeholder transition frame");
                }
                // The P010 stream has no per-sample EOTF tag. Keep the
                // old texture under its old interpretation until a sample
                // delivered after the new XU state was observed can replace it.
                // Classification still runs above, preserving placeholder
                // detector and one-frame guard semantics.
                if (m_isGC553Pro && m_config && m_config->hdrAutoFromSource &&
                    m_lastCaptureIsP010 &&
                    m_gc553ProSourceFrameSync.Hold(frame.arrivalWallNs)) {
                    shouldUpload = false;
                }
                freshFrameIsRealSource =
                    shouldUpload;

                if (gc553Pro) {
                    if (cls == PlaceholderDetector::FrameClassification::ConfirmedPlaceholder) {
                        m_gc553ProStartupBlackHint.ConfirmPlaceholder();
                    } else {
                        const bool zeroLumaZones = std::all_of(
                            fp.begin(), fp.end(), [](uint8_t value) { return value == 0; });
                        m_gc553ProStartupBlackHint.ObserveFrame(shouldUpload, zeroLumaZones);
                    }
                }
                if (gc553Pro) {
                    const std::wstring formatLabel = PlaceholderFormatName(fmt);
                    if (cls == PlaceholderDetector::FrameClassification::CandidatePlaceholder &&
                        !m_placeholderCandidateLogged) {
                        PlaceholderLog(L"GC553Pro " + formatLabel + L" placeholder candidate");
                        m_placeholderCandidateLogged = true;
                    } else if (cls == PlaceholderDetector::FrameClassification::ConfirmedPlaceholder &&
                               !m_placeholderConfirmedLogged) {
                        PlaceholderLog(L"GC553Pro " + formatLabel + L" placeholder confirmed");
                        m_placeholderConfirmedLogged = true;
                    } else if (cls == PlaceholderDetector::FrameClassification::Real &&
                               (m_placeholderCandidateLogged || m_placeholderConfirmedLogged)) {
                        PlaceholderLog(L"Placeholder cleared by real frame");
                        m_placeholderCandidateLogged = false;
                        m_placeholderConfirmedLogged = false;
                    }
                }
            } else {
                // No detector available: fall back to "every fresh frame
                // is real" so behavior never regresses versus pre-detector state.
                freshFrameIsRealSource = true;
            }

            // Re-arm only when a non-placeholder Real frame follows an actual
            // fresh Candidate/Confirmed placeholder classification. Ordinary
            // Real -> Real motion can never repeatedly trigger the guard.
            const bool gc553Pro = placeholderFamily ==
                PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro;
            const bool candidateOrConfirmedPlaceholder =
                cls == PlaceholderDetector::FrameClassification::CandidatePlaceholder ||
                cls == PlaceholderDetector::FrameClassification::ConfirmedPlaceholder;
            if (gc553Pro && cls == PlaceholderDetector::FrameClassification::Real &&
                !knownPlaceholder && m_gc553ProPreviousFreshFrameWasPlaceholder) {
                m_gc553ProPlaceholderTransitionGuardConsumed = false;
            }
            m_gc553ProPreviousFreshFrameWasPlaceholder =
                gc553Pro && candidateOrConfirmedPlaceholder;

            m_lastPresentationFrameFresh = true;
            m_lastPresentationClassification = cls;

            // Four-way branch on the classifier:
            //
            //   Real                  : upload, bump grace timer, run differ.
            //   CandidatePlaceholder  : suppress upload + differ + grace
            //                           bump. Do NOT log: the streak may
            //                           still break on the next frame.
            //                           This prevents even a single placeholder
            //                           frame from polluting m_captureTexture
            //                           before confirmation is reached.
            //   ConfirmedPlaceholder  : same suppression as candidate, plus
            //                           log the transition once and let the
            //                           shorter placeholder-specific grace
            //                           inside ShouldShowNoSignal kick in.
            //   InvalidTransitionalFrame: structural zero-filled sample;
            //                             suppress without touching detector
            //                             or source-liveness state.
            const bool commitGc553ProEotf = shouldUpload && m_isGC553Pro &&
                m_config && m_config->hdrAutoFromSource && m_lastCaptureIsP010 &&
                m_gc553ProSourceFrameSync.Ready(frame.arrivalWallNs);
            if (commitGc553ProEotf) {
                // Invalidate just before replacing the texture. A failed GPU
                // upload cannot expose the old frame under the new EOTF flag.
                m_renderer->InvalidateCaptureFrame();
                m_renderer->UpdateCaptureTexture(
                    frame.data, frame.size, frame.width, frame.height);
                if (m_renderer->HasCaptureFrame()) {
                    m_renderer->SetSourceIsHDR10(m_sourceIsHDR10);
                    m_gc553ProSourceFrameSync.Reset();
                } else {
                    shouldUpload = false;
                    freshFrameIsRealSource = false;
                }
            }
            if (shouldUpload) {
                if (!commitGc553ProEotf) {
                    m_renderer->UpdateCaptureTexture(
                        frame.data, frame.size, frame.width, frame.height);
                }
                m_captureFrameValidForSession = true;
                auto captureEnd = Clock::now();
                m_captureLatencyMs = std::chrono::duration<double, std::milli>(captureEnd - captureStart).count();

                // App ingest: real card-driver-to-app delivery time.
                // MFSampleExtension_DeviceTimestamp is in QPC 100ns units and
                // shares the QPC epoch with steady_clock on Windows, so the
                // delta against arrivalWallNs is the actual delivery latency.
                // Replaces the 16+8 baked constants from the old HUD formula.
                // If the driver doesn't populate the attribute,
                // deviceTimestamp is 0 and the reported latency is also
                // 0 (rather than a garbage huge-negative subtraction).
                if (frame.deviceTimestamp != 0) {
                    const int64_t deviceTimestampNs = (int64_t)frame.deviceTimestamp * 100;
                    const int64_t deliveryDeltaNs   = frame.arrivalWallNs - deviceTimestampNs;
                    m_mfDeliveryLatencyMs = deliveryDeltaNs / 1'000'000.0;
                } else {
                    m_mfDeliveryLatencyMs = 0.0;
                }
                // Periodic visibility: log every ~60 real frames (~1s @60fps)
                // without spamming.
                if ((m_mfDeliveryLogCounter++ % 60) == 0) {
                    AppLog(L"App ingest: " + std::to_wstring(m_mfDeliveryLatencyMs) + L" ms");
                }

                m_lastGoodFrameTime    = std::chrono::steady_clock::now();
                if (m_signalLostLogged || m_signalReacquiringLogged ||
                    m_noSignalPresentationLatched) {
                    AppLog(L"Signal: accepted real frame cleared No Signal presentation latch");
                }
                m_signalLostLogged = false;
                m_signalReacquiringLogged = false;
                // Only a newly accepted Real frame may clear the
                // presentation latch. Detector Reset/reconcile alone must
                // never expose an invalidated capture surface or the card's
                // own placeholder during HDR format changes.
                m_noSignalPresentationLatched = false;
                if (m_captureTransitionActive) {
                    AppLog(L"HDR transition: accepted real frame restored capture presentation");
                    m_captureTransitionActive = false;
                }
                m_gc553ProP010ReopenPresentation = false;
                // Motion-recency gate: this frame is non-placeholder content
                // arriving from the capture device, so bump the content
                // freshness marker the placeholder gate consults. See
                // application.h for the gate's full semantics.
                m_lastContentTime      = m_lastGoodFrameTime;
                m_hasEverReceivedFrame = true;
                if (m_inPlaceholderState) {
                    AppLog(L"Signal: real source frames resumed");
                    m_inPlaceholderState = false;
                }
                if (m_placeholderHold.exchange(false, std::memory_order_acq_rel)) {
                    AppLog(L"Signal: copy gate released after dropping " +
                           std::to_wstring(m_placeholderDropped.load(std::memory_order_relaxed)) +
                           L" placeholder frames");
                }

                // HDR levels readout (Ctrl+F6): measure this frame's code
                // range so the per-card color range is read off the signal,
                // not eyeballed. Throttled to ~4 Hz; the scan is a subsampled
                // histogram costing a fraction of a millisecond. Helpers are
                // ComputeFrameLevels / FormatLevelsText at the top of the file.
                if (m_levelsDiagOn && m_captureDevice) {
                    const auto lvNow = std::chrono::steady_clock::now();
                    if (lvNow - m_lastLevelsCompute >= std::chrono::milliseconds(250)) {
                        m_lastLevelsCompute = lvNow;
                        const auto sub = m_captureDevice->GetOutputFormat().subtype;
                        const bool isP010 = IsEqualGUID(sub, MFVideoFormat_P010);
                        const bool isNV12 = IsEqualGUID(sub, MFVideoFormat_NV12);
                        const FrameLevels lv = ComputeFrameLevels(
                            frame.data, frame.size, frame.width, frame.height,
                            isP010, isNV12);
                        ShowToast(FormatLevelsText(lv, EffectiveSourceFullRange()),
                                  std::chrono::milliseconds(1200));
                    }
                }
            } else if (cls == PlaceholderDetector::FrameClassification::ConfirmedPlaceholder) {
                // MOTION-RECENCY GATE.
                //
                // PlaceholderDetector confirmed: this frame is the
                // kRequiredConsecutiveMatches-th in a row whose fingerprint
                // matches a baked placeholder, and the streak passed the
                // temporal-stability check. That is sufficient evidence for
                // a real capture-device NO SIGNAL placeholder, but it is ALSO
                // sufficient evidence for a static game intro card whose
                // luma pattern coincidentally matches the placeholder
                // (Star Wars Jedi Lucasfilm logo on P010, etc.).
                //
                // To break the tie, check history: if either real
                // content (m_lastContentTime) or motion (m_lastMotionTime)
                // was seen inside kMotionRecencyWindowMs, the HDMI source
                // is alive and this is a false positive. Fall through
                // exactly like CandidatePlaceholder (no log, no latch, no
                // upload; the Real branch did not run, so no upload
                // happened).
                //
                // If both timers have gone stale, the source has been
                // quiet long enough that this really is a disconnect.
                // Honor the confirmation: log the transition once and
                // arm the placeholder-specific shorter no-signal grace
                // inside ShouldShowNoSignal.
                const auto now = std::chrono::steady_clock::now();
                if (!RecentSourceActivity(now)) {
                    if (!m_inPlaceholderState) {
                        AppLog(L"Signal: capture-device placeholder detected, holding last real frame, no upload");
                        m_inPlaceholderState = true;
                    }
                    // Arm the capture-thread copy gate with the confirmed
                    // fingerprint so the placeholder stream stops being copied.
                    {
                        std::lock_guard<std::mutex> lk(m_placeholderFpMutex);
                        m_placeholderFp  = fp;
                        m_placeholderFmt = fmt;
                        m_placeholderW   = frame.width;
                        m_placeholderH   = frame.height;
                    }
                    if (!m_placeholderHold.exchange(true, std::memory_order_acq_rel)) {
                        m_placeholderDropped.store(0, std::memory_order_relaxed);
                        AppLog(L"Signal: placeholder frames now dropped before copy");
                    }
                }
                // else: suppress confirmation. Stay out of m_inPlaceholderState
                // so the no-signal grace does not arm. The upload-skip behavior
                // is already correct because the Real branch did not run.
            }
            // CandidatePlaceholder: no upload, no grace bump, no log. Wait
            // and see whether the streak breaks (back to Real) or completes
            // (Confirmed).
        }
        // No fresh frame at all: existing no-fresh-frame debounce path
        // (ShouldShowNoSignal + m_lastGoodFrameTime) handles it unchanged.

        // Evaluate the no-signal debounce ONCE per iteration. ShouldShowNoSignal
        // owns the latched "reacquiring"/"lost"/"restored" log transitions, so
        // calling it multiple times in the same iteration would double-log
        // state changes. The render block below reads showNoSignalNow at
        // both the HDR and SDR no-signal trigger sites.
        const bool showNoSignalNow = ShouldShowNoSignal();
        const bool noSignalPresentation =
            m_noSignalPresentationLatched || showNoSignalNow;
        const bool rendererHasFrame = m_renderer && m_renderer->HasCaptureFrame();
        const bool captureReady = CapturePresentationReady(
            m_captureFrameValidForSession, rendererHasFrame);
        const NitLink::PresentationState presentationState =
            NitLink::DecidePresentation(
                noSignalPresentation,
                showNoSignalNow,
                captureReady,
                m_captureTransitionActive);
        // Keep the diagnostic transition-only: it reports the state change,
        // the current renderer/app readiness split, the latest classification,
        // and the branch the render section will select. This is deliberately
        // not emitted for every render iteration.
        const bool panelCoversPicture = m_settingsVisible && m_webviewSettings &&
            m_webviewSettings->GetDock() == WebViewSettings::Dock::Full;
        m_window->SetVideoAvailable(
            presentationState == PresentationState::Capture && !panelCoversPicture);
        if (!m_hasPresentationStateDiagnostic ||
            presentationState != m_lastPresentationStateDiagnostic) {
            const wchar_t* reason = L"state evaluation";
            if (presentationState == PresentationState::WaitingForCapture) {
                if (m_lastPresentationFrameFresh &&
                    m_lastPresentationClassification ==
                        PlaceholderDetector::FrameClassification::InvalidTransitionalFrame) {
                    reason = L"invalid transitional frame is not presentable";
                } else if (m_lastPresentationFrameFresh &&
                           m_lastPresentationClassification ==
                        PlaceholderDetector::FrameClassification::CandidatePlaceholder) {
                    reason = L"candidate placeholder is not presentable";
                } else if (m_lastPresentationFrameFresh &&
                           m_lastPresentationClassification ==
                                PlaceholderDetector::FrameClassification::ConfirmedPlaceholder) {
                    reason = L"confirmed placeholder is not capture content";
                } else if (!m_captureFrameValidForSession) {
                    reason = L"no accepted Real frame for current capture session";
                } else if (!rendererHasFrame) {
                    reason = L"accepted frame has no renderer capture texture";
                } else {
                    reason = L"waiting for presentable capture";
                }
            } else if (presentationState == PresentationState::Capture) {
                reason = L"accepted Real frame is presentable";
            } else if (presentationState == PresentationState::NoSignal) {
                reason = noSignalPresentation
                    ? L"No Signal latch/debounce is authoritative"
                    : L"No Signal presentation selected";
            } else if (presentationState == PresentationState::Transition) {
                reason = L"capture/output transition active";
            }

            const std::wstring oldState = m_hasPresentationStateDiagnostic
                ? PresentationStateName(m_lastPresentationStateDiagnostic)
                : L"<none>";
            const std::wstring finalBranch = panelCoversPicture
                ? L"SettingsBlack"
                : PresentationStateName(presentationState);
            AppLog(L"Presentation transition: " + oldState + L" -> " +
                   PresentationStateName(presentationState) +
                   L"; reason=" + reason +
                   L"; m_hasFrame=" + (rendererHasFrame ? L"yes" : L"no") +
                   L"; captureFrameValid=" +
                   (m_captureFrameValidForSession ? L"yes" : L"no") +
                   L"; frameClassification=" +
                   (m_lastPresentationFrameFresh
                        ? PlaceholderClassificationName(m_lastPresentationClassification)
                        : L"none") +
                   L"; finalRenderBranch=" + finalBranch);
            m_hasPresentationStateDiagnostic = true;
            m_lastPresentationStateDiagnostic = presentationState;
        }

        // 4K Pro source timing (props 210 and 208) needs a live HDMI signal.
        // UpdateWindowTitle only composes cached state, so a startup read with
        // no source must be followed by a new read on the signal-return edge.
        // Keep that edge armed until a fresh, non-placeholder frame arrives:
        // startup/reacquire grace can hide the no-signal page without proving
        // that the card's source registers are populated yet.
        //
        // Consume the edge before querying, including on a failed read. The
        // DirectShow filter open and property calls run once per signal return,
        // never once per render iteration. HDR InfoFrame availability does not
        // gate these separate timing properties. The 4K S and 4K X retain their
        // own source-information paths; the Pro result only refreshes the title.
        if (showNoSignalNow) {
            m_prevSourceNoSignal = true;
        } else if (m_prevSourceNoSignal && haveFreshFrame && freshFrameIsRealSource) {
            m_prevSourceNoSignal = false;
            if (!m_is4KS && !m_is4KX && IsElgatoDevice(m_currentDeviceInfo.name)) {
                m_source4KProMode = Detect4KProSourceMode(m_currentDeviceInfo.name);
                UpdateWindowTitle();
            }
        }

        // 4K X: a background poller reads the live source mode off the render
        // thread (Detect4KXSourceMode opens a DirectShow filter, ~50-100ms; too
        // slow for this loop). Start is idempotent. On a source-mode change, force
        // a reconcile so capture re-matches the source resolution/fps. A live PS5
        // resolution change is too brief to trip the no-signal edge, so polling is
        // what catches it.
        if (m_is4KX) {
            // 4K X source poller: opens the XU once and holds it (Start is
            // idempotent), reads resolution/fps/HDR/source name off the render
            // thread via the readiness-poll + paced sequence the Elgato app uses.
            m_4kxPoller.Start(m_currentDeviceInfo.name, m_source4KProMode);
            if (m_4kxPoller.AcceptUpdate(&m_source4KProMode)) {
                m_4kxForceReconcile = true;
                AppLog(L"4K X source mode: "
                       + std::to_wstring(m_source4KProMode.width) + L"x"
                       + std::to_wstring(m_source4KProMode.height) + L"@"
                       + std::to_wstring(m_source4KProMode.fps)
                       + (m_source4KProMode.hdrActive ? L" HDR" : L" SDR"));
            }
        }

        // =====================================================================
        // VRR PRESENT PACING: differ-driven Present
        // =====================================================================
        // The Elgato 4K Pro delivers frames at constant 60Hz to Media Foundation
        // regardless of the source's actual framerate: frame duplication happens
        // at the HDMI signal level. So in v1.0 with naive present-every-frame,
        // the monitor's VRR sees a constant 60Hz Present rate even when the PS5
        // game is running at 30fps or has variable framerate.
        //
        // Fix: run the GPU frame differ HERE, before the render decision. If the
        // captured frame is a duplicate of the previous one (frame duplication at
        // the HDMI level), skip Present entirely. With Independent Flip +
        // ALLOW_TEARING active on the swap chain, the monitor's G-Sync/FreeSync
        // VRR will sync to the actual unique-frame rate instead of constant 60Hz.
        //
        // Verified working: LG C3 OLED Game Dashboard reports refresh rate
        // tracking the application's Present rate when NitLink is the focused
        // window.
        //
        // The differ runs on the raw capture SRV which was populated by
        // UpdateCaptureTexture above. The differ is hoisted here so its result
        // can gate the entire render+Present block.
        //
        // Fallback: if VRR pacing is disabled, render+present always (v1.0
        // behavior). Also: if differ isn't ready (first few frames) always
        // present. The differ is conservative: it defaults to "is new" until
        // it has previous-frame data to compare against.
        // =====================================================================
        bool isNewFrame = true;  // default to "present" for safety
        // freshFrameIsRealSource gate keeps the differ off Elgato placeholder
        // frames. Feeding the differ a stretch of placeholder luma would
        // let m_prevTex go stale relative to the next real frame: the first
        // post-recovery diff would either spike (false "new") or get
        // suppressed by the ring-buffer smoother depending on smoothing
        // state, both bad. Skipping the differ entirely keeps m_prevTex
        // anchored to the last real-source frame so recovery diffs against
        // a sensible baseline.
        if (haveFreshFrame && freshFrameIsRealSource
            && m_frameDiffer && m_renderer->GetRawCaptureSRV()) {
            const auto differStart = std::chrono::steady_clock::now();
            m_frameDiffer->Process(m_renderer->GetContext(),
                                     m_renderer->GetRawCaptureSRV());
            m_loopDifferSumMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - differStart).count();
            isNewFrame = m_frameDiffer->WasPreviousFrameNew();
            m_sourceCadence.OnClassifiedFrame(isNewFrame);
            if (isNewFrame) {
                m_uniqueFrameCount++;
                // Motion-recency gate: the differ saw a non-duplicate frame,
                // so the HDMI source is producing fresh content. This is the
                // motion half of the (motion OR content) gate consulted by
                // the ConfirmedPlaceholder branch above. Updated here
                // rather than inside the Real branch so that paused games
                // (which classify Real but produce duplicates) leave
                // m_lastMotionTime stale; the content timer carries them.
                m_lastMotionTime = std::chrono::steady_clock::now();
            }
        } else if (haveFreshFrame) {
            m_sourceCadence.OnUnclassifiedFrame();
        }

        // FPS sampling and differ diagnostic: fire every iteration (NOT
        // inside the render block). Otherwise when VRR pacing skips render+
        // Present, the FPS counter would freeze on stale values and the
        // periodic log wouldn't print, hiding differ behavior.
        {
            auto now = Clock::now();
            auto elapsed = std::chrono::duration<double>(now - lastFpsUpdate).count();
            if (elapsed >= 1.0) {
                // Null guard: m_frameBuffer can be null between a failed
                // reopen and a later successful one. Treat as "no new frames
                // written this window" so m_currentFps naturally reads 0.
                uint64_t framesWritten = m_frameBuffer
                    ? m_frameBuffer->GetFramesWritten()
                    : lastFramesWritten;
                // Buffer-reset guard: framesWritten resets to 0 on every
                // FrameBuffer rebuild (HDR/SDR reconcile, format change).
                // Without this guard, 0 - lastFramesWritten underflows the
                // unsigned subtraction and produces a ~2^32 fps spike on
                // the next sample.
                uint64_t deltaFrames = (framesWritten >= lastFramesWritten)
                    ? framesWritten - lastFramesWritten
                    : 0;
                m_currentFps         = static_cast<uint32_t>(deltaFrames / elapsed);
                lastFramesWritten    = framesWritten;

                uint64_t deltaUnique   = m_uniqueFrameCount - lastUniqueFrameCount;
                m_currentContentFps    = static_cast<uint32_t>(deltaUnique / elapsed);
                lastUniqueFrameCount   = m_uniqueFrameCount;

                uint64_t deltaPresents = m_presentCount - lastPresentCount;
                m_currentPresentFps    = static_cast<uint32_t>(deltaPresents / elapsed);
                lastPresentCount       = m_presentCount;

                lastFpsUpdate          = now;

                if (m_settingsVisible) PushSettingsState();
            }

            // VRR pacing diagnostic, every 2 seconds. Reports exactly what
            // the differ is observing and whether classification is stable.
            if (m_frameDiffer) {
                static auto lastDiffLog = Clock::now();
                if (std::chrono::duration<double>(now - lastDiffLog).count() >= 2.0) {
                    // dropped = total frames the producer (capture worker)
                    // overran because the renderer hadn't picked up the
                    // previous fresh frame yet. Monotonic since FrameBuffer
                    // construction (rebuilt on every reconcile), so it
                    // resets to 0 across format swaps. A non-zero delta
                    // between consecutive 2-second log lines means frames
                    // are actively being dropped right now: useful when
                    // chasing transition-time anomalies.
                    const uint64_t dropped =
                        m_frameBuffer ? m_frameBuffer->GetFramesDropped() : 0;
                    std::wstringstream ss;
                    ss << L"VRR pacing: lastDiff=" << m_frameDiffer->GetLastDiffValue()
                       << L" threshold=" << m_frameDiffer->GetThreshold()
                       << L" maxTile=" << m_frameDiffer->GetLastMaxTileValue()
                       << L" tileThr=" << m_frameDiffer->GetTileThreshold()
                       << L" contentFps=" << m_currentContentFps
                       << L" hdmiFps=" << m_currentFps
                       << L" dropped=" << dropped
                       << L" totalSkipped=" << m_skippedFrameCount
                       << L" consecutive=" << m_consecutiveSkips
                       << L" cadence=" << m_sourceCadence.CadenceFrames()
                       << L" dupRun=" << m_sourceCadence.DuplicateRun()
                       << L" holdPresents=" << m_cadenceHoldPresents
                       << L" presentFps=" << m_currentPresentFps
                       << L" thisFrameNew=" << (isNewFrame ? L"Y" : L"N")
                       << L" haveFresh=" << (haveFreshFrame ? L"Y" : L"N")
                       << L" frameAge=" << m_frameAgeMs
                       << L" renderMs=" << m_renderLatencyMs;
                    // Where the loop spent its time since the previous line:
                    // period between iterations, frame-ready wait, capture
                    // upload, differ, and Present, as per-iteration averages
                    // (Present per presented frame).
                    {
                        const DX11Renderer::PhaseTimes ph =
                            m_renderer ? m_renderer->ConsumePhaseTimes() : DX11Renderer::PhaseTimes{};
                        const double iters = m_loopIterations > 0 ? (double)m_loopIterations : 1.0;
                        ss << L" loopMs=" << (m_loopPeriodSumMs / iters)
                           << L" waitMs=" << ph.waitMs
                           << L" uploadMs=" << ph.uploadMs
                           << L" differMs=" << (m_loopDifferSumMs / iters)
                           << L" presentMs=" << ph.presentMs
                           << L" iters=" << m_loopIterations
                           << L" presents=" << ph.presents
                           << L" readbackSkips=" << m_frameDiffer->GetReadbackSkips()
                           << L" compMode=" << (m_renderer ? m_renderer->PresentationMode() : -1);
                        m_loopPeriodSumMs = 0.0;
                        m_loopDifferSumMs = 0.0;
                        m_loopIterations  = 0;
                    }
                    AppLog(ss.str());
                    lastDiffLog = now;
                    const int mode = m_renderer ? m_renderer->PresentationMode() : -1;
                    if (mode != m_lastPresentationMode) {
                        AppLog(L"Presentation mode: " + std::to_wstring(m_lastPresentationMode)
                               + L" -> " + std::to_wstring(mode)
                               + L" (0 composed, 1 overlay, 2 none, 3 failure)");
                        m_lastPresentationMode = mode;
                    }
                }
            }
        }

        // Decision: should this iteration render and Present?
        //
        // kPacingRefresh: Present every iteration. The waitable swap chain
        //   paces the loop near the desktop refresh rate, which is the
        //   lowest-latency behavior and the default.
        //
        // kPacingCaptured: Present once per frame the card delivers. The loop
        //   already blocked on the capture buffer above, so a fresh frame is
        //   the normal case and the present rate follows the HDMI cadence.
        //
        // kPacingUnique: Present only on a fresh frame the differ classifies
        //   as new content, so the present rate follows the real source frame
        //   rate. A variable refresh monitor then syncs to the source instead
        //   of the card's constant delivery rate, an external frame-generation
        //   tool reads the real frame rate off the present rate, and a 30 fps
        //   source stops juddering against a present rate that is not a
        //   multiple of the content rate. While the picture is still, the
        //   source cadence hold repeats the latest frame at the measured
        //   source rate, so a paused game or an idle menu keeps presenting at
        //   the source rate and a one-frame menu change reaches the screen on
        //   the next present.
        //
        // Use elapsed time for the safety floor: duplicate-frame cadence can
        // vary, and a stalled capture stream produces no frame-ready events.
        bool shouldRender;
        bool cadenceHold = false;
        if (pacing == kPacingRefresh) {
            shouldRender = true;
        } else if (presentationState != PresentationState::Capture) {
            // Waiting/Transition/NoSignal are independent presentations. They
            // must keep being rendered even when no fresh capture sample is
            // available; otherwise BeginFrame/Present pacing can expose a
            // cleared or undefined backbuffer during startup.
            shouldRender = true;
        } else if (!haveFreshFrame) {
            // The frame-ready wait timed out: the source is stalled or gone.
            shouldRender = false;
        } else if (pacing == kPacingCaptured || isNewFrame) {
            shouldRender = true;
        } else if (pacing == kPacingUnique && m_sourceCadence.ShouldHold()) {
            shouldRender = true;
            cadenceHold  = true;
        } else {
            shouldRender = false;
        }

        if (shouldRender) {
            m_consecutiveSkips = 0;
        } else if (std::chrono::steady_clock::now() - lastPresentTime >= kPresentKeepaliveInterval) {
            shouldRender = true;
            m_consecutiveSkips = 0;
        }

        if (!shouldRender) {
            // Duplicate frame detected, VRR pacing on: skip the entire
            // render+Present block. The monitor will hold the previous frame
            // an extra cycle, which is exactly what VRR is designed for.
            //
            // Small sleep to not spin the CPU. Tuned to be short enough that
            // the wake happens well before the next capture frame arrives (capture
            // worker delivers at ~16.6ms intervals for 60Hz HDMI), but long
            // enough to drop CPU usage meaningfully.
            m_skippedFrameCount++;
            m_consecutiveSkips++;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        m_sourceCadence.OnPresent();
        m_presentCount++;
        if (cadenceHold) m_cadenceHoldPresents++;

        // Small sleep to avoid 100% CPU spin when running uncapped.
        // 1ms is short enough to be imperceptible but stops the render loop
        // from hammering at 3000+ fps doing nothing useful.
        // LATENCY TEST (2026-06-02): disabled -- cost ~1ms on every presented
        // frame; the waitable swapchain already paces the loop, so this was
        // redundant. Restore if idle-content CPU spin returns.
        // std::this_thread::sleep_for(std::chrono::milliseconds(1));

        auto renderStart = Clock::now();

        // Frame-age localizer: ms from MF delivering this frame to the moment
        // rendering starts on it. arrivalWallNs and Clock both ride steady_clock,
        // so the subtraction is real elapsed time. Guard on a fresh frame; a
        // re-presented iteration leaves frame.arrivalWallNs at 0.
        if (haveFreshFrame && frame.arrivalWallNs > 0) {
            const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                renderStart.time_since_epoch()).count();
            m_frameAgeMs = (double)(nowNs - frame.arrivalWallNs) / 1.0e6;
        }

        // HDR path renders the capture directly into the HDR10 PQ
        // backbuffer. D2D can't target R10G10B10A2 HDR10 backbuffers, so
        // overlay D2D content paints into a BGRA8 offscreen and gets
        // composited via CompositeUI in this branch.
        const bool hdrActive = m_renderer && m_renderer->IsHDREnabled();

        // Source-paced modes already wait for capture arrivals. Waiting on
        // DXGI after selecting a frame would age it and allow later capture
        // frames to replace one another before the next read.
        const bool waitForSwapChain = pacing == kPacingRefresh && !m_lowLatency;

        // When the settings overlay is visible, render a solid black
        // frame underneath the WebView2 child every iteration. This is
        // simpler and more reliable than freezing the last rendered
        // frame (which depended on flip-discard NOT discarding it,
        // unreliable, and any swap chain recreation like toggling HDR
        // wiped the stored frame, leaving a flicker). Solid black is
        // what real game pause menus do; the WebView2 child renders on
        // top normally and the swap chain keeps presenting clean black
        // pixels behind it.
        //
        // Important: BeginFrame/EndFrame still run because the swap
        // chain MUST keep presenting; on a bare Sleep, DWM eventually
        // marks the window as unresponsive and the WebView2 child can
        // get its compositing context torn down.
        // A docked panel leaves the picture running beside it, so the black
        // frame applies only to the full-window panel.
        if (panelCoversPicture) {
            m_renderer->BeginFrame(waitForSwapChain);   // clears to (0,0,0,1): solid black
            m_renderer->EndFrame();     // presents the black frame
            lastPresentTime = std::chrono::steady_clock::now();
            continue;                    // skip the rest of the pipeline
        }

        // Start/advance the presentation-only cap only on an iteration that
        // will actually render the hint. This keeps startup time, skipped
        // duplicate presents, and the full-window settings blackout from
        // consuming its visible-duration budget.
        const bool showGc553ProStartupWaitingCaption =
            shouldRender && m_overlay && m_renderer &&
            ShouldDrawStartupBlackFrameHint(
                m_placeholderDeviceFamily ==
                    PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro,
                presentationState, m_gc553ProStartupBlackHint,
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count()));

        const auto drawWaitingStatus = [this]() {
            if (!m_overlay || !m_renderer) return;
            const bool captureStarted =
                m_captureDevice && m_captureDevice->IsCapturing();
            m_overlay->DrawStatusMessage(
                m_renderer->GetWindowWidth(), m_renderer->GetWindowHeight(),
                captureStarted ? L"overlay.waitingForSource"
                               : L"overlay.initializingCapture");
            if (m_overlay->IsUsingOffscreen()) {
                m_renderer->CompositeUI(m_overlay->GetOffscreenSRV());
            }
        };

        if (hdrActive) {
            // HDR path bypasses NIS (the NIS compute shader is wired for the
            // SDR pipeline today). Make sure DrawCaptureFrame writes to the
            // backbuffer, not the post-input intermediate. If the user
            // toggled NIS on while in SDR and then turned HDR on, the
            // renderer would otherwise still have post-input enabled from
            // the previous SDR iteration, and DrawCaptureFrame would paint
            // into the unused intermediate, leaving the backbuffer black.
            m_renderer->SetPostInputEnabled(false);

            m_renderer->BeginFrame(waitForSwapChain);
            if (presentationState == PresentationState::Capture) {
                m_renderer->DrawCaptureFrame();
            }

            // HDR color-fidelity diagnostic overlay (Ctrl+F4). Draws known
            // scRGB reference patches over the bottom of the captured frame
            // for visual A/B against a PS5-direct-to-TV signal showing the
            // same patches. Patches use raw scRGB values that bypass the
            // capture pipeline entirely: any color shift between the local
            // overlay and the native render is measurable pipeline bias.
            if (m_renderer->IsHDRDiagModeOn()) {
                m_renderer->DrawHDRDiagnostics();
            }

            // No-signal screen takes precedence over the HUD overlay: when
            // there's no HDMI input, the branded screen is drawn instead of
            // the (meaningless) FPS/latency HUD. This also sidesteps the
            // HDR offscreen-sharing complication: only one of these draws
            // into the BGRA8 offscreen per frame.
            //
            // showNoSignalNow is the debounced decision from
            // ShouldShowNoSignal(): true only after the grace period has
            // elapsed without a fresh frame. During the grace period
            // DrawNoSignal is skipped entirely: DrawCaptureFrame above
            // already repainted the last good frame from m_captureTexture,
            // which is exactly the "keep showing the last good capture"
            // behavior wanted for PS5 boot logo / source-switch transitions.
            //
            // Null guard mirrors the run-loop Read site above: if a failed
            // reopen left m_frameBuffer null, report no signal. The
            // user-visible no-signal page is still gated by
            // showNoSignalNow, so this only affects the HUD stats path.
            const bool signalActive = m_frameBuffer && m_frameBuffer->IsSignalActive();

            if (presentationState == PresentationState::Transition && m_overlay) {
                if (m_gc553ProP010ReopenPresentation) {
                    drawWaitingStatus();
                } else {
                    m_overlay->DrawStatusMessage(
                        m_renderer->GetWindowWidth(), m_renderer->GetWindowHeight(),
                        L"overlay.switchingHdr");
                    if (m_overlay->IsUsingOffscreen()) {
                        m_renderer->CompositeUI(m_overlay->GetOffscreenSRV());
                    }
                }
            } else if (presentationState == PresentationState::NoSignal && m_overlay) {
                m_overlay->DrawNoSignal(m_renderer->GetWindowWidth(),
                                        m_renderer->GetWindowHeight());
                if (m_overlay->IsUsingOffscreen()) {
                    m_renderer->CompositeUI(m_overlay->GetOffscreenSRV());
                }
            } else if (presentationState == PresentationState::WaitingForCapture) {
                drawWaitingStatus();
            }
            // PiP gate: the HUD panel is a fixed 280x156 px overlay, which
            // dominates a 480x270 PiP window and looks broken. Suppress it
            // entirely while PiP is active; the panel still renders
            // normally when PiP is off.
            else if (m_showOverlay && m_overlay && !m_isPiP) {
                Overlay::Stats stats{};
                stats.captureLatencyMs = m_captureLatencyMs;
                stats.renderLatencyMs  = m_renderLatencyMs;
                stats.appIngestMs      = m_mfDeliveryLatencyMs;
                stats.gpuMs            = m_renderer->GetLastGpuMs();
                // Content fps comes from the frame differ (detects unique
                // frames). When the scene is static the differ correctly
                // reports 0, but a "0 fps" reading is misleading because
                // the game IS still running. Fall back to the HDMI signal
                // rate in that case so the overlay never lies about it.
                // Source frame rate pacing reports presents per second
                // instead, the rate an external frame-generation tool sees.
                stats.fps = (pacing == kPacingUnique && m_currentPresentFps > 0)
                              ? m_currentPresentFps
                              : (m_frameDiffer && m_currentContentFps > 0)
                                  ? m_currentContentFps
                                  : m_currentFps;
                stats.captureWidth     = m_captureDevice->GetOutputFormat().width;
                stats.captureHeight    = m_captureDevice->GetOutputFormat().height;
                stats.deviceName       = m_captureDevice->GetDeviceName();
                stats.signalActive     = signalActive;
                // Pipeline feature flags shown as the HUD's bottom strip so
                // users can see which features are on without opening F1.
                stats.hdrActive        = true; // inside the HDR branch
                stats.nisActive        = m_nisUpscaler && m_config && m_config->nisEnabled;
                stats.colorExpansion   = m_config && m_config->colorExpansion;
                m_overlay->Render(stats);

                // Composite the offscreen onto the HDR backbuffer. The
                // overlay only generates an offscreen SRV in HDR mode;
                // in SDR it paints the backbuffer directly, no composite
                // needed.
                if (m_overlay->IsUsingOffscreen()) {
                    m_renderer->CompositeUI(m_overlay->GetOffscreenSRV());
                }
            }

            if (showGc553ProStartupWaitingCaption && m_overlay) {
                m_overlay->DrawStatusMessage(
                    m_renderer->GetWindowWidth(), m_renderer->GetWindowHeight(),
                    L"overlay.waitingForSource", 0.72f);
                if (m_overlay->IsUsingOffscreen()) {
                    m_renderer->CompositeUI(m_overlay->GetOffscreenSRV());
                }
            }

            // Transient toast (separate D2D pass on top of everything else).
            // Fades over the last 500 ms so it doesn't pop out.
            if (m_overlay && !m_toastText.empty()) {
                const auto now = std::chrono::steady_clock::now();
                if (now < m_toastExpiry) {
                    const auto remaining = std::chrono::duration<float>(m_toastExpiry - now).count();
                    const float alpha = (remaining < 0.5f) ? (remaining / 0.5f) : 1.0f;
                    m_overlay->DrawToast(
                        m_renderer->GetWindowWidth(), m_renderer->GetWindowHeight(),
                        m_toastText.c_str(), alpha);
                    if (m_overlay->IsUsingOffscreen()) {
                        m_renderer->CompositeUI(m_overlay->GetOffscreenSRV());
                    }
                } else {
                    m_toastText.clear();
                }
            }

            // Screenshot dispatch fires before Present. With FLIP_DISCARD the
            // backbuffer becomes undefined after Present, so a post-Present
            // readback would copy garbage. Both render branches need this
            // site since either path may run on a given frame; the atomic
            // exchange guarantees a single fire per hotkey press.
            if (m_screenshotRequested.exchange(false)) {
                TakeScreenshot();
            }

            m_renderer->EndFrame();
            lastPresentTime = std::chrono::steady_clock::now();
        }

        if (!hdrActive) {
        // NIS upscaling requires the renderer to write through the post-input
        // intermediate (clean RGBA8 capture-resolution texture independent
        // of NV12/BGRA source format). When NIS is off, DrawCaptureFrame
        // writes straight to the backbuffer for minimum latency.
        const bool nisActive = m_nisUpscaler && m_config && m_config->nisEnabled;
        m_renderer->SetPostInputEnabled(nisActive);

        m_renderer->BeginFrame(waitForSwapChain);
        if (presentationState == PresentationState::Capture) {
            m_renderer->DrawCaptureFrame();
        }

        if (presentationState == PresentationState::Capture &&
            nisActive && m_renderer->GetCaptureOutputSRV()) {
            const uint32_t inW  = m_renderer->GetCaptureOutputWidth();
            const uint32_t inH  = m_renderer->GetCaptureOutputHeight();
            uint32_t outW = inW;
            uint32_t outH = inH;

            // Compute upscale target dimensions. NIS only supports 1.0x
            // to 2.0x scale ratio (its NVScalerUpdateConfig rejects
            // anything beyond), so clamp aggressively.
            switch (m_config->nisScaleMode) {
                case 0: // 1.5x
                    outW = (uint32_t)(inW * 1.5f);
                    outH = (uint32_t)(inH * 1.5f);
                    break;
                case 1: // 2x
                    outW = inW * 2;
                    outH = inH * 2;
                    break;
                case 2: // Match window, aspect-preserving, clamped to <= 2x
                default: {
                    // Find the largest scale factor that:
                    //   - fits in the window
                    //   - preserves source aspect ratio
                    //   - is at most 2.0 (NIS's upper limit)
                    // The composite pass will letterbox the result inside
                    // the window, but NIS itself produces aspect-correct
                    // output -- preventing the vertical squish that would
                    // appear if windowW/windowH were used naively.
                    const uint32_t winW = m_renderer->GetWindowWidth();
                    const uint32_t winH = m_renderer->GetWindowHeight();
                    if (winW == 0 || winH == 0 || inW == 0 || inH == 0) {
                        outW = inW * 2; outH = inH * 2;
                    } else {
                        float scaleW = (float)winW / (float)inW;
                        float scaleH = (float)winH / (float)inH;
                        float scale  = std::min(scaleW, scaleH);
                        if (scale < 1.0f) scale = 1.0f;
                        if (scale > 2.0f) scale = 2.0f;
                        outW = (uint32_t)(inW * scale);
                        outH = (uint32_t)(inH * scale);
                    }
                    break;
                }
            }

            if (m_nisUpscaler->Configure(m_renderer->GetContext(),
                                          inW, inH, outW, outH,
                                          m_config->nisSharpness))
            {
                m_nisUpscaler->Dispatch(m_renderer->GetContext(),
                                         m_renderer->GetCaptureOutputSRV());

                m_renderer->CompositeUpscaledTexture(
                    m_nisUpscaler->GetOutputSRV(),
                    m_nisUpscaler->GetOutputWidth(),
                    m_nisUpscaler->GetOutputHeight());
            }
        }
        
        // No-signal screen: when the capture card reports no HDMI input,
        // override its hardware "NO SIGNAL · elgato" placeholder with the
        // NitLink brand identity. Takes precedence over the HUD overlay: a
        // FPS/latency HUD with no signal is just zeros, not useful.
        //
        // showNoSignalNow comes from the debounce in ShouldShowNoSignal();
        // see comment in the HDR branch above. During the grace period
        // DrawNoSignal is skipped and the backbuffer keeps DrawCaptureFrame's
        // last-good-frame output.
        //
        // Null guard: see HDR branch above. Same rationale.
        const bool signalActive = m_frameBuffer && m_frameBuffer->IsSignalActive();
        if (presentationState == PresentationState::Transition && m_overlay) {
            if (m_gc553ProP010ReopenPresentation) {
                drawWaitingStatus();
            } else {
                m_overlay->DrawStatusMessage(
                    m_renderer->GetWindowWidth(), m_renderer->GetWindowHeight(),
                    L"overlay.switchingHdr");
            }
        } else if (presentationState == PresentationState::NoSignal && m_overlay) {
            m_overlay->DrawNoSignal(m_renderer->GetWindowWidth(),
                                    m_renderer->GetWindowHeight());
        } else if (presentationState == PresentationState::WaitingForCapture) {
            drawWaitingStatus();
        }
        // Draw overlay if enabled (only when a real signal is present).
        // PiP gate: same rationale as the HDR branch: the 280x156 HUD
        // dominates a 480x270 PiP window. Suppress while PiP is active.
        else if (m_showOverlay && !m_isPiP) {
            Overlay::Stats stats{};
            stats.captureLatencyMs = m_captureLatencyMs;
            stats.renderLatencyMs  = m_renderLatencyMs;
            stats.appIngestMs      = m_mfDeliveryLatencyMs;
            stats.gpuMs            = m_renderer->GetLastGpuMs();
            // Prefer the real game framerate (from FrameDiffer) over the
            // HDMI signal rate. They diverge for sub-60fps games: HDMI
            // duplicates frames at the signal level, so a 30fps game still
            // produces 60 captured frames/sec, and only the differ knows
            // the real number.
            //
            // BUT: when the scene is static (menu screens, paused game,
            // looking at a wall), the differ correctly reports 0 unique
            // frames, and displaying "0 fps" to the user is misleading
            // because the game IS still running. Fall back to the HDMI
            // signal rate in that case. Source frame rate pacing reports
            // presents per second instead, the rate an external
            // frame-generation tool sees.
            stats.fps = (pacing == kPacingUnique && m_currentPresentFps > 0)
                          ? m_currentPresentFps
                          : (m_frameDiffer && m_currentContentFps > 0)
                              ? m_currentContentFps
                              : m_currentFps;
            stats.captureWidth     = m_captureDevice->GetOutputFormat().width;
            stats.captureHeight    = m_captureDevice->GetOutputFormat().height;
            stats.deviceName       = m_captureDevice->GetDeviceName();
            stats.signalActive     = signalActive;
            // Pipeline feature flags shown as the HUD's bottom strip so
            // users can see which features are on without opening F1.
            stats.hdrActive        = false; // inside the SDR branch
            stats.nisActive        = m_nisUpscaler && m_config && m_config->nisEnabled;
            stats.colorExpansion   = m_config && m_config->colorExpansion;
            m_overlay->Render(stats);
        }

        if (showGc553ProStartupWaitingCaption && m_overlay) {
            m_overlay->DrawStatusMessage(
                m_renderer->GetWindowWidth(), m_renderer->GetWindowHeight(),
                L"overlay.waitingForSource", 0.72f);
        }

        // Transient toast: see matching block in the HDR branch above.
        // In SDR the D2D target is the backbuffer directly, so no
        // CompositeUI is needed.
        if (m_overlay && !m_toastText.empty()) {
            const auto now = std::chrono::steady_clock::now();
            if (now < m_toastExpiry) {
                const auto remaining = std::chrono::duration<float>(m_toastExpiry - now).count();
                const float alpha = (remaining < 0.5f) ? (remaining / 0.5f) : 1.0f;
                m_overlay->DrawToast(
                    m_renderer->GetWindowWidth(), m_renderer->GetWindowHeight(),
                    m_toastText.c_str(), alpha);
            } else {
                m_toastText.clear();
            }
        }

        // Screenshot dispatch: see HDR branch above for the FLIP_DISCARD
        // rationale.
        if (m_screenshotRequested.exchange(false)) {
            TakeScreenshot();
        }

        m_renderer->EndFrame();
        lastPresentTime = std::chrono::steady_clock::now();
        } // end if (!hdrActive)
        
        auto renderEnd = Clock::now();
        m_renderLatencyMs = std::chrono::duration<double, std::milli>(renderEnd - renderStart).count();
    }
}

void Application::Shutdown()
{
    m_running = false;
    if (m_window) m_window->SetVideoAvailable(false);
    m_4kxPoller.Stop();

    // Stop the HDR source poller FIRST. Its worker thread can be mid-call
    // to ReadElgatoHDRSource, which opens a DirectShow graph against the
    // same Elgato device. Joining it here ensures no orphaned graph
    // outlives the capture device teardown below.
    if (m_hdrPoller) {
        m_hdrPoller->Stop();
        m_hdrPoller.reset();
    }

    // Disconnect Discord RPC FIRST so its worker thread can join cleanly
    // before anything else is torn down. The destructor would do this too
    // but being explicit avoids any chance of the worker thread racing
    // against the cleanup path.
    if (m_discord) {
        m_discord->Disconnect();
        m_discord.reset();
    }

    if (m_audioRouter) {
        m_audioRouter->Shutdown();
        m_audioRouter.reset();
    }

    // Stop capture FIRST and explicitly close the device. The Elgato driver
    // can hold the device handle indefinitely if the process exits without
    // calling Shutdown() on the IMFMediaSource, which leads to "no signal"
    // on subsequent runs until USB is reconnected or the system reboots.
    if (m_captureDevice) {
        m_captureDevice->StopCapture();
        m_nonGcP010Fallback.Reset();
        m_captureDevice->Close();

        // Frame-buffer teardown contract (see frame_buffer.h): the capture
        // worker is the producer that calls m_frameBuffer->Write(). The buffer
        // must not be freed while that worker can still write. StopCapture()
        // above joined the worker, so IsCapturing() is now false and freeing
        // the buffer here is safe. Do it explicitly at this known-good point
        // rather than leaving it to member-destruction order, which would
        // otherwise be the only thing standing between a future reorder and a
        // use-after-free.
        assert(!m_captureDevice->IsCapturing());
        m_frameBuffer.reset();

        m_captureDevice.reset();
    }

    // Closing the window can destroy its handle before shutdown saves the configuration.
    if (m_config && m_window) {
        auto [w, h] = m_window->GetWindowedClientSize();
        if (w >= 200 && h >= 200) { // Don't save zero-sized state from a closing window
            m_config->windowWidth = w;
            m_config->windowHeight = h;
        }
        m_config->Save("nitlink.json");
    }

    // Tear down GPU resources before the window goes away
    if (m_webviewSettings) { m_webviewSettings->Shutdown(); m_webviewSettings.reset(); }
    if (m_overlay)       { m_overlay->Shutdown();       m_overlay.reset(); }
    if (m_nisUpscaler)   { m_nisUpscaler->Shutdown();   m_nisUpscaler.reset(); }
    if (m_frameDiffer)   { m_frameDiffer->Shutdown();   m_frameDiffer.reset(); }
    if (m_renderer) m_renderer.reset();
    if (m_window)   m_window.reset();
}

void Application::ToggleFullscreen()
{
    // Safety: refuse fullscreen toggle while PiP is active. PiP runs
    // the main window as WS_EX_LAYERED + WS_EX_TOPMOST with an alpha-keyed
    // layered surface; if SetFullscreen swaps styles out from under those
    // attributes, DWM ends up with a fullscreen-ish window that still
    // carries the layered alpha state, visible as a transparent /
    // ghosted main window. The user must drop PiP (Alt+O) first, then
    // toggle fullscreen. Logged once per attempt so a quick test in
    // DebugView shows why the hotkey did nothing.
    if (m_isPiP) {
        if (!m_pipFullscreenBlockedLogged) {
            AppLog(L"[NitLink/PiP] fullscreen toggle ignored while PiP is active");
            m_pipFullscreenBlockedLogged = true;
        }
        return;
    }
    m_pipFullscreenBlockedLogged = false;

    m_isFullscreen = !m_isFullscreen;
    m_window->SetFullscreen(m_isFullscreen);

    // Note: the WebView2 settings overlay (when visible) is a child of
    // main, so it follows fullscreen automatically. The resize handler
    // calls webviewSettings->Resize() on the new client dimensions.
}

void Application::TogglePiP()
{
    // PiP is allowed regardless of HDR state. The SetPiP exit-style
    // handling and the Alt+Enter guard keep the layered-window alpha
    // and the HDR10 PQ backbuffer interacting cleanly; the window-state
    // behaviour is sound. SDR-in-HDR PiP colour handling has room for
    // refinement but does not gate the toggle.

    m_isPiP = !m_isPiP;

    if (m_isPiP) {
        m_isFullscreen = false;
        m_window->SetFullscreen(false);
        // Hand the saved (or hotkey-updated) preferred top-left to Window
        // so it places PiP where the user last had it. -1 / -1 = default
        // bottom-right corner.
        m_window->SetPreferredPiPPosition(m_config->pipX, m_config->pipY);
        m_window->SetPiP(true, m_config->pipWidth, m_config->pipHeight, m_config->pipOpacity);
        // Sync back the actual position SetPiP landed on (in case the
        // preferred coord was clamped) so persistence reflects reality.
        int32_t actualX = 0, actualY = 0;
        if (m_window->GetPiPPosition(actualX, actualY)) {
            m_config->pipX = actualX;
            m_config->pipY = actualY;
        }
    } else {
        m_window->SetPiP(false, 0, 0, 1.0f);
    }
}

void Application::SetPiPOpacity(float opacity)
{
    if (!m_config || !std::isfinite(opacity)) return;
    opacity = std::clamp(opacity, 0.1f, 1.0f);
    if (m_isPiP && (!m_window || !m_window->SetPiPOpacity(opacity))) return;
    m_config->pipOpacity = opacity;
    // Only the changed value is sent during dragging or held-key repeats.
    if (m_settingsVisible && m_webviewSettings) {
        m_webviewSettings->PostMessage(L"{\"pipOpacity\":" + std::to_wstring(opacity) + L"}");
    }
}

void Application::ToggleOverlay()
{
    m_showOverlay = !m_showOverlay;
    if (m_config) {
        m_config->showOverlay = m_showOverlay;
        if (!m_config->Save("nitlink.json")) {
            AppLog(L"HUD visibility: could not save nitlink.json");
        }
    }
}

void Application::ShowToast(const std::wstring& text,
                              std::chrono::milliseconds duration)
{
    m_toastText   = text;
    m_toastExpiry = std::chrono::steady_clock::now() + duration;
}

void Application::ToggleSettings()
{
    m_settingsVisible = !m_settingsVisible;
    if (m_webviewSettings) {
        m_webviewSettings->Show(m_settingsVisible);
        // Push current state every time the menu opens so toggles reflect
        // the truth: important for things that can change outside the menu
        // (Alt+H for HDR, Ctrl+G for game cycle).
        if (m_settingsVisible) PushSettingsState();
    }
}

bool Application::DropPlaceholderFrame(const uint8_t* data, uint32_t size)
{
    if (!m_placeholderHold.load(std::memory_order_acquire)) return false;
    PlaceholderDetector::Fingerprint       held;
    PlaceholderDetector::CaptureFormatKind fmt;
    uint32_t w, h;
    {
        std::lock_guard<std::mutex> lk(m_placeholderFpMutex);
        held = m_placeholderFp;
        fmt  = m_placeholderFmt;
        w    = m_placeholderW;
        h    = m_placeholderH;
    }
    if (w == 0 || h == 0) return false;
    // 144 sampled bytes out of the driver's buffer instead of a 12 MB copy.
    const auto fp = PlaceholderDetector::Compute(data, size, w, h, fmt);
    if (!PlaceholderDetector::Matches(fp, held)) {
        // Release before copying so subsequent source frames can reach the
        // detector even while the render thread is processing this frame.
        m_placeholderHold.store(false, std::memory_order_release);
        return false;
    }
    m_placeholderDropped.fetch_add(1, std::memory_order_relaxed);
    // This sample is intentionally filtered before FrameBuffer::Write(). Wake
    // the source-paced render loop without making a readable frame available.
    if (m_frameBuffer) m_frameBuffer->NotifyFrameActivity();
    return true;
}

bool Application::RecentSourceActivity(std::chrono::steady_clock::time_point now) const
{
    using namespace std::chrono;
    const auto recent = (m_lastMotionTime > m_lastContentTime)
                            ? m_lastMotionTime
                            : m_lastContentTime;
    const auto elapsedMs = duration_cast<milliseconds>(now - recent).count();
    return elapsedMs < kMotionRecencyWindowMs;
}

bool Application::ShouldShowNoSignal()
{
    using namespace std::chrono;
    using Clock = steady_clock;

    // Reacquire grace: how long to keep showing the last good frame after
    // the capture pipeline stops delivering frames. 2.5s comfortably
    // covers PS5 boot logo, source switch, and SDR<->HDR handshakes (all
    // empirically <2s) without making true signal loss feel unresponsive.
    constexpr auto kReacquireGrace   = milliseconds(2500);
    // Startup grace: shorter so a user who launches the app with no
    // source connected isn't staring at a black window for 2.5s.
    constexpr auto kStartupGrace     = milliseconds(1500);
    // Placeholder-confirmed grace is device-policy-specific. GC553Pro has
    // device + format binding and a 15-frame confirmation streak, so a
    // confirmed card placeholder can switch immediately to NitLink No Signal.
    // Keep the legacy Elgato one-second post-confirmation grace.
    constexpr auto kElgatoPlaceholderGrace = milliseconds(1000);
    constexpr auto kGc553ProPlaceholderGrace = milliseconds(0);
    // Reacquire-log debounce: minimum elapsed-without-a-real-frame before
    // surfacing the "Signal: reacquiring" transition. The render path
    // already paints the last good capture texture during this window
    // without any change, so there is nothing user-visible to surface;
    // logging the transition before this threshold just produces noise
    // on legitimate transient gaps (renderer iterating faster than the
    // capture worker, brief MF reader stalls between frames, short
    // format-reconcile windows). Once elapsed crosses this threshold,
    // the transition logs once and the latch holds until elapsed drops
    // back below the threshold or grace expires.
    constexpr auto kReacquireDebounce = milliseconds(250);

    const auto now     = Clock::now();
    const bool placeholderConfirmed = m_placeholderDetector
        && m_placeholderDetector->IsCurrentlyPlaceholder();
    const bool gc553ProPlaceholder =
        m_placeholderDeviceFamily ==
        PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro;
    const bool useLegacyMotionRecency =
        m_placeholderDeviceFamily ==
        PlaceholderDetector::PlaceholderDeviceFamily::Elgato;

    // Once NitLink has committed to its own No Signal presentation, keep it
    // authoritative across capture-session rebuilds. Only the accepted-Real
    // branch clears this latch.
    if (m_noSignalPresentationLatched) return true;
    if (m_signalLostLogged) {
        m_noSignalPresentationLatched = true;
        return true;
    }

    // MOTION-RECENCY EARLY EXIT.
    //
    // Before consulting any of the grace timers, ask whether the source
    // has shown evidence of being alive in the recent past. If either
    // the frame differ reported non-duplicate motion or the placeholder
    // classifier let a Real frame through within kMotionRecencyWindowMs,
    // the source is healthy; short-circuit: clear the latches, do
    // not log, do not arm the no-signal page.
    //
    // This is the discriminator that handles fingerprint-match false
    // positives (Star Wars Jedi Lucasfilm card, similar static intro
    // cards). The PlaceholderDetector cannot distinguish those from a
    // real Elgato placeholder by fingerprint alone; both produce
    // pixel-identical 9-zone luma signatures. Temporal context can:
    // a real placeholder is preceded by silence, an intro card is
    // preceded by animation. See m_lastMotionTime / m_lastContentTime
    // in application.h for how the two halves are tracked.
    //
    // Guarded by m_hasEverReceivedFrame so the startup grace below can
    // still trip when the user launches NitLink with no source
    // connected. Without that guard, the timers initialized to "now()"
    // in Initialize would suppress the startup no-signal screen for
    // the full kMotionRecencyWindowMs.
    if (m_hasEverReceivedFrame &&
        (!placeholderConfirmed || useLegacyMotionRecency) &&
        RecentSourceActivity(now)) {
        if (m_signalLostLogged) {
            AppLog(L"Signal: restored (was lost)");
        } else if (m_signalReacquiringLogged) {
            AppLog(L"Signal: reacquired");
        }
        m_signalLostLogged        = false;
        m_signalReacquiringLogged = false;
        return false;
    }

    // Decision-of-record: elapsed time since the last frame the run loop
    // accepted as a real source frame (UpdateCaptureTexture called and
    // m_lastGoodFrameTime bumped at the same site).
    //
    // FrameBuffer::IsSignalActive() is deliberately not consulted here.
    // That predicate is content-based: it flips false after 30 consecutive
    // identical-hash frames in the producer (about 500 ms at 60 Hz
    // capture, because the same 256 sampled bytes hash equal on bit-
    // static content). Legitimate low-motion playback like intro logos,
    // slow fades, paused menus, and splash screens routinely toggles that
    // predicate false even while real frames keep flowing and being
    // classified as Real downstream. Using it as a fast path here would
    // surface a paired "reacquiring" / "reacquired" log every ~500 ms
    // hash cycle of static content.
    //
    // Signal loss is decided from two positive / negative signals
    // instead. Positive: the placeholder detector (format-tagged
    // fingerprints plus the temporal-stability gate) confirms the
    // Elgato NO SIGNAL output. Negative: m_lastGoodFrameTime fails to
    // advance for longer than the active grace window.
    //
    // `now` was already taken at the top of the function for the
    // motion-recency early exit; reuse it here so both checks see the
    // exact same instant.
    const auto elapsed = now - m_lastGoodFrameTime;
    // A confirmed card placeholder is stronger evidence than the lifetime
    // startup state, so apply placeholder policy before startup grace.
    const auto grace = placeholderConfirmed
        ? (gc553ProPlaceholder ? kGc553ProPlaceholderGrace
                               : kElgatoPlaceholderGrace)
        : (!m_hasEverReceivedFrame ? kStartupGrace : kReacquireGrace);

    // Active / recovering: elapsed below the reacquire debounce. The
    // capture pipeline has produced a Real frame within the last
    // kReacquireDebounce, so by every meaningful definition the signal
    // is healthy. Clear the latches; the transition log fires only when
    // a "reacquiring" or "lost" latch was held coming into this call.
    if (!placeholderConfirmed && elapsed < kReacquireDebounce) {
        if (m_signalLostLogged) {
            AppLog(L"Signal: restored (was lost)");
        } else if (m_signalReacquiringLogged) {
            AppLog(L"Signal: reacquired");
        }
        m_signalLostLogged        = false;
        m_signalReacquiringLogged = false;
        return false;
    }

    // Past the reacquire debounce but still within grace. Render side
    // keeps painting the last good capture texture; log the transition
    // once so DebugView shows the cause (startup window, placeholder
    // confirmed, or ordinary no-fresh-frame stall).
    if (elapsed < grace) {
        if (!m_signalReacquiringLogged && !m_signalLostLogged) {
            const wchar_t* reason = !m_hasEverReceivedFrame
                ? L"startup"
                : (placeholderConfirmed ? L"placeholder" : L"no-fresh-frame");
            const auto ms = duration_cast<milliseconds>(elapsed).count();
            std::wstringstream ss;
            ss << L"Signal: reacquiring (reason=" << reason
               << L", elapsed=" << ms
               << L"ms, grace=" << duration_cast<milliseconds>(grace).count()
               << L"ms, holding last good frame)";
            AppLog(ss.str());
            m_signalReacquiringLogged = true;
        }
        return false;
    }

    // Grace exhausted: commit to the no-signal page.
    if (!m_signalLostLogged) {
        const wchar_t* reason = !m_hasEverReceivedFrame
            ? L"startup"
            : (placeholderConfirmed ? L"placeholder" : L"no-fresh-frame");
        const auto ms = duration_cast<milliseconds>(elapsed).count();
        std::wstringstream ss;
        ss << L"Signal: lost (reason=" << reason
           << L", elapsed=" << ms
           << L"ms, exceeded " << duration_cast<milliseconds>(grace).count()
           << L"ms grace), showing no-signal screen";
        AppLog(ss.str());
        m_signalLostLogged        = true;
        m_signalReacquiringLogged = false;
        m_noSignalPresentationLatched = true;
    }
    return true;
}

bool Application::ReconcileCaptureFormat(bool force)
{
    // Same decision logic as Initialize: P010 when the user wants HDR AND
    // either the source was detected as HDR10 OR detection isn't available
    // (the user's preference is trusted in that case: typically the 4K S).
    // Keeping the formula in one place would be nicer, but pulling it into
    // a helper for two call sites (Initialize + here) isn't worth the extra
    // indirection yet.
    if (!m_captureDevice || !m_config) return false;

    // Snapshot presentation authority before any capture teardown. A visible
    // NitLink No Signal page remains authoritative across the reopen; only an
    // accepted Real frame may clear it.
    const bool noSignalPresentationLatched =
        m_noSignalPresentationLatched || m_signalLostLogged;
    m_noSignalPresentationLatched = noSignalPresentationLatched;
    if (noSignalPresentationLatched) {
        m_captureTransitionActive = false;
    }

    // Forced-reopen path: refresh the source's HDR state synchronously
    // before computing wantP010 below. The capture worker only flags
    // needs-reopen on a fatal stream condition (MF media-type change,
    // reader error, or repeated ReadSample failures); the most common
    // trigger is the source itself transitioning HDR<->SDR (PS5 HDR
    // setting flipped, game launched with a different HDR mode, etc).
    // The periodic HDRSourcePoller will eventually catch up via its
    // AcceptUpdate drain in the main loop, but on a polling cadence of
    // ~1s the drain often hasn't fired yet by the time the capture
    // worker dies and triggers this path. Using the stale cached
    // m_sourceIsHDR10 in that window makes wantP010 below resolve to
    // the OLD format, the device re-opens as P010 against a now-SDR
    // source, and the next frames upload as green garbage (SDR-shaped
    // bytes interpreted as P010 luma+chroma). A synchronous re-read of
    // the Elgato InfoFrame property here closes that window for the
    // 4K Pro path; the 4K S path has no detection property and falls
    // through to the user's hdrEnabled preference as before.
    // Gate Elgato-specific calls in this function on whether the
    // current device is an Elgato. For non-Elgato sources, the
    // IKsPropertySet HDR InfoFrame property is unavailable and the
    // 4K S HID protocol does not apply. m_hdrDetectionAvailable is
    // already false for non-Elgato devices (set by Initialize and
    // SwitchCaptureDevice), so the InfoFrame re-read below was
    // already effectively gated; this makes the gating explicit and
    // keeps the structure consistent with the other call sites.
    const bool isElgato = IsElgatoDevice(m_currentDeviceInfo.name);

    if (force && isElgato && m_hdrDetectionAvailable) {
        bool titleDirty = false;

        // Try the 4K Pro IKsPropertySet path first. Cheap (no HID
        // transactions, just a property GUID call). Returns
        // propertyAccessible=false on the 4K S, which falls through to
        // the vendor HID probe below.
        HDRSourceInfo srcInfo = ReadElgatoHDRSource(m_currentDeviceInfo.name);
        if (srcInfo.propertyAccessible) {
            if (srcInfo.isHDR10 != m_sourceIsHDR10) {
                AppLog(srcInfo.isHDR10
                    ? L"Reconcile force-reopen: source InfoFrame re-read says HDR10 (was SDR)"
                    : L"Reconcile force-reopen: source InfoFrame re-read says SDR (was HDR10)");
                m_sourceIsHDR10 = srcInfo.isHDR10;
                titleDirty = true;
            }
        } else {
            // 4K S path: re-probe via vendor HID. Costs 3 HID
            // transactions plus a 1.5s wall-clock wait on the MCU
            // re-parse. Per-toggle this is comfortably within the
            // firmware envelope. Rapid repeated force-reopens within
            // a single USB session can push the cumulative op count
            // past the fragility ceiling, but the 1.5s mid-probe wait
            // provides natural pacing between bursts. If MCU stalls
            // become reproducible from this path, gate by a
            // last-successful-probe timestamp throttle.
            const HdrMetadataProbeResult probe = Probe4KSHdrMetadata();
            if (probe.queryOk && probe.hdrActive != m_sourceIsHDR10) {
                AppLog(probe.hdrActive
                    ? L"Reconcile force-reopen: 4K S vendor HID probe says HDR ACTIVE (was SDR)"
                    : L"Reconcile force-reopen: 4K S vendor HID probe says SDR (was HDR ACTIVE)");
                m_sourceIsHDR10 = probe.hdrActive;
                titleDirty = true;
            } else if (probe.queryOk) {
                AppLog(L"Reconcile force-reopen: 4K S vendor HID probe confirms current HDR state (no change)");
            } else {
                AppLog(L"Reconcile force-reopen: 4K S vendor HID probe failed; keeping cached HDR state");
            }
        }

        // (Title update happens later, after the post-Open format check
        // settles the actual capture subtype. titleDirty above is purely
        // informational at this point: the source-state change is
        // already in m_sourceIsHDR10, and the title write reads from
        // the soon-to-be-negotiated capture format anyway.)
        (void)titleDirty;
    }

    // Keep the capture-format policy separate from the output-mode policy.
    // For GC553Pro, GetOutputFormat().subtype is the source of truth for the
    // stream that is actually running; RequestP010()/IsP010Requested() only
    // describe a future Open attempt and must not drive this decision.
    //
    // The output HDR preference does not select the capture subtype in
    // GC553Pro Auto; confirmed source EOTF does. Thus changing output policy
    // alone does not cause a Media Foundation reader restart.
    const bool userWantsHDR = m_config->hdrEnabled;
    const bool retainedFormatBelongsToCurrentDevice =
        m_captureDevice->HasPublishedFormatForDevice(m_currentDeviceInfo);
    const CaptureFormat negotiatedFormat = m_captureDevice->GetOutputFormat();
    const NegotiatedCaptureFormatKind scopedActualCaptureFormat =
        IsEqualGUID(negotiatedFormat.subtype, MFVideoFormat_P010)
            ? NegotiatedCaptureFormatKind::P010
            : (IsEqualGUID(negotiatedFormat.subtype, MFVideoFormat_NV12)
                   ? NegotiatedCaptureFormatKind::NV12
                   : NegotiatedCaptureFormatKind::Other);
    const NegotiatedCaptureFormatKind actualCaptureFormat =
        ScopedNegotiatedCaptureFormat(retainedFormatBelongsToCurrentDevice,
                                      scopedActualCaptureFormat);
    const CaptureFormatOverride configuredOverride =
        m_config->GetOverride(m_currentDeviceInfo.name);
    const CaptureFormatPreference formatPreference =
        FormatPreferenceForOverride(configuredOverride);

    bool wantP010;
    Gc553ProSourceCapturePolicy gc553ProPolicy{};
    if (m_isGC553Pro) {
        if (!m_config->hdrAutoFromSource ||
            formatPreference != CaptureFormatPreference::Auto)
            m_gc553ProP010ReopenToastShown = false;
        gc553ProPolicy = DecideGc553ProSourceOutputPolicy(
            actualCaptureFormat, userWantsHDR, formatPreference,
            m_config->hdrAutoFromSource, m_gc553ProSourceState);
        if (m_gc553ProAutoP010Rejected && !force &&
            m_config->hdrAutoFromSource &&
            m_gc553ProSourceState == Gc553ProSourceHdrState::Hdr10Pq &&
            formatPreference == CaptureFormatPreference::Auto &&
            actualCaptureFormat != NegotiatedCaptureFormatKind::P010)
            gc553ProPolicy.reopenCapture = false;
        wantP010 = gc553ProPolicy.desiredCaptureIsP010;
        if (gc553ProPolicy.hdrRejected) {
            m_config->hdrEnabled = false;
            const std::wstring warning = ManualFormatHDRWarning(
                configuredOverride.format);
            AppLog(L"Reconcile: " + warning);
            ShowToast(warning, std::chrono::milliseconds(8000));
        }
    } else if (m_is4KS) {
        // Existing Elgato 4K S policy is intentionally unchanged.
        wantP010 = isElgato && m_sourceIsHDR10 && userWantsHDR;
    } else {
        // Existing Elgato HDR source policy is intentionally unchanged.
        wantP010 = isElgato &&
                   (m_sourceIsHDR10 ||
                    (userWantsHDR && !m_hdrDetectionAvailable));
    }
    if (!m_isGC553Pro) {
        wantP010 = ApplyManualFormatPreferenceToNonGcPolicy(
            wantP010, formatPreference);
    }
    const bool actualIsP010 = actualCaptureFormat == NegotiatedCaptureFormatKind::P010;
    // This runs even when the capture policy takes the same-format path.
    // It also restores the existing subtype-driven renderer flag if the user
    // turns GC553Pro source-auto mode off while keeping the same P010 stream.
    if (m_isGC553Pro && !m_config->hdrAutoFromSource)
        m_gc553ProSourceFrameSync.Reset();
    if (m_isGC553Pro && m_renderer &&
        !m_gc553ProSourceFrameSync.pending) {
        m_renderer->SetSourceIsHDR10(RendererInputIsHdr10(
            true, m_config->hdrAutoFromSource, m_sourceIsHDR10, actualIsP010));
    }
    const bool hasNonFormatOverride =
        configuredOverride.format.empty() &&
        (configuredOverride.width > 0 ||
         configuredOverride.height > 0 ||
         configuredOverride.fps > 0 ||
         configuredOverride.fpsNumerator > 0);
    m_nonGcP010Fallback.ObservePolicy(
        wantP010, retainedFormatBelongsToCurrentDevice, force);
    const bool formatReopenNeeded = m_isGC553Pro
        ? gc553ProPolicy.reopenCapture
        : ShouldReopenNonGcCapture(
              wantP010, actualCaptureFormat,
              retainedFormatBelongsToCurrentDevice,
              formatPreference, hasNonFormatOverride,
              m_nonGcP010Fallback.Accepted());

    // Run reaches this decision every render iteration. A failed Open can
    // leave the requested format equal to wantP010 while capture is stopped;
    // that state still needs recovery after ConsumeNeedsReopen has cleared
    // the one-shot flag. Only a running stream may take the same-format no-op.
    //
    // Automatic retries use a steady-clock deadline armed below, 1000 ms from
    // the start of each attempt. Enumeration, source activation and format
    // negotiation are expensive main-thread operations; repeating them at
    // render frequency would monopolize the loop and flood the driver/log
    // while a disconnected card cannot open. One second spaces those calls
    // and gives device re-enumeration time to settle between automatic
    // attempts. The deadline survives failed Open and missing-device
    // returns, so an absent source takes this cheap early return
    // between attempts instead of spinning through teardown and enumeration.
    // A successful restart clears the deadline. Explicit forced reconciles
    // bypass it so a device switch or newly reported stream failure is handled
    // immediately rather than inheriting an unrelated retry delay.
    const bool captureStopped = !m_captureDevice->IsCapturing();
    const auto retryNow = std::chrono::steady_clock::now();
    if (captureStopped && !force && retryNow < m_nextCaptureRetry) return false;

    if (!formatReopenNeeded && !force && !captureStopped) {
        // The capture stream already has the format selected by the last
        // stable GC553Pro source EOTF. Output HDR remains independent of this
        // capture-side no-op.
        return false;
    }

    // This is past the no-op and retry gates: an actual Stop/Close/Open for
    // the first GC553Pro Auto NV12 -> P010 promotion is about to begin.
    // The toast changes only the UI hint; transition/WaitingForCapture still
    // protects presentation until an accepted Real frame arrives.
    m_gc553ProP010ReopenPresentation = IsGc553ProAutoNv12ToP010Reopen(
        m_isGC553Pro, m_config->hdrAutoFromSource, formatPreference,
        actualCaptureFormat, m_gc553ProSourceState, wantP010);
    if (formatReopenNeeded && !noSignalPresentationLatched &&
        !m_captureTransitionActive) {
        m_captureTransitionActive = true;
        AppLog(L"HDR transition: capture format change, showing transition UI");
    }

    m_nextCaptureRetry = retryNow + std::chrono::milliseconds(1000);

    if (force && !formatReopenNeeded) {
        AppLog(wantP010
            ? L"Reconcile: forced reopen, staying on P010 HDR10"
            : L"Reconcile: forced reopen, staying on SDR");
    } else if (!formatReopenNeeded && captureStopped) {
        AppLog(actualIsP010
            ? L"Reconcile: capture stopped, reopening the negotiated P010 stream"
            : L"Reconcile: capture stopped, reopening the negotiated SDR stream");
    } else {
        AppLog(wantP010
            ? L"Reconcile: capture format change, SDR -> P010 HDR10"
            : L"Reconcile: capture format change, P010 HDR10 -> SDR");
    }

    // Invalidate the previous capture session before teardown. The renderer
    // may retain GPU resources, but they are not presentable until a newly
    // accepted Real frame from the reopened stream arrives.
    m_captureFrameValidForSession = false;
    m_gc553ProPlaceholderTransitionGuardConsumed = false;
    m_gc553ProPreviousFreshFrameWasPlaceholder = false;
    m_gc553ProStartupBlackHint.Reset();
    if (m_renderer) m_renderer->InvalidateCaptureFrame();

    // Tear down. StopCapture joins the worker so once it returns no more
    // frames will land in m_frameBuffer; Close releases the source reader
    // and the IMFMediaSource. m_captureDevice itself is NOT destroyed:
    // its FrameCallback and ownership stays stable.
    m_captureDevice->StopCapture();
    m_nonGcP010Fallback.Reset();
    m_captureDevice->Close();

    // The frame buffer's capacity and stride are fixed at construction;
    // P010 needs 2x the bytes-per-row of NV12, so the buffer is rebuilt
    // after the new negotiated format is known. Release the old one
    // first so the capture worker can't possibly write into a buffer
    // about to be discarded. StopCapture above already joined the worker,
    // so IsCapturing() is false here, and the assert encodes that frame-buffer
    // teardown contract (see frame_buffer.h) so a future reorder that frees
    // the buffer before the join trips loudly in debug builds.
    assert(!m_captureDevice->IsCapturing());
    m_frameBuffer.reset();

    // Disable the Elgato hardware tonemap before re-Open. All HDR<->SDR
    // conversion happens in the shaders now, so raw HDR10 codes from
    // the card are required whenever the source is HDR10. Gated on the
    // current device being an Elgato: webcams and third-party capture
    // cards do not implement this property and the call would only
    // generate log noise. (4K S HID tonemap is sent further down,
    // after the wantP010 decision is known and right before Open;
    // sending unconditional OFF here would leave the card in HDR-
    // passthrough mode when reconciling to an SDR/NV12 capture, which
    // makes the SDR picture wash out.)
    if (isElgato) {
        SetElgatoTonemap(m_currentDeviceInfo.name, false);
    } else {
        AppLog(L"Reconcile: current device is not Elgato; skipping Elgato-specific HDR controls");
    }

    // Re-enumerate first to handle the edge case where the user replugged
    // the card mid-session (the device index can shift). Keep the selected
    // source and wait for it to return when enumeration cannot find it.
    DeviceInfo deviceToOpen = m_currentDeviceInfo;
    {
        auto devices = DeviceEnumerator::FindCaptureDevices();
        if (!devices.empty()) {
            // First try to keep the device that was open before the
            // reconcile (handles the replug index-shift case where the
            // physical device is back but at a different MF index).
            bool matched = false;
            for (const auto& d : devices) {
                if (d.name == m_currentDeviceInfo.name) {
                    deviceToOpen = d;
                    matched = true;
                    break;
                }
            }
            // A missing source must not silently open a different card
            // using the old card's format/HDR policy. Keep retrying the
            // selected source; switching cards goes through SwitchCaptureDevice.
            if (!matched) {
                AppLog(L"Reconcile: selected capture device unavailable; will retry");
                return false;
            }
        } else {
            AppLog(L"Reconcile: no capture devices available; will retry");
            return false;
        }
    }

    m_captureDevice->RequestP010(wantP010);

    // Pair the 4K S HID tonemap state with the upcoming capture format
    // (same rationale as Initialize). On Alt+H toggling from HDR to SDR
    // this re-sends ON so the card stops passing raw HDR10 through and
    // starts delivering clean SDR for the NV12 capture; on toggling
    // back to HDR it re-sends OFF. The HID protocol is 4K S-specific;
    // selecting another card must not change a connected 4K S.
    if (m_is4KS && Set4KSTonemap(/*enableTonemap=*/ !wantP010)) {
        AppLog(wantP010
            ? L"Reconcile: 4K S HID tonemap OFF sent for raw HDR/P010"
            : L"Reconcile: 4K S HID tonemap ON sent for SDR/NV12");
    }

    // F1 Source picker manual override (same lifecycle as Initialize:
    // applied before Open, fallback handled inside CaptureDevice). This
    // is the path the setCaptureFormatOverride message handler reaches
    // by calling ReconcileCaptureFormat(force=true). The user just
    // changed a dropdown, the config is fresh, the next Open picks up
    // the new values.
    {
        CaptureDevice::OverrideSpec ov;
        if (m_config) {
            const CaptureFormatOverride cv = m_config->GetOverride(deviceToOpen.name);
            ov.width  = cv.width;
            ov.height = cv.height;
            ov.fps    = cv.fps;
            ov.fpsNumerator = cv.fpsNumerator;
            ov.fpsDenominator = cv.fpsDenominator;
            ov.format = cv.format;
        }

        // 4K S HDR resolution clamp: same rule as Initialize. When
        // wantP010 is true on the 4K S, clamp >1080p down to 1080p so
        // P010 negotiation succeeds. When wantP010 is false (user just
        // toggled Alt+H to SDR or source went SDR), this branch is
        // skipped and the user's saved 4K override applies as normal.
        // The Reconcile cadence (every render-loop iteration) means
        // Alt+H toggles propagate here within one frame: SDR<->HDR
        // toggle becomes a capture-format-and-resolution swap on 4K S.
        if (m_is4KS && wantP010 && ov.height > 1080) {
            AppLog(L"Reconcile: 4K S HDR pipeline, clamping capture override "
                   L"from " + std::to_wstring(ov.width) + L"x" +
                   std::to_wstring(ov.height) + L" down to 1920x1080");
            ov.width  = 1920;
            ov.height = 1080;
        }

        // 4K X follow-source: when the user has not pinned an explicit override
        // (Auto == width/height 0), capture at the source's ACTUAL resolution and
        // fps from Detect4KXSourceMode instead of the card's native-best (4K@144).
        // The card otherwise upscales e.g. a 1080p source to 4K; matching the
        // source is cheaper and lets NIS do the upscale to the display.
        if (m_is4KX && m_source4KProMode.detected &&
            ov.width == 0 && ov.height == 0) {
            ov.width  = m_source4KProMode.width;
            ov.height = m_source4KProMode.height;
            SetIntegerFrameRateFields(
                ov.fps, ov.fpsNumerator, ov.fpsDenominator,
                m_source4KProMode.fps);
            AppLog(L"Reconcile: 4K X following source "
                   + std::to_wstring(ov.width) + L"x" + std::to_wstring(ov.height)
                   + L"@" + std::to_wstring(ov.fps));
        }
        // 4K X HDR fps clamp: the X publishes 4K P010 only at 30fps. When the HDR
        // pipeline wants P010 at 4K, drop fps to 30 so negotiation succeeds (the X
        // trades framerate for HDR, the way the 4K S trades resolution).
        if (m_is4KX && wantP010 && ov.height >= 2160 && (ov.fps == 0 || ov.fps > 30)) {
            AppLog(L"Reconcile: 4K X 4K HDR, clamping fps to 30 (4K P010 cap)");
            SetIntegerFrameRateFields(
                ov.fps, ov.fpsNumerator, ov.fpsDenominator, 30);
        }

        m_captureDevice->SetFormatOverride(ov);
    }

    bool opened = m_captureDevice->Open(deviceToOpen);
    if (!opened && wantP010) {
        // P010 negotiation failed at the MF level. Same fallback as
        // Initialize: retry without P010 to get the SDR pipeline back.
        // Roll the user's HDR config flag back so the UI reflects what
        // actually happened (and so the next reconcile doesn't immediately
        // try P010 again on the next iteration).
        AppLog(L"Reconcile: P010 Open failed, retrying with BGRA");
        m_captureDevice->RequestP010(false);
        // Same retry-fallback HID flip as Initialize: tonemap was
        // sent OFF for the P010 attempt, that failed, so the pipeline
        // is falling back to SDR capture; flip the card's tonemap
        // state to ON so the NV12 frames come out clean. 4K-S-gated
        // like the call above.
        if (m_is4KS && Set4KSTonemap(/*enableTonemap=*/ true)) {
            AppLog(L"Reconcile: 4K S HID tonemap ON sent for SDR retry");
        }
        opened = m_captureDevice->Open(deviceToOpen);
        if (opened) {
            AppLog(L"Reconcile: rolled back hdrEnabled, P010 unavailable for this source");
            m_config->hdrEnabled = false;
            if (m_isGC553Pro) {
                if (m_config->hdrAutoFromSource &&
                    m_gc553ProSourceState == Gc553ProSourceHdrState::Hdr10Pq &&
                    formatPreference == CaptureFormatPreference::Auto)
                    m_gc553ProAutoP010Rejected = true;
                const std::wstring warning = P010UnavailableWarning(
                    m_captureDevice->GetOutputFormat());
                AppLog(L"Reconcile: " + warning);
                ShowToast(warning, std::chrono::milliseconds(8000));
            }
        }
    }

    if (!opened) {
        // Both attempts failed. Capture device is closed; frames will
        // stop arriving and the no-signal screen will take over. Surface
        // the failure so the user at least sees something in the log.
        AppLog(L"Reconcile: CaptureDevice::Open FAILED after format swap, capture is stopped");
        m_currentDeviceInfo = deviceToOpen;
        return false;
    }

    // Successful open: rebuild the frame buffer at the new format, refresh
    // the renderer's row-order and range hints (driver flips between paths
    // can change either), and restart the capture worker.
    m_captureDevice->LogAvailableFormats();
    auto format = m_captureDevice->GetOutputFormat();
    if (m_isGC553Pro && wantP010 &&
        !IsEqualGUID(format.subtype, MFVideoFormat_P010) &&
        m_config->hdrAutoFromSource &&
        m_gc553ProSourceState == Gc553ProSourceHdrState::Hdr10Pq &&
        formatPreference == CaptureFormatPreference::Auto)
        m_gc553ProAutoP010Rejected = true;
    m_nonGcP010Fallback.CompleteOpen(
        !m_isGC553Pro && wantP010, IsEqualGUID(format.subtype, MFVideoFormat_P010));

    m_placeholderDeviceFamily = PlaceholderFamilyForDevice(deviceToOpen.name);
    m_placeholderCaptureMetadataReady =
        TryPlaceholderFormatForSubtype(format.subtype, m_placeholderCaptureFormat);
    if (!m_placeholderCaptureMetadataReady) {
        m_placeholderDeviceFamily =
            PlaceholderDetector::PlaceholderDeviceFamily::Unknown;
    }

    if (const P010SelectionNotice notice =
            m_captureDevice->ConsumeP010SelectionNotice();
        notice.available) {
        const std::wstring warning = P010SelectionWarning(notice);
        AppLog(L"Reconcile: " + warning);
        ShowToast(warning, std::chrono::milliseconds(8000));
    }

    // Same post-Open tonemap reconciliation as Initialize. If wantP010
    // was true but Open negotiated a non-P010 format (4K manual override
    // path inside Open's attempts loop), the 4K S tonemap is misaligned
    // (set OFF earlier for the P010 attempt, but capture is now NV12).
    // Flip tonemap to ON so NV12 frames carry clean SDR. See Initialize
    // for the long-form rationale.
    if (m_is4KS && wantP010) {
        const bool actualIsP010 = IsEqualGUID(format.subtype, MFVideoFormat_P010);
        if (!actualIsP010) {
            if (Set4KSTonemap(/*enableTonemap=*/ true)) {
                AppLog(L"Reconcile: wantP010=true but Open negotiated a non-HDR "
                       L"format; flipped 4K S tonemap ON for clean SDR NV12");
            }
        }
    }

    // Title bar may need updating: if the source HDR state changed (the
    // earlier force-reopen re-probe already set m_sourceIsHDR10) OR if
    // the negotiated capture format changed (the [HDR]/[SDR] suffix is
    // derived from format.subtype). Always call here; UpdateWindowTitle
    // is cheap (no HID ops) and idempotent (SetWindowTextW with the
    // same string is a no-op).
    UpdateWindowTitle();

    m_frameBuffer = std::make_unique<FrameBuffer>(format.width, format.height, format.stride);

    // Reset the frame differ's per-stream state. Its m_prevTex still holds
    // luma from the previous capture format's frames, and its smoothing ring
    // buffer holds pre-reconcile votes; without this clear, the first
    // post-reconcile diff is against stale data and a sticky "duplicate"
    // classification can persist across the swap, leaving contentFps wedged
    // at 0 even when fresh content is arriving from the new capture session.
    if (m_frameDiffer) m_frameDiffer->Reset();
    // The new stream may run at another source rate, so its cadence is
    // measured again from the first new frames.
    m_sourceCadence.Reset();

    // Reset the placeholder detector for the same reason: a streak of
    // matching frames in the prior format would otherwise carry over, and
    // the same Elgato placeholder encodes to different zone luma in NV12
    // vs P010 vs BGRA, so any in-flight match streak is invalid post-swap.
    if (m_placeholderDetector) m_placeholderDetector->Reset();
    m_placeholderHold.store(false, std::memory_order_release);
    m_inPlaceholderState = false;
    m_placeholderCandidateLogged = false;
    m_placeholderConfirmedLogged = false;

    if (m_renderer) {
        m_renderer->SetSourceRowOrder(format.topDown);
        m_lastMfFullRange   = format.fullRange;
        m_lastCaptureIsP010 = IsEqualGUID(format.subtype, MFVideoFormat_P010);
        UpdateCaptureColorInterpretation(deviceToOpen.name);
        // GC553Pro Auto uses verified source EOTF for the shader's HDR10 flag.
        // The other paths keep their existing subtype-driven behavior. See
        // Initialize() for the distinction between source and capture format.
        //
        // Critical: do NOT write to m_sourceIsHDR10 here. That variable is
        // consumed at the top of this function (and elsewhere) to decide
        // what format to negotiate; setting it to "true whenever capture
        // is P010" would create a feedback loop on the 4K S: every
        // reconcile would see "in P010, so source must be HDR10, so
        // stay in P010", and Alt+H off would no-op instead of switching
        // back to NV12.
        const bool captureIsP010 = IsEqualGUID(format.subtype, MFVideoFormat_P010);
        if (m_isGC553Pro && m_config->hdrAutoFromSource && !captureIsP010)
            m_gc553ProSourceFrameSync.Reset();
        m_renderer->SetSourceIsHDR10(RendererInputIsHdr10(
            m_isGC553Pro, m_config && m_config->hdrAutoFromSource,
            m_sourceIsHDR10, captureIsP010));

        // Same subtype-to-enum routing as Initialize. Critical to do this BEFORE
        // StartCapture so the new capture worker thread can never deliver a
        // frame to UpdateCaptureTexture with a stale m_sourceFormat from the
        // previous format's session.
        DX11Renderer::CaptureFormatKind rkind;
        if (IsEqualGUID(format.subtype, MFVideoFormat_P010)) {
            rkind = DX11Renderer::CaptureFormatKind::P010;
        } else if (IsEqualGUID(format.subtype, MFVideoFormat_NV12)) {
            rkind = DX11Renderer::CaptureFormatKind::NV12;
        } else {
            rkind = DX11Renderer::CaptureFormatKind::BGRA;
        }
        m_renderer->SetSourceFormat(rkind);

        // Post-negotiation HDR/swap-chain sync. CaptureDevice's internal
        // attempts loop can reject P010 and fall through to NV12/BGRA
        // with Open returning true (e.g. 4K S has no 4K-P010, so a 4K
        // override + HDR-on lands NV12 at 4K). The Open retry-fallback
        // earlier in this function only fires when the OUTER Open call
        // returns false; it does not catch this internal-fallback case.
        // Without this sync, the swap chain stays HDR10 (from Alt+H ON)
        // while the renderer feeds 8-bit SDR luma through it. Bring
        // both flags down to match capture reality.
        if (wantP010 && !captureIsP010 && m_config->hdrEnabled) {
            AppLog(L"Reconcile: capture is non-P010 despite wantP010; "
                   L"disabling renderer HDR mode and rolling back hdrEnabled");
            m_config->hdrEnabled = false;
            if (m_isGC553Pro) {
                const std::wstring warning = P010UnavailableWarning(format);
                AppLog(L"Reconcile: " + warning);
                ShowToast(warning, std::chrono::milliseconds(8000));
            }
            if (m_overlay) m_overlay->OnResizeBegin();
            m_renderer->SetHDREnabled(false);
            if (m_overlay) m_overlay->OnResizeEnd();
        }
    }

    // Publish device identity before StartCapture so first-frame placeholder
    // family/format decisions cannot observe the previous device.
    m_currentDeviceInfo = deviceToOpen;
    const bool started = m_captureDevice->StartCapture([this](const uint8_t* data, uint32_t size, int64_t timestamp,
                                          int64_t arrivalWallNs, uint64_t deviceTimestamp) {
        if (DropPlaceholderFrame(data, size)) return;
        if (m_frameBuffer) m_frameBuffer->Write(data, size, timestamp, arrivalWallNs, deviceTimestamp);
    });

    if (!started) {
        AppLog(L"Reconcile: capture start failed; will retry");
        return false;
    }
    if (m_gc553ProP010ReopenPresentation &&
        IsEqualGUID(format.subtype, MFVideoFormat_P010) &&
        !m_gc553ProP010ReopenToastShown) {
        ShowToast(Tr(L"toast.gc553proCaptureSwitch"));
        m_gc553ProP010ReopenToastShown = true;
    }
    m_nextCaptureRetry = {};
    ApplyPresentCap();
    AppLog(L"Reconcile: capture restarted in new format");
    return true;
}

// Present-rate cap policy for the low-latency tearing-allowed present.
//
// A variable-refresh display engages VRR only when presents arrive a few
// hertz under its maximum: a vsync present never engages it, and an
// uncapped tearing present overshoots the ceiling and tears. A fixed
// refresh display gains nothing from the cap and loses frames whenever the
// cap sits under the source rate (a 60 Hz panel with a 60 fps source capped
// at 57 Hz skips three frames a second). The automatic cap is therefore the
// monitor refresh minus 3, applied only when that stays at or above the
// source frame rate. present_cap_hz in nitlink.json: 0 automatic, negative
// off, 30 to 1000 a fixed rate. A VRR_CAP.txt marker next to the exe wins
// over all of this inside the renderer.
//
// The refresh rate is read from the current display mode of the monitor
// under the window. A window dragged to another monitor without a resize
// keeps the previous cap until the next resize or format change.
void Application::ApplyPresentCap()
{
    if (!m_renderer) return;

    // The cap exists to hold a tearing-allowed present a few Hz under the
    // panel maximum so a variable refresh display stays inside its VRR
    // window. Frame-generation friendly mode already paces Present from the
    // source cadence, which sits below the panel rate, so a cap on top of
    // that would drop captured frames without buying anything.
    if (m_config && m_config->presentPacing != kPacingRefresh) {
        const bool capOff = m_renderer->SetPresentCap(0.0);
        if (m_appliedPresentCapHz != 0.0) {
            m_appliedPresentCapHz = 0.0;
            AppLog(capOff
                ? L"PresentCap: off (present pacing follows the source)"
                : L"PresentCap: VRR_CAP.txt pins the renderer cap; source-paced policy not applied");
        }
        return;
    }

    const int cfg = m_config->presentCapHz;
    double capHz = 0.0;
    std::wstring policy;

    if (cfg >= 30) {
        capHz = static_cast<double>(cfg);
        policy = L"fixed " + std::to_wstring(cfg) + L" Hz (present_cap_hz)";
    } else if (cfg < 0) {
        policy = L"off (present_cap_hz)";
    } else {
        double refreshHz = 0.0;
        HMONITOR monitor = MonitorFromWindow(m_window->GetHWND(), MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (monitor && GetMonitorInfoW(monitor, &info)) {
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            // dmDisplayFrequency of 0 or 1 means the hardware default rate,
            // which carries no usable number.
            if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
                mode.dmDisplayFrequency > 1) {
                refreshHz = static_cast<double>(mode.dmDisplayFrequency);
            }
        }

        const uint32_t sourceFps = m_captureDevice ? m_captureDevice->GetOutputFormat().fps : 0;
        const double candidate = refreshHz - 3.0;
        std::wstringstream ss;
        if (refreshHz >= 50.0 && candidate >= static_cast<double>(sourceFps)) {
            capHz = candidate;
            ss << L"automatic " << static_cast<int>(candidate) << L" Hz (monitor "
               << static_cast<int>(refreshHz) << L" Hz, source " << sourceFps << L" fps)";
        } else if (refreshHz < 50.0) {
            ss << L"off (monitor refresh unknown or under 50 Hz)";
        } else {
            ss << L"off (monitor " << static_cast<int>(refreshHz) << L" Hz minus 3 would sit under the "
               << sourceFps << L" fps source)";
        }
        policy = ss.str();
    }

    const bool applied = m_renderer->SetPresentCap(capHz);
    if (capHz != m_appliedPresentCapHz) {
        m_appliedPresentCapHz = capHz;
        AppLog(applied ? L"PresentCap: " + policy
                       : L"PresentCap: VRR_CAP.txt pins the renderer cap; policy not applied: " + policy);
    }
}

// Preset ratios offered by Alt+A and the panel, in cycle order. Any other
// "W:H" string in nitlink.json is honored as a custom ratio but is not part
// of the cycle.
static const char* const kAspectPresets[] = { "auto", "4:3", "16:9", "16:10", "21:9", "stretch" };

// Parses the config string into the renderer's override value: 0 for
// auto, a negative value for stretch, otherwise width divided by height.
// Anything unparseable falls back to auto so a typo cannot blank the picture.
static float AspectOverrideFromString(const std::string& text)
{
    if (text == "auto")    return 0.0f;
    if (text == "stretch") return -1.0f;
    const size_t colon = text.find(':');
    if (colon == std::string::npos) return 0.0f;
    const float w = static_cast<float>(std::atof(text.substr(0, colon).c_str()));
    const float h = static_cast<float>(std::atof(text.substr(colon + 1).c_str()));
    if (w < 0.5f || h < 0.5f || w > 100.0f || h > 100.0f) return 0.0f;
    return w / h;
}

std::wstring Application::ApplyAspectRatio()
{
    std::string text = m_config ? m_config->aspectRatio : std::string("auto");
    const float ratio = AspectOverrideFromString(text);
    if (ratio == 0.0f && text != "auto") text = "auto";
    if (m_renderer) m_renderer->SetAspectOverride(ratio);
    std::wstring label(text.begin(), text.end());
    if (label == L"auto")    label = L"Auto";
    if (label == L"stretch") label = L"Stretch";
    return label;
}

// Steps present pacing refresh -> captured -> unique -> refresh. The run
// loop reads m_config->presentPacing every iteration, so the next iteration
// picks the new mode up with no pipeline rebuild. The present cap is
// re-evaluated because the paced modes drive Present from the source cadence
// and a cap on top of that would drop delivered frames. The consecutive-skip
// counter is zeroed so the periodic pacing diagnostic does not report a count
// left over from the previous mode.
void Application::CyclePresentPacing()
{
    if (!m_config) return;
    m_config->presentPacing = (m_config->presentPacing + 1) % 3;
    m_consecutiveSkips = 0;
    ApplyPresentCap();
    m_config->Save("nitlink.json");

    const std::wstring label =
        m_config->presentPacing == kPacingUnique   ? Tr(L"value.sourceFrameRate")
      : m_config->presentPacing == kPacingCaptured ? Tr(L"value.captureRate")
                                                   : Tr(L"value.displayRefresh");
    AppLog(L"Present pacing: " + label);
    ShowToast(Tr(L"toast.presentPacing") + L": " + label);
    if (m_settingsVisible) PushSettingsState();
}

void Application::CycleAspectRatio()
{
    if (!m_config) return;
    const int count = static_cast<int>(sizeof(kAspectPresets) / sizeof(kAspectPresets[0]));
    int next = 0;
    for (int i = 0; i < count; ++i) {
        if (m_config->aspectRatio == kAspectPresets[i]) { next = (i + 1) % count; break; }
    }
    m_config->aspectRatio = kAspectPresets[next];
    m_config->Save("nitlink.json");
    const std::wstring rawLabel = ApplyAspectRatio();
    const std::wstring label = rawLabel == L"Auto" ? Tr(L"value.auto")
        : rawLabel == L"Stretch" ? Tr(L"value.stretch") : rawLabel;
    AppLog(L"Aspect ratio: " + label);
    ShowToast(Tr(L"toast.aspectRatio") + L": " + label);
    if (m_settingsVisible) PushSettingsState();
}

bool Application::ApplyNoSignalSettings(bool forceReload)
{
    if (!m_overlay || !m_config) return false;

    const std::wstring imagePath = Utf8ToWide(m_config->noSignalImage);
    if (!m_config->noSignalImage.empty() && imagePath.empty()) {
        AppLog(L"No Signal image path is not valid UTF-8; using branded fallback");
        return false;
    }

    const bool loaded = m_overlay->SetNoSignalSettings(
        m_config->noSignalMode, imagePath, m_config->noSignalFit,
        m_config->noSignalDimImage, forceReload);
    if (!loaded && m_config->noSignalMode == "image") {
        AppLog(L"No Signal custom image is unavailable; using branded fallback");
    }
    return loaded;
}

bool Application::ChooseNoSignalImage()
{
    if (!m_config || !m_window) return false;

    ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) {
        AppLog(L"ChooseNoSignalImage: failed to create IFileOpenDialog");
        return false;
    }

    const std::wstring imageFilter = Tr(L"dialog.imagesFilter");
    const std::wstring allFilesFilter = Tr(L"dialog.allFilesFilter");
    const std::wstring dialogTitle = Tr(L"dialog.chooseNoSignalImage");
    const COMDLG_FILTERSPEC filters[] = {
        {imageFilter.c_str(), L"*.png;*.jpg;*.jpeg;*.bmp"},
        {allFilesFilter.c_str(), L"*.*"},
    };
    hr = dialog->SetFileTypes(ARRAYSIZE(filters), filters);
    if (FAILED(hr)) {
        AppLog(L"ChooseNoSignalImage: SetFileTypes failed");
        return false;
    }
    hr = dialog->SetFileTypeIndex(1);
    if (FAILED(hr)) {
        AppLog(L"ChooseNoSignalImage: SetFileTypeIndex failed");
        return false;
    }
    hr = dialog->SetTitle(dialogTitle.c_str());
    if (FAILED(hr)) {
        AppLog(L"ChooseNoSignalImage: SetTitle failed");
        return false;
    }
    FILEOPENDIALOGOPTIONS options{};
    hr = dialog->GetOptions(&options);
    if (FAILED(hr) || FAILED(dialog->SetOptions(
            options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST))) {
        AppLog(L"ChooseNoSignalImage: failed to configure dialog options");
        return false;
    }

    hr = dialog->Show(m_window->GetHWND());
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return false;
    if (FAILED(hr)) {
        AppLog(L"ChooseNoSignalImage: dialog failed");
        return false;
    }

    ComPtr<IShellItem> item;
    hr = dialog->GetResult(&item);
    if (FAILED(hr) || !item) {
        AppLog(L"ChooseNoSignalImage: GetResult failed");
        return false;
    }

    PWSTR rawPath = nullptr;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
    if (FAILED(hr) || !rawPath) {
        AppLog(L"ChooseNoSignalImage: GetDisplayName failed");
        return false;
    }
    const std::wstring path(rawPath);
    CoTaskMemFree(rawPath);

    const std::string utf8Path = WideToUtf8(path);
    if (utf8Path.empty()) {
        AppLog(L"ChooseNoSignalImage: selected path is not valid Unicode");
        return false;
    }

    AppLog(L"No Signal image selected: \"" + path + L"\"");
    m_config->noSignalImage = utf8Path;
    m_config->noSignalMode = "image";
    const bool loaded = ApplyNoSignalSettings(/*forceReload=*/true);
    if (!m_config->Save("nitlink.json")) {
        AppLog(L"No Signal image: could not save nitlink.json");
    }
    ShowToast(loaded ? Tr(L"toast.noSignalImageLoaded")
                     : Tr(L"toast.noSignalImageLoadFailed"),
              std::chrono::milliseconds(5000));
    PushSettingsState();
    return true;
}

void Application::CycleNoSignalMode()
{
    if (!m_config) return;
    m_config->noSignalMode =
        m_config->noSignalMode == "image" ? "default" : "image";
    ApplyNoSignalSettings();
    if (!m_config->Save("nitlink.json")) {
        AppLog(L"No Signal mode: could not save nitlink.json");
    }
    AppLog(L"No Signal mode: " + Utf8ToWide(m_config->noSignalMode));
    if (m_settingsVisible) PushSettingsState();
}

void Application::CycleNoSignalFit()
{
    if (!m_config) return;
    if (m_config->noSignalFit == "contain") {
        m_config->noSignalFit = "cover";
    } else if (m_config->noSignalFit == "cover") {
        m_config->noSignalFit = "stretch";
    } else {
        m_config->noSignalFit = "contain";
    }
    ApplyNoSignalSettings();
    if (!m_config->Save("nitlink.json")) {
        AppLog(L"No Signal fit: could not save nitlink.json");
    }
    AppLog(L"No Signal image fit: " + Utf8ToWide(m_config->noSignalFit));
    if (m_settingsVisible) PushSettingsState();
}

void Application::CycleNoSignalDimImage()
{
    if (!m_config) return;
    m_config->noSignalDimImage = !m_config->noSignalDimImage;
    ApplyNoSignalSettings();
    if (!m_config->Save("nitlink.json")) {
        AppLog(L"No Signal dim setting: could not save nitlink.json");
    }
    AppLog(m_config->noSignalDimImage
        ? L"No Signal image dimming: ON"
        : L"No Signal image dimming: OFF");
    if (m_settingsVisible) PushSettingsState();
}

std::wstring Application::ApplyPanelLayout()
{
    std::string side = m_config ? m_config->panelSide : std::string("right");
    WebViewSettings::Dock dock = WebViewSettings::Dock::Right;
    if (side == "left")      dock = WebViewSettings::Dock::Left;
    else if (side == "full") dock = WebViewSettings::Dock::Full;
    else                     side = "right";
    if (m_webviewSettings) {
        m_webviewSettings->SetDock(dock, m_config ? m_config->panelWidth : 420);
        m_webviewSettings->SetTransparentBackground(dock != WebViewSettings::Dock::Full);
    }
    if (side == "left") return L"Left";
    if (side == "full") return L"Full";
    return L"Right";
}

void Application::CyclePanelSide()
{
    if (!m_config) return;
    const std::string current = m_config->panelSide;
    m_config->panelSide = (current == "right") ? "left" : (current == "left") ? "full" : "right";
    m_config->Save("nitlink.json");
    const std::wstring label = ApplyPanelLayout();
    AppLog(L"Panel position: " + label);
    if (m_settingsVisible) PushSettingsState();
}

bool Application::RecoverFromDeviceLost()
{
    AppLog(L"Device lost: rebuilding renderer and device-dependent objects");

    // Capture the live renderer state before teardown. These are session
    // choices a fresh DX11Renderer would otherwise reset to defaults, plus the
    // backbuffer size to rebuild at. When a prior rebuild failed and left the
    // renderer null, the retry path enters here with no renderer to query, so
    // the size falls back to the live window client rect (which does not need
    // a renderer); without that fallback a failed rebuild could never retry.
    uint32_t w              = m_renderer ? m_renderer->GetWindowWidth()     : 0;
    uint32_t h              = m_renderer ? m_renderer->GetWindowHeight()    : 0;
    if ((w == 0 || h == 0) && m_window) {
        auto [cw, ch] = m_window->GetClientSize();
        w = cw;
        h = ch;
    }
    const bool wasVsync     = m_renderer ? m_renderer->IsVSyncOn()          : false;
    const bool wasHDR       = m_renderer ? m_renderer->IsHDREnabled()       : false;
    const bool wasDiag      = m_renderer ? m_renderer->IsHDRDiagModeOn()    : false;
    const bool wasPostInput = m_renderer ? m_renderer->IsPostInputEnabled() : false;

    // Tear down in reverse dependency order: overlay, NIS upscaler, and frame
    // differ all hold D3D11 objects created from the renderer's device, so they
    // must release before the device they were built on.
    m_captureFrameValidForSession = false;
    m_gc553ProPlaceholderTransitionGuardConsumed = false;
    m_gc553ProPreviousFreshFrameWasPlaceholder = false;
    m_gc553ProStartupBlackHint.Reset();
    m_frameDiffer.reset();
    m_nisUpscaler.reset();
    // Keep the Overlay object itself so its device-independent decoded No
    // Signal pixels survive the graphics-device rebuild. Shutdown releases all
    // old-device COM resources; Initialize below recreates only the D2D bitmap.
    if (m_overlay) m_overlay->Shutdown();
    m_renderer.reset();

    if (w == 0 || h == 0) {
        AppLog(L"Device lost: no cached backbuffer size; aborting rebuild");
        return false;
    }

    // Rebuild the renderer on a fresh device.
    m_renderer = std::make_unique<DX11Renderer>();
    if (!m_renderer->Initialize(m_window->GetHWND(), w, h)) {
        AppLog(L"Device lost: renderer rebuild FAILED");
        m_renderer.reset();
        return false;
    }

    // Rebuild the device-dependent objects on the new device. Best-effort, same
    // as Initialize: a failed dependent disables its feature but the app keeps
    // running. (Kept in lock-step with the Initialize bring-up at the overlay/
    // NIS/frame-differ block.)
    if (!m_overlay) m_overlay = std::make_unique<Overlay>();
    if (m_overlay->Initialize(m_renderer->GetDevice(), m_renderer->GetContext(),
                              m_renderer->GetSwapChain(), m_window->GetHWND())) {
        ApplyNoSignalSettings();
        m_showOverlay = m_config->showOverlay;
    } else {
        m_showOverlay = false;
        AppLog(L"Device lost: Overlay rebuild failed (continuing without overlay)");
    }

    m_nisUpscaler = std::make_unique<NisUpscaler>();
    if (!m_nisUpscaler->Initialize(m_renderer->GetDevice())) {
        m_nisUpscaler.reset();
        AppLog(L"Device lost: NIS rebuild failed (continuing without NIS)");
    }

    m_frameDiffer = std::make_unique<FrameDiffer>();
    if (!m_frameDiffer->Initialize(m_renderer->GetDevice())) {
        m_frameDiffer.reset();
        AppLog(L"Device lost: FrameDiffer rebuild failed (continuing without)");
    }

    // Re-apply the live renderer toggles the fresh device reset to defaults.
    m_renderer->SetVSync(wasVsync);
    m_renderer->SetHDRDiagMode(wasDiag);
    ApplyPresentCap();
    m_renderer->SetPostInputEnabled(wasPostInput);
    if (m_config) m_renderer->SetColorExpansion(m_config->colorExpansion);
    ApplyAspectRatio();

    // Re-teach the new renderer how to interpret the capture stream, reading
    // the format the capture device is currently running. Mirrors the renderer
    // source-state push in ReconcileCaptureFormat. The next UpdateCaptureTexture
    // re-creates the GPU capture textures for this format.
    if (m_captureDevice) {
        auto fmt = m_captureDevice->GetOutputFormat();
        m_renderer->SetSourceRowOrder(fmt.topDown);
        m_lastMfFullRange   = fmt.fullRange;
        m_lastCaptureIsP010 = IsEqualGUID(fmt.subtype, MFVideoFormat_P010);
        UpdateCaptureColorInterpretation(m_currentDeviceInfo.name);
        const bool captureIsP010 = IsEqualGUID(fmt.subtype, MFVideoFormat_P010);
        m_renderer->SetSourceIsHDR10(RendererInputIsHdr10(
            m_isGC553Pro, m_config && m_config->hdrAutoFromSource,
            m_sourceIsHDR10, captureIsP010));
        DX11Renderer::CaptureFormatKind rkind;
        if (captureIsP010) {
            rkind = DX11Renderer::CaptureFormatKind::P010;
        } else if (IsEqualGUID(fmt.subtype, MFVideoFormat_NV12)) {
            rkind = DX11Renderer::CaptureFormatKind::NV12;
        } else {
            rkind = DX11Renderer::CaptureFormatKind::BGRA;
        }
        m_renderer->SetSourceFormat(rkind);
    }

    // Restore HDR swap-chain mode last, bracketed by the overlay resize hooks
    // the same way every other HDR toggle is, so the overlay releases and
    // re-wraps the backbuffer bitmap cleanly. On failure the renderer stays in
    // SDR and config is brought down to match, same as the reconcile fallback.
    if (wasHDR) {
        if (m_overlay) m_overlay->OnResizeBegin();
        if (!m_renderer->SetHDREnabled(true)) {
            AppLog(L"Device lost: HDR re-enable failed; renderer staying in SDR");
            if (m_config) m_config->hdrEnabled = false;
        }
        if (m_overlay) m_overlay->OnResizeEnd();
    }

    AppLog(L"Device lost: rebuild complete");
    return true;
}

bool Application::SwitchCaptureDevice(const std::wstring& deviceName)
{
    if (!m_captureDevice || !m_config) return false;

    // Resolve the user-provided name against the live enumeration.
    auto devices = DeviceEnumerator::FindCaptureDevices();
    m_cachedCaptureDeviceNames.clear();
    m_cachedCaptureDeviceNames.reserve(devices.size());
    for (const auto& device : devices) {
        m_cachedCaptureDeviceNames.push_back(device.name);
    }
    if (devices.empty()) {
        AppLog(L"SwitchCaptureDevice: enumeration returned no devices");
        return false;
    }

    // Strict resolver: exact name match, then substring, then fail.
    // Unlike PickPreferredDevice's startup ladder, this does NOT fall
    // back to Elgato-bias or to devices[0] when the requested name is
    // not present. An explicit user switch must open the requested
    // device or none; silently swapping in a different device than the
    // user asked for would be a worse failure mode than returning false
    // and leaving the current device active.
    int idx = DeviceEnumerator::FindDeviceByName(devices, deviceName);
    if (idx < 0) {
        std::wstringstream ss;
        ss << L"SwitchCaptureDevice: no device matched '" << deviceName
           << L"' (enumeration size=" << devices.size()
           << L"); keeping current device";
        AppLog(ss.str());
        return false;
    }
    const DeviceInfo& newDevice = devices[idx];

    // Same-device case: skip the tear-down + reconcile, but still update
    // and persist the saved preference so an explicit user confirmation
    // sticks across the next launch.
    if (newDevice.name == m_currentDeviceInfo.name) {
        if (m_config->preferredDevice != deviceName) {
            m_config->preferredDevice = deviceName;
            m_config->Save("nitlink.json");
        }
        return true;
    }

    // Snapshot rollback state. Capture mutable members about to change
    // so a failed reconcile on the new device can restore the previous
    // working pipeline. Without this, an Open() failure on the new
    // device would leave the user staring at a black window.
    const DeviceInfo previousDevice = m_currentDeviceInfo;
    const bool previousSourceIsHDR10 = m_sourceIsHDR10;
    const auto previousGc553ProSourceState = m_gc553ProSourceState;
    const bool previousGc553ProAutoP010Rejected = m_gc553ProAutoP010Rejected;
    const bool previousHdrDetectionAvailable = m_hdrDetectionAvailable;
    const std::wstring previousPreferredDevice = m_config->preferredDevice;
    const bool previousIs4KS = m_is4KS, previousIs4KX = m_is4KX;
    const bool previousIsGC553Pro = m_isGC553Pro;
    const bool previousHdrEnabled = m_config->hdrEnabled;
    const auto previousSourceMode = m_source4KProMode;
    const auto previousHdmiSource = m_detectedHdmiSource;

    // No update from an old device may be applied to the new session.
    if (m_hdrPoller) { m_hdrPoller->Stop(); m_hdrPoller.reset(); }
    m_4kxPoller.Stop();
    const auto restartHdrPoller = [this] {
        if (m_isGC553Pro) {
            m_hdrPoller = std::make_unique<HDRSourcePoller>();
            m_hdrPoller->StartGc553Pro(m_currentDeviceInfo.name, m_gc553ProSourceState);
        } else if (m_hdrDetectionAvailable && !m_is4KS && !m_is4KX) {
            m_hdrPoller = std::make_unique<HDRSourcePoller>();
            m_hdrPoller->Start(m_currentDeviceInfo.name, m_sourceIsHDR10, m_hdrDetectionAvailable);
        }
        // The 4K X poller starts in Run after the new stream has signal.
    };

    {
        std::wstringstream ss;
        ss << L"SwitchCaptureDevice: '" << previousDevice.name
           << L"' -> '" << newDevice.name << L"'";
        AppLog(ss.str());
    }

    // Apply the new selection. ReconcileCaptureFormat keys off
    // m_currentDeviceInfo.name when deciding which enumerated device to
    // open, so updating the cache here is what redirects the reconcile
    // to the new device instead of re-opening the previous one.
    m_currentDeviceInfo = newDevice;
    m_config->preferredDevice = deviceName;
    std::wstring nameLower = newDevice.name;
    std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::towlower);
    m_is4KS = IsElgatoDevice(newDevice.name) && nameLower.find(L"4k s") != std::wstring::npos;
    m_is4KX = IsElgatoDevice(newDevice.name) && nameLower.find(L"4k x") != std::wstring::npos;
    m_isGC553Pro = GetCaptureDevicePolicy(newDevice.name).family ==
                   CaptureDeviceFamily::AverMediaGC553Pro;
    if (m_isGC553Pro) {
        AppLog(L"SwitchCaptureDevice: GC553Pro selected; probing source HDR via XU mailbox");
    }
    m_source4KProMode = {};
    m_detectedHdmiSource.clear();
    m_prevSourceNoSignal = true;
    m_4kxForceReconcile = false;

    // Refresh HDR detection state for the new device. The InfoFrame
    // read is an Elgato-specific property call; skip it for non-Elgato
    // devices and clear the detection flags so the reconcile's
    // wantP010 decision below falls through to the user's hdrEnabled
    // preference, same as the 4K S path does at startup.
    m_gc553ProSourceFrameSync.Reset();
    m_gc553ProP010ReopenToastShown = false;
    m_gc553ProP010ReopenPresentation = false;
    m_gc553ProSourceState = Gc553ProSourceHdrState::Unknown;
    m_gc553ProAutoP010Rejected = false;
    if (m_isGC553Pro) {
        Gc553ProHdrReader reader(newDevice.name);
        const auto probe = reader.Read();
        m_gc553ProSourceState = probe.state;
        m_hdrDetectionAvailable = probe.state != Gc553ProSourceHdrState::Unknown;
        m_sourceIsHDR10 = probe.state == Gc553ProSourceHdrState::Hdr10Pq;
    } else if (IsElgatoDevice(newDevice.name)) {
        HDRSourceInfo srcInfo = ReadElgatoHDRSource(newDevice.name);
        m_hdrDetectionAvailable = srcInfo.propertyAccessible;
        m_sourceIsHDR10 = srcInfo.propertyAccessible ? srcInfo.isHDR10 : false;
        if (m_is4KS) {
            const auto source = Detect4KSHdmiSource();
            if (source.detected) m_detectedHdmiSource = source.label;
            const auto probe = Probe4KSHdrMetadata();
            m_hdrDetectionAvailable = probe.queryOk;
            m_sourceIsHDR10 = probe.queryOk ? probe.hdrActive : m_config->hdrEnabled;
        }
    } else {
        m_hdrDetectionAvailable = false;
        m_sourceIsHDR10 = false;
        AppLog(L"SwitchCaptureDevice: selected device is not Elgato; skipping Elgato-specific HDR controls");
    }

    // Hand off to Reconcile for the actual transition work: tear down
    // the current capture, run the Elgato property + 4K S HID tonemap
    // calls for the new device, negotiate the format (with P010 -> NV12
    // fallback when needed), rebuild the framebuffer, reset the frame
    // differ and placeholder detector, sync the renderer's row order
    // and HDR10 flag, and restart the capture worker.
    if (!ReconcileCaptureFormat(/*force=*/ true)) {
        std::wstringstream ss;
        ss << L"SwitchCaptureDevice: reconcile failed on '" << newDevice.name
           << L"', rolling back to '" << previousDevice.name << L"'";
        AppLog(ss.str());

        // Restore the previous state and reconcile back onto it. If the
        // rollback reconcile also fails (the previous device was
        // unplugged between the snapshot and now, for example), the
        // existing no-signal UI takes over and the user sees a clear
        // failure rather than a half-open pipeline.
        m_currentDeviceInfo = previousDevice;
        m_sourceIsHDR10 = previousSourceIsHDR10;
        m_gc553ProSourceState = previousGc553ProSourceState;
        m_gc553ProAutoP010Rejected = previousGc553ProAutoP010Rejected;
        m_hdrDetectionAvailable = previousHdrDetectionAvailable;
        m_config->preferredDevice = previousPreferredDevice;
        m_config->hdrEnabled = previousHdrEnabled;
        m_is4KS = previousIs4KS;
        m_is4KX = previousIs4KX;
        m_isGC553Pro = previousIsGC553Pro;
        m_source4KProMode = previousSourceMode;
        m_detectedHdmiSource = previousHdmiSource;
        if (!ReconcileCaptureFormat(/*force=*/ true)) {
            AppLog(L"SwitchCaptureDevice: rollback reconcile also failed; capture pipeline is down");
        }
        restartHdrPoller();
        return false;
    }
    restartHdrPoller();
    if (IsElgatoDevice(newDevice.name) && !m_is4KS && !m_is4KX) {
        m_source4KProMode = Detect4KProSourceMode(newDevice.name);
        m_prevSourceNoSignal = !m_source4KProMode.detected;
    }
    UpdateWindowTitle();

    // Re-route audio to the new device. The audio router was bound to the
    // previous device at Initialize time; without this re-init, audio
    // either falls silent (endpoint gone with the old card) or keeps
    // streaming from the wrong card's endpoint.
    if (m_audioRouter) {
        const std::wstring newAudioHint = DeriveAudioHint(newDevice.name);
        m_audioRouter->Shutdown();
        if (!m_audioRouter->Initialize(newAudioHint)) {
            AppLog(L"SwitchCaptureDevice: AudioRouter has no endpoints yet (worker keeps retrying)");
        } else {
            AppLog(L"SwitchCaptureDevice: AudioRouter re-bound to '" + newAudioHint + L"'");
        }
    }

    // No 4K X XU access on the device-switch path: probing the XU here would
    // trigger the destructive scan that can wedge the card.

    // Persist the new preference so the next launch opens this device
    // by default through Initialize's PickPreferredDevice path.
    m_config->Save("nitlink.json");
    return true;
}

const wchar_t* FormatGuidToString(const GUID& g)
{
    if (g == MFVideoFormat_NV12)  return L"NV12";
    if (g == MFVideoFormat_P010)  return L"P010";
    if (g == MFVideoFormat_RGB32) return L"BGRA";
    return L"Unknown";
}

void Application::PushSettingsState(bool refreshCaptureDevices)
{
    if (!m_webviewSettings || !m_config) return;

    // Media Foundation enumeration can block the UI thread. Only explicit
    // Source-list requests pass true; F1 opens and ordinary state updates
    // reuse the startup/switch/refresh cache.
    RefreshCaptureDeviceNamesIfRequested(
        refreshCaptureDevices, m_cachedCaptureDeviceNames, [] {
            const auto devices = DeviceEnumerator::FindCaptureDevices();
            std::vector<std::wstring> names;
            names.reserve(devices.size());
            for (const auto& device : devices) {
                names.push_back(device.name);
            }
            return names;
        });

    // Build a JSON state blob and push to JS via WebView2 PostWebMessageAsJson.
    // Schema must match what nitlink-menu.html's applyState() expects.
    // Keep manual (no JSON lib) since it's small and structured.
    std::wstringstream js;
    js << L"{\"state\":{";
    const std::wstring languagePreference(m_config->language.begin(), m_config->language.end());
    js << L"\"languagePreference\":\"" << languagePreference << L"\",";
    js << L"\"locale\":\"" << Localization::Instance().LocaleName() << L"\",";
    js << L"\"hdrEnabled\":"        << (m_config->hdrEnabled        ? L"true" : L"false") << L",";
    js << L"\"hdrAutoDetectAvailable\":" << (m_hdrDetectionAvailable ? L"true" : L"false") << L",";
    js << L"\"colorExpansion\":"    << (m_config->colorExpansion    ? L"true" : L"false") << L",";
    js << L"\"nisEnabled\":"        << (m_config->nisEnabled        ? L"true" : L"false") << L",";
    js << L"\"presentPacing\":\""
       << (m_config->presentPacing == kPacingUnique   ? L"unique"
         : m_config->presentPacing == kPacingCaptured ? L"captured"
                                                      : L"refresh") << L"\",";
    js << L"\"lowLatency\":"        << (m_config->lowLatency        ? L"true" : L"false") << L",";
    js << L"\"preventSleep\":"      << (m_config->preventSleep      ? L"true" : L"false") << L",";
    js << L"\"audioMuted\":"        << (m_config->audioMuted        ? L"true" : L"false") << L",";
    js << L"\"volume\":"            << m_config->audioVolume        << L",";
    js << L"\"pipOpacity\":"        << m_config->pipOpacity         << L",";
    js << L"\"scalerName\":\"Catmull-Rom\",";
    js << L"\"aspectRatio\":\"" << JsonEscapeWide(ApplyAspectRatio()) << L"\",";
    js << L"\"panelSide\":\"" << ApplyPanelLayout() << L"\",";
    js << L"\"noSignalMode\":\""
       << JsonEscapeWide(Utf8ToWide(m_config->noSignalMode)) << L"\",";
    js << L"\"noSignalImage\":\""
       << JsonEscapeWide(Utf8ToWide(m_config->noSignalImage)) << L"\",";
    js << L"\"noSignalFit\":\""
       << JsonEscapeWide(Utf8ToWide(m_config->noSignalFit)) << L"\",";
    js << L"\"noSignalDimImage\":"
       << (m_config->noSignalDimImage ? L"true" : L"false") << L",";

    if (m_captureDevice) {
        auto fmt = m_captureDevice->GetOutputFormat();
        js << L"\"negotiatedWidth\":"  << fmt.width  << L",";
        js << L"\"negotiatedHeight\":" << fmt.height << L",";
        js << L"\"negotiatedFps\":"    << fmt.fps    << L",";
        js << L"\"negotiatedFormat\":\"" << FormatGuidToString(fmt.subtype) << L"\",";

        // Available formats: the full set the device's media type handler
        // exposed at Open time. Drives the F1 Source picker's cascade
        // dropdowns. Interlaced entries are filtered here rather than on
        // the JS side because no NitLink-target consumer source emits
        // interlaced and the user-facing dropdowns should not even hint
        // at it as a possibility.
        js << L"\"availableFormats\":[";
        const auto& formats = m_captureDevice->GetAvailableFormats();
        bool firstFormat = true;
        for (const auto& af : formats) {
            if (af.interlaced) continue;
            if (!firstFormat) js << L",";
            firstFormat = false;
            js << L"{\"width\":"   << af.width
               << L",\"height\":" << af.height
               << L",\"fps\":"    << af.fps
               << L",\"fpsNumerator\":" << af.fpsNumerator
               << L",\"fpsDenominator\":" << af.fpsDenominator
               << L",\"format\":\"" << FormatGuidToString(af.subtype) << L"\"}";
        }
        js << L"],";
    }

    // captureFormatOverride: the user's saved manual selection from the
    // F1 Source picker for the CURRENTLY ACTIVE device. Per-device
    // storage in m_config->captureFormatOverrides; the JS wire schema
    // stays single-object because the picker only ever shows one
    // device's choices at a time. Switching devices triggers another
    // PushSettingsState which emits the new device's saved override.
    //
    // Convention: each numeric field at 0 means Auto for that dimension;
    // empty format string means Auto for format. All-Auto = full automatic
    // negotiation (default). The JS side mirrors this interpretation;
    // no separate "isAuto" flag is needed.
    {
        const CaptureFormatOverride ov =
            m_config->GetOverride(m_currentDeviceInfo.name);
        js << L"\"captureFormatOverride\":{"
           << L"\"width\":"  << ov.width  << L","
           << L"\"height\":" << ov.height << L","
           << L"\"fps\":"    << ov.fps    << L","
           << L"\"fpsNumerator\":" << ov.fpsNumerator << L","
           << L"\"fpsDenominator\":" << ov.fpsDenominator << L","
           << L"\"format\":\"" << ov.format << L"\""
           << L"},";
    }

    // Transient user-facing notice produced by the capture pipeline
    // (currently only set when a manual format override could not be
    // honored and the pipeline fell back to Auto). Consumed here so the
    // next PushSettingsState pass does not re-show the same toast. Empty
    // string = no notice; JS treats it that way and renders nothing.
    {
        std::wstring notice = m_captureDevice
            ? m_captureDevice->ConsumeFallbackNotice()
            : L"";
        if (!notice.empty()) notice = Tr(L"toast.captureFormatUnavailable");
        js << L"\"notification\":\"" << JsonEscapeWide(notice) << L"\",";
    }

    // Most recent screenshot path, surfaced as a richer toast on the JS
    // side (filename + clickable open-folder link). One-shot: cleared
    // after emission so the same toast does not re-fire on subsequent
    // pushes.
    {
        js << L"\"screenshotSaved\":\"" << JsonEscapeWide(m_lastScreenshotPath) << L"\",";
        m_lastScreenshotPath.clear();
    }

    // Header meta line: real capture resolution, last measured fps,
    // and end-to-end latency. These come from the running pipeline.
    {
        uint32_t w = 0, h = 0;
        if (m_captureDevice) {
            auto fmt = m_captureDevice->GetOutputFormat();
            w = fmt.width;
            h = fmt.height;
        }
        std::wstringstream res;
        if (w && h) {
            res << w << L"×" << h << L" · " << m_currentContentFps << L" fps";
        } else {
            res << Tr(L"overlay.noSignal");
        }
        js << L"\"resolutionText\":\"" << res.str() << L"\",";

        // Link type. The Elgato capture cards NitLink targets are HDMI-
        // input devices, so the link is always HDMI by hardware contract.
        // The field exists as a string rather than a constant so a later
        // EDID-derived link-type readout can drop in without a schema
        // change on the JS side.
        js << L"\"linkText\":\"HDMI\",";

        const double e2e = m_captureLatencyMs + m_renderLatencyMs;
        std::wstringstream lat;
        lat << static_cast<int>(e2e + 0.5) << L" " << Tr(L"unit.ms");
        js << L"\"latencyText\":\"" << lat.str() << L"\",";
    }

    // ===== Game selector state =====
    //
    // currentGameId: the selected game's id, or "" if none. JS reads this
    // to highlight the current selection in the dropdown.
    //
    // gameList: array of { id, title } for every entry in the built-in
    // catalog. JS uses it to populate the search dropdown.
    {
        std::wstring wcur;
        wcur.reserve(m_config->currentGameId.size());
        for (char c : m_config->currentGameId) wcur.push_back(static_cast<wchar_t>(c));
        js << L"\"currentGameId\":\"" << wcur << L"\",";

        js << L"\"gameList\":[";
        const auto& games = GetGameDatabase();
        for (size_t i = 0; i < games.size(); ++i) {
            const auto& g = games[i];
            // ASCII-safe widening of id; titles may need escaping for the
            // few that contain apostrophes or quotes.
            std::wstring wid;
            wid.reserve(g.id.size());
            for (char c : g.id) wid.push_back(static_cast<wchar_t>(c));

            std::wstring wtitle;
            wtitle.reserve(g.title.size());
            for (char c : g.title) {
                // Escape \ and " (apostrophes don't need escaping in JSON).
                if (c == '\\') { wtitle.push_back(L'\\'); wtitle.push_back(L'\\'); }
                else if (c == '"') { wtitle.push_back(L'\\'); wtitle.push_back(L'"'); }
                else wtitle.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
            }

            js << L"{\"id\":\"" << wid << L"\",\"title\":\"" << wtitle << L"\"}";
            if (i + 1 < games.size()) js << L",";
        }
        js << L"]";
    }

    // ===== Capture-device picker state =====
    //
    // captureDevices: array of friendly-name strings produced by a
    // fresh Media Foundation enumeration. The order matches MF
    // enumeration order; the UI consumer can display it directly.
    // Each name is run through JsonEscapeWide so non-ASCII Unicode
    // and the rare quote/backslash do not break the JSON.
    //
    // activeDevice: friendly name of the device the capture pipeline
    // currently has open (mirrors m_currentDeviceInfo.name). The UI
    // uses this to mark which entry in captureDevices is currently
    // active. May differ from preferredDevice when the saved
    // preference was unplugged and Initialize fell through to a
    // different device via PickPreferredDevice.
    //
    // preferredDevice: the user's saved preference from Config. The
    // UI can show this distinctly from activeDevice when the two
    // differ ("you asked for X, currently showing Y because X is
    // not connected"). Empty when the user has never explicitly
    // picked a device.
    {
        js << L",\"captureDevices\":[";
        for (size_t i = 0; i < m_cachedCaptureDeviceNames.size(); ++i) {
            js << L"\"" << JsonEscapeWide(m_cachedCaptureDeviceNames[i]) << L"\"";
            if (i + 1 < m_cachedCaptureDeviceNames.size()) js << L",";
        }
        js << L"]";

        js << L",\"activeDevice\":\""
           << JsonEscapeWide(m_currentDeviceInfo.name) << L"\"";
        js << L",\"preferredDevice\":\""
           << JsonEscapeWide(m_config->preferredDevice) << L"\"";
    }

    js << L"}}";

    m_webviewSettings->PostMessage(js.str());
}

void Application::ApplyGameSettings(const std::string& gameId)
{
    if (!m_config || gameId.empty()) return;

    // If the user has saved settings for this game, load them into the live
    // config. Otherwise, seed a new entry with the *current* settings so the
    // user has a starting point to tweak from.
    auto it = m_config->gameSettings.find(gameId);
    if (it != m_config->gameSettings.end()) {
        const auto& gs = it->second;
        m_config->nisEnabled     = gs.nisEnabled;
        m_config->colorExpansion = gs.colorExpansion;

        // Notify the renderer about the pipeline change. Color expansion
        // feeds into the SDR shader so push it now; NIS is checked per
        // frame by the render loop and needs no explicit kick.
        if (m_renderer) {
            m_renderer->SetColorExpansion(m_config->colorExpansion);
        }

        std::wstring wid;
        for (char c : gameId) wid.push_back(static_cast<wchar_t>(c));
        AppLog(L"Applied saved settings for game: " + wid);
    } else {
        // First time selecting this game: record current state as its
        // starting profile. User can tweak and Save to commit.
        GameSettings gs;
        gs.nisEnabled     = m_config->nisEnabled;
        gs.colorExpansion = m_config->colorExpansion;
        m_config->gameSettings[gameId] = gs;
    }
}

void Application::UpdateDiscordForCurrentGame()
{
    if (!m_discord || !m_discord->IsConnected() || !m_config) return;

    if (m_config->currentGameId.empty()) {
        // No game: back to idle presence
        m_discord->SetActivity(
            L"In NitLink",
            L"PS5 Capture Viewer",
            std::chrono::system_clock::now(),
            "nitlink-logo",
            L"NitLink"
        );
        return;
    }

    // Look up the human-readable title from the catalog. If a currentGameId
    // is somehow not in the catalog (manual edit of config?), fall back to
    // the id itself as the display string.
    const auto* entry = FindGameById(m_config->currentGameId);
    std::wstring title;
    if (entry) {
        for (char c : entry->title) title.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    } else {
        for (char c : m_config->currentGameId) title.push_back(static_cast<wchar_t>(c));
    }

    // Use the game id as the Discord art asset key. The user must have
    // uploaded an asset with this exact name to their Discord Developer
    // Portal for the image to render. If no asset exists, Discord falls
    // back to no image: the text still shows fine.
    m_discord->SetActivity(
        title,                                      // "Spider-Man 2"
        L"Playing on PS5",                          // state line under title
        std::chrono::system_clock::now(),           // restart elapsed timer
        m_config->currentGameId,                    // large image asset key
        title                                       // tooltip when hovering image
    );
}

void Application::TakeScreenshot()
{
    AppLog(L"TakeScreenshot: ENTRY");
    OutputDebugStringW(L"[NitLink/App] TakeScreenshot ENTRY\n");

    // Save screenshots under the user's Pictures folder in a NitLink subdir
    PWSTR picturesPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &picturesPath))) {
        AppLog(L"Screenshot: failed to find Pictures folder");
        return;
    }

    std::wstring folder = std::wstring(picturesPath) + L"\\NitLink";
    CoTaskMemFree(picturesPath);

    CreateDirectoryW(folder.c_str(), nullptr);

    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_s(&tm_buf, &time_t);

    // File name prefix: "nitlink" plus the detected source name when one is
    // known (the identifier the title bar shows), so a folder of captures
    // reads as nitlink-PS5_<timestamp> instead of timestamps alone.
    std::wstring safeTitle = L"nitlink";
    {
        const std::wstring& source = !m_detectedHdmiSource.empty()
            ? m_detectedHdmiSource : m_source4KProMode.sourceName;
        std::wstring slug;
        for (wchar_t c : source) {
            const bool keep = (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') ||
                              (c >= L'a' && c <= L'z');
            if (keep) slug += c;
            else if (!slug.empty() && slug.back() != L'-') slug += L'-';
            if (slug.size() >= 32) break;
        }
        while (!slug.empty() && slug.back() == L'-') slug.pop_back();
        if (!slug.empty()) safeTitle += L"-" + slug;
    }

    wchar_t timestamp[64];
    wcsftime(timestamp, 64, L"%Y%m%d_%H%M%S", &tm_buf);

    std::wstring fullPath = folder + L"\\" + safeTitle + L"_" + timestamp + L".png";

    AppLog(L"Screenshot: calling SaveScreenshot for " + fullPath);
    bool ok = m_renderer->SaveScreenshot(fullPath);
    if (ok) {
        AppLog(L"Screenshot saved: " + fullPath);
        m_lastScreenshotPath = fullPath;
        PushSettingsState();

        // Surface a renderer-overlay toast so the user sees the save
        // confirmation during gameplay. The WebView toast fired by
        // PushSettingsState above only renders into a visible surface
        // when the settings menu is open; the overlay path draws on
        // top of the main capture window regardless of menu state.
        std::wstring filename = fullPath;
        size_t slash = filename.find_last_of(L"\\/");
        if (slash != std::wstring::npos) filename = filename.substr(slash + 1);
        ShowToast(Tr(L"toast.screenshotSaved") + filename);
    } else {
        AppLog(L"Screenshot failed");
    }
}

void Application::UpdateTaskbarIcon(const std::wstring& iconPath)
{
    m_window->SetIcon(iconPath);
}

// Compose the title bar string from current state.
//
//   No source detected, no HDR    -> "NitLink"
//   detection
//   Source detected, no HDR       -> "NitLink - PlayStation 5"
//   detection
//   Source detected, HDR          -> "NitLink - PlayStation 5 [HDR]"
//   detection, capture is P010    (real HDR pipeline)
//   Source detected, HDR          -> "NitLink - PlayStation 5 [SDR]"
//   detection, capture is NV12    (4K S 4K-override fallback: source
//                                  may be HDR but card delivers SDR)
//
// The "[HDR]" / "[SDR]" suffix reflects the ACTUAL capture format (the
// negotiated MF subtype), not the upstream source HDR state. Source can
// be HDR while capture is NV12 (4K S manual override at 3840x2160 forces
// NV12 because P010 is only published at 1080p/720p): in that case the
// user sees an SDR-tonemapped picture, so "[SDR]" is the honest label.
// Showing "[HDR]" when capture is actually NV12 would be misleading;
// the user pressing Alt+H expecting HDR would see the renderer mode
// flip but the picture stay SDR-shaped.
//
// Suffix is only added when m_hdrDetectionAvailable is true (i.e. the
// 4K Pro property GUID or the 4K S vendor HID probe returned a
// definite answer). Without detection, the suffix is omitted to avoid
// the misleading implication that "[SDR]" means "confirmed SDR" when
// it actually means "detection unavailable".
void Application::UpdateWindowTitle()
{
    if (!m_window) return;

    std::wstring title = L"NitLink";

    // Source identifier segment. Currently populated on the 4K S path
    // (Detect4KSHdmiSource decodes the HDMI Source Product Descriptor
    // InfoFrame). Empty on the 4K Pro until a source-identifier
    // mechanism is found on that device.
    if (!m_detectedHdmiSource.empty()) {
        title += L" - ";
        title += m_detectedHdmiSource;
    } else if (!m_source4KProMode.sourceName.empty()) {
        // 4K X: source product string decoded from the XU SPD InfoFrame
        // (Detect4KXSourceMode), e.g. "PS5". Same slot as the 4K S identifier.
        title += L" - ";
        title += m_source4KProMode.sourceName;
    }

    // Source resolution + fps segment. Currently populated on the 4K
    // Pro path (Detect4KProSourceMode reads the Elgato custom property
    // set's props 210 + 208 post-Open). Skipped on the 4K S because
    // the source identifier above already gives the user the device
    // context they need.
    if (m_source4KProMode.detected) {
        std::wstringstream ss;
        ss << L" - " << m_source4KProMode.width << L"x"
           << m_source4KProMode.height << L" @ "
           << m_source4KProMode.fps << L"Hz";
        title += ss.str();
    }

    // HDR/SDR suffix. Reflects the EFFECTIVE display mode (what the
    // user actually sees on screen): [HDR] when the source is sending
    // HDR signal AND the user has HDR rendering enabled. Both gates
    // matter:
    //   source HDR + hdrEnabled    -> renderer outputs HDR10 PQ          -> [HDR]
    //   source HDR + !hdrEnabled   -> shader tonemaps HDR to SDR          -> [SDR]
    //   source SDR + hdrEnabled    -> SDR data through HDR pipeline       -> [SDR]
    //   source SDR + !hdrEnabled   -> SDR everything                      -> [SDR]
    //
    // On the 4K S, hdrEnabled drives the capture format swap (P010 vs
    // NV12) via the wantP010 formula, so this also matches the actual
    // capture format. On the 4K Pro, capture stays P010 whenever
    // source is HDR (the shader handles tonemap when hdrEnabled is
    // false), so the title only flips when the user toggles Alt+H,
    // matching what they perceive.
    //
    // Suffix only appears when direct HDR detection is available, to
    // avoid the misleading implication that an absent suffix means SDR.
    if (m_hdrDetectionAvailable &&
        (!m_isGC553Pro ||
         m_gc553ProSourceState == Gc553ProSourceHdrState::Sdr ||
         m_gc553ProSourceState == Gc553ProSourceHdrState::Hdr10Pq)) {
        const bool effectiveHDR = m_sourceIsHDR10 &&
                                  m_config && m_config->hdrEnabled;
        title += L" [";
        title += effectiveHDR ? Tr(L"title.hdr") : Tr(L"title.sdr");
        title += L"]";
    }

    m_window->SetTitle(title);
}

} // namespace NitLink
