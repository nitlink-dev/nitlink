#include "capture_device.h"
#include "dshow_capture.h"
#include "p010_format_selector.h"
#include <mferror.h>
#include <debugapi.h>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <condition_variable>
#include <wrl/implements.h>

namespace NitLink {

// Asynchronous Media Foundation reader and capture-worker handoff.
// Synchronous ReadSample can remain blocked when a card stops delivering
// samples. CaptureDevice's stop flag cannot cancel that in-flight call, so a
// worker executing it cannot observe shutdown or finish its join. An async
// reader returns from ReadSample immediately and leaves CaptureLoop waiting
// on a condition variable that StopCapture can wake without another frame.
//
// Open creates one CaptureReadState shared by CaptureDevice and this callback.
// Media Foundation retains the callback through MF_SOURCE_READER_ASYNC_CALLBACK;
// a pending completion can keep it alive after Close releases the reader and
// after CaptureDevice is destroyed. The callback owns only the shared state,
// never a CaptureDevice pointer, so that longer lifetime cannot reach an
// expired frame callback, FrameBuffer, renderer or application object.
//
// CaptureLoop keeps one ReadSample request outstanding. On a Media Foundation
// thread, OnReadSample stores the HRESULT, flags, timestamp and a COM reference
// to the sample under the state mutex, marks the completion ready, then wakes
// the worker. CaptureLoop takes that sample and performs buffer access and
// frame delivery on the capture thread; neither runs on the MF callback thread.
// Once stopped is set, a late OnReadSample may only lock the retained state
// and latch readerFailed for a fatal flag. It must return without publishing
// a sample or touching CaptureDevice. OnFlush likewise touches only that
// state, clearing the flush-in-progress flag used to guard reader reuse.
// Reference: https://learn.microsoft.com/en-us/windows/win32/medfound/using-the-source-reader-in-asynchronous-mode
struct CaptureReadState {
    std::mutex mutex;
    std::condition_variable ready;
    bool stopped = true;
    bool completed = false;
    bool flushing = false;
    bool readerFailed = false;
    HRESULT status = S_OK;
    DWORD flags = 0;
    LONGLONG timestamp = 0;
    ComPtr<IMFSample> sample;
};

class CaptureReaderCallback final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFSourceReaderCallback> {
public:
    explicit CaptureReaderCallback(std::shared_ptr<CaptureReadState> state)
        : m_state(std::move(state)) {}

    HRESULT STDMETHODCALLTYPE OnReadSample(HRESULT hr, DWORD, DWORD flags,
                                           LONGLONG timestamp, IMFSample* sample) override {
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (flags & MF_SOURCE_READERF_ERROR) m_state->readerFailed = true;
            if (m_state->stopped) return S_OK;
            m_state->status = hr;
            m_state->flags = flags;
            m_state->timestamp = timestamp;
            m_state->sample = sample;
            m_state->completed = true;
        }
        m_state->ready.notify_one();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFlush(DWORD) override {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->flushing = false;
        return S_OK;
    }
private:
    std::shared_ptr<CaptureReadState> m_state;
};

// Debug helper -- writes to Visual Studio Output window
static void DebugLog(const std::wstring& msg) {
    OutputDebugStringW((L"[NitLink] " + msg + L"\n").c_str());
}

CaptureDevicePolicy GetCaptureDevicePolicy(const std::wstring& deviceName)
{
    std::wstring lower = deviceName;
    if (!lower.empty()) {
        CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
    }

    // The model identifier is preferred; the product-name alias covers
    // drivers that omit it without applying the policy to other AVerMedia
    // devices.
    if (lower.find(L"gc553pro") != std::wstring::npos ||
        (lower.find(L"avermedia") != std::wstring::npos &&
         lower.find(L"live gamer ultra s") != std::wstring::npos)) {
        return {CaptureDeviceFamily::AverMediaGC553Pro, true};
    }
    return {};
}

// Optional DirectShow capture backend: when USE_DSHOW.txt sits next to the
// exe, capture runs through the DirectShow one-hop backend instead of the
// Media Foundation source reader. Delete the marker to restore Media
// Foundation (no rebuild).
//
// The marker is resolved against the executable directory, not the current
// working directory: an installed or shortcut launch, or "Run as
// administrator" (working directory becomes System32), leaves the CWD
// unrelated to the exe, so a CWD-relative lookup would miss the marker the
// user placed next to the exe. The std::error_code overload of exists() is
// used so a transient access error on the path returns false instead of
// throwing out of this predicate.
static bool UseDShowBackend() {
    wchar_t exePath[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;

    std::filesystem::path marker(exePath);
    marker.replace_filename(L"USE_DSHOW.txt");

    std::error_code ec;
    return std::filesystem::exists(marker, ec);
}

// Integer fps from a media type's MF_MT_FRAME_RATE attribute.
// Returns 0 if the attribute is missing or has a zero denominator.
// Used by LogAvailableFormats to populate m_availableFormats AND by
// NegotiateFormat's post-acceptance guard to compare negotiated fps
// against the requested override. Both paths must produce identical
// values, or the cascade (driven by m_availableFormats) and the
// backstop (driven by readback) will disagree about which framerates
// are valid.
static UINT32 GetFpsFromMediaType(IMFMediaType* type) {
    UINT32 fpsNum = 0, fpsDen = 1;
    if (SUCCEEDED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE,
                                      &fpsNum, &fpsDen))
        && fpsDen > 0) {
        return fpsNum / fpsDen;
    }
    return 0;
}

CaptureDevice::CaptureDevice() = default;

CaptureDevice::~CaptureDevice()
{
    StopCapture();
    Close();
}

bool CaptureDevice::Open(const DeviceInfo& device)
{
    m_deviceName = device.name;
    m_needsReopen = false;
    m_p010SelectionNotice = {};

    if (UseDShowBackend()) {
        DebugLog(L"USE_DSHOW.txt present: using DirectShow capture backend");
        m_dshow = std::make_unique<DShowCapture>();
        if (m_dshow->Open(device)) {
            std::lock_guard<std::mutex> lock(m_formatMutex);
            m_publishedFormat = m_dshow->GetOutputFormat();
            return true;
        }
        // DirectShow could not open this device. Release the failed backend
        // and fall through to the Media Foundation path below rather than
        // failing the whole Open: a bad or unsupported marker must not leave
        // the app with no capture at all. m_dshow is null after the reset, so
        // no DirectShow state leaks into the Media Foundation attempt.
        DebugLog(L"DirectShow backend Open failed; falling back to Media Foundation");
        m_dshow.reset();
    }

    DebugLog(L"Opening device: " + device.name);

    ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 2);
    if (FAILED(hr)) return false;

    hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) return false;

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(attrs.Get(), &devices, &count);
    if (FAILED(hr) || count == 0) return false;

    bool found = false;
    for (UINT32 i = 0; i < count; i++) {
        if (i == device.index) {
            hr = devices[i]->ActivateObject(IID_PPV_ARGS(&m_source));
            found = SUCCEEDED(hr);
            break;
        }
    }

    for (UINT32 i = 0; i < count; i++) devices[i]->Release();
    CoTaskMemFree(devices);

    if (!found || !m_source) {
        DebugLog(L"Failed to activate device source");
        return false;
    }


    // Find the best native format the device offers (MF can convert if needed)
    if (!NegotiateFormat(m_source.Get())) {
        DebugLog(L"Failed to negotiate native format");
        return false;
    }

    // Enable video processing in the source reader. This inserts a Media
    // Foundation Transform (MFT) in the pipeline that automatically converts
    // whatever native format the card outputs (YUY2, NV12, UYVY, etc.) into
    // the format requested below.
    //
    // NOTE: MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING and ENABLE_ADVANCED_VIDEO_PROCESSING
    // are mutually exclusive. Advanced requires a D3D device manager too.
    // Only the basic flag is used here; it works on all Windows 10/11 systems.
    //
    // MF_LOW_LATENCY (additional attribute below):
    //   Microsoft-documented hint to the source reader, the underlying media
    //   source, and any in-line MFTs to prioritize minimum delay over quality
    //   (or smoothness). For a live console viewer, this is exactly the
    //   trade-off needed. Whether the Elgato media source actually
    //   honors the hint is implementation-defined; the attribute is set
    //   regardless, since there's no downside on capture sources (no
    //   compression trade-off applies to an uncompressed YUV/RGB capture
    //   pipeline).
    //   Reference: learn.microsoft.com/en-us/windows/win32/medfound/mf-low-latency
    ComPtr<IMFAttributes> readerAttrs;
    hr = MFCreateAttributes(&readerAttrs, 3);
    if (FAILED(hr)) {
        std::wstringstream ss;
        ss << L"MFCreateAttributes failed with HRESULT 0x" << std::hex << hr;
        DebugLog(ss.str());
        return false;
    }
    readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    readerAttrs->SetUINT32(MF_LOW_LATENCY, TRUE);

    m_readState = std::make_shared<CaptureReadState>();
    auto readerCallback = Microsoft::WRL::Make<CaptureReaderCallback>(m_readState);
    if (!readerCallback) return false;
    hr = readerAttrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, readerCallback.Get());
    if (FAILED(hr)) return false;

    hr = MFCreateSourceReaderFromMediaSource(m_source.Get(), readerAttrs.Get(), &m_reader);
    if (FAILED(hr)) {
        std::wstringstream ss;
        ss << L"MFCreateSourceReaderFromMediaSource failed with HRESULT 0x" << std::hex << hr;
        DebugLog(ss.str());
        return false;
    }

    // Try to get the desired output format. With video processing enabled, MF
    // can convert from whatever the card outputs natively. A small priority
    // list is tried because cards/drivers expose different format GUIDs.
    //
    // When the caller has requested P010 (HDR10 source detected and HDR mode
    // is on), P010 is tried FIRST. P010 is 10-bit BT.2020 PQ: the gold standard
    // for HDR10 passthrough. The Elgato 4K Pro publishes this format. If
    // P010 negotiation fails (e.g. video processor can't supply it for this
    // configuration), the code falls through to RGB32/etc.
    // and the caller is responsible for noticing and degrading to SDR pipeline.
    //
    // Manual override (F1 Source picker): if m_overrideSpec is non-default,
    // its dimensions replace the native-best m_format dimensions, and its
    // format string (if set) constrains the attempts list to just that
    // GUID. If override-driven negotiation fails, the code falls back to
    // native-best dimensions + standard attempts and sets m_fallbackNotice
    // so the JSON state push surfaces a toast.
    struct FormatAttempt {
        GUID         subtype;
        const wchar_t* name;
    };
    FormatAttempt attemptsP010[] = {
        { MFVideoFormat_P010,   L"P010 (10-bit BT.2020 PQ)" },
        // No fallback within HDR mode: if P010 fails, the caller decides
        // whether to retry without P010 or just accept SDR pipeline.
    };
    // SDR format preference order. NV12 (8-bit YUV 4:2:0) is preferred over
    // RGB32 (8-bit BGRA) because it's about 2.7x less bandwidth per frame
    // (12 bpp vs 32 bpp). On PCIe devices like the 4K Pro both formats deliver
    // identical resolution; on USB devices like the 4K S, RGB32 silently
    // downgrades to 1080p because USB 3.x can't sustain 4K@60 RGB32
    // (~24 Gbps), but it can deliver 4K@60 NV12 (~9 Gbps). The renderer's
    // NV12 shader path is already implemented and routinely used.
    FormatAttempt attemptsSDR[] = {
        { MFVideoFormat_NV12,   L"NV12" },
        { MFVideoFormat_RGB32,  L"RGB32" },
        { MFVideoFormat_ARGB32, L"ARGB32" },
    };

    // Snapshot the native-best dimensions before any override mutates
    // m_format. The fallback path restores from this snapshot when the
    // override turns out to be unachievable on the live source.
    const uint32_t nativeBestW   = m_format.width;
    const uint32_t nativeBestH   = m_format.height;
    const uint32_t nativeBestFps = m_format.fps;

    // Build the attempts list and apply override dimensions. Captured by
    // a lambda so the fallback path can reset and rebuild the list before
    // retrying.
    std::vector<FormatAttempt> attempts;
    auto rebuildStandardAttempts = [&]() {
        attempts.clear();
        if (m_requestP010) {
            for (auto& a : attemptsP010) attempts.push_back(a);
        } else {
            for (auto& a : attemptsSDR) attempts.push_back(a);
        }
    };

    const bool overrideRequested = !m_overrideSpec.isFullAuto();
    if (overrideRequested) {
        // Apply override dimensions. Zero / empty fields keep their
        // native-best values, so the user can override just format or
        // just resolution without specifying every axis.
        if (m_overrideSpec.width  > 0) m_format.width  = m_overrideSpec.width;
        if (m_overrideSpec.height > 0) m_format.height = m_overrideSpec.height;
        if (m_overrideSpec.fps    > 0) m_format.fps    = m_overrideSpec.fps;

        // If override.format is specified, the attempts list contains
        // only that GUID. Exact match or fall through to fallback. If not,
        // the user is overriding dimensions only; format then follows
        // the HDR mode flag the same as auto behavior.
        GUID overrideGuid = GUID_NULL;
        if      (m_overrideSpec.format == L"NV12") overrideGuid = MFVideoFormat_NV12;
        else if (m_overrideSpec.format == L"P010") overrideGuid = MFVideoFormat_P010;
        else if (m_overrideSpec.format == L"BGRA" ||
                 m_overrideSpec.format == L"RGB32") overrideGuid = MFVideoFormat_RGB32;

        if (!IsEqualGUID(overrideGuid, GUID_NULL)) {
            attempts.push_back({ overrideGuid, L"manual override format" });
            std::wstringstream ss;
            ss << L"Manual override: " << m_format.width << L"x"
               << m_format.height << L" @ " << m_format.fps
               << L"fps, format " << m_overrideSpec.format;
            DebugLog(ss.str());
        } else {
            // Override dimensions only, format=Auto. The user's intent here
            // is "give me THIS resolution, any working format", not "give
            // me ONLY P010 at this resolution and fail if unavailable."
            //
            // Build a hybrid attempts list: HDR-preferred format first
            // (when m_requestP010), then SDR fallbacks. This lets a user
            // on a 4K Pro pick 1080p with HDR enabled and still succeed,
            // because the 4K Pro publishes P010 only at 4K, so the override
            // would otherwise need to fall back to native-best 4K to
            // satisfy the HDR mode flag, which silently ignores the
            // user's dimension pick.
            //
            // No fallback notice in this branch: the dimension override
            // was honored (just with a different format than the HDR
            // flag would imply), which is the correct interpretation of
            // "Auto" for format.
            if (m_requestP010) {
                attempts.push_back({ MFVideoFormat_P010, L"P010 (HDR preferred)" });
            }
            attempts.push_back({ MFVideoFormat_NV12,   L"NV12 (auto)" });
            attempts.push_back({ MFVideoFormat_RGB32,  L"RGB32 (auto)" });
            attempts.push_back({ MFVideoFormat_ARGB32, L"ARGB32 (auto)" });
            std::wstringstream ss;
            ss << L"Manual override (dimensions only): " << m_format.width
               << L"x" << m_format.height << L" @ " << m_format.fps << L"fps";
            DebugLog(ss.str());
        }
    } else {
        rebuildStandardAttempts();
        if (m_requestP010) DebugLog(L"Requesting P010 (HDR10 path)");
    }

    // The attempts loop, factored into a lambda so it can run twice:
    // once with the override settings, then again with native-best +
    // standard attempts if the override path fails. The loop mutates
    // m_format on success (subtype + stride) and uses the current
    // m_format.width/height/fps for the request; both get rewritten
    // before each invocation of this lambda.
    auto tryAttempts = [&](bool enforceOverrideDims) -> bool {
        for (const auto& attempt : attempts) {
            ComPtr<IMFMediaType> outputType;
            if (FAILED(MFCreateMediaType(&outputType))) continue;
            outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            outputType->SetGUID(MF_MT_SUBTYPE, attempt.subtype);
            outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE,
                               m_format.width, m_format.height);
            MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

            // REQUEST the frame rate. Without this MF often picks the LOWEST
            // matching frame rate from the device's enum list. The 4K S, for
            // example, lists 4K@30 NV12 BEFORE 4K@60 NV12 in its 186-format
            // enumeration, and MF defaults to the first match. Result:
            // a request for "4K NV12" without specifying "at 60" yields 4K@30
            // silently. m_format.fps was populated by NegotiateFormat (or
            // overridden above) and reflects what the caller wants.
            if (m_format.fps > 0) {
                MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE,
                                    m_format.fps, 1);
            }

            HRESULT hrAttempt = m_reader->SetCurrentMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outputType.Get());
            if (SUCCEEDED(hrAttempt)) {
                // Post-negotiation dimension/fps guard: defends against
                // drivers that accept SetCurrentMediaType at the requested
                // values but silently substitute the source-native ones on
                // readback. Observed on Elgato 4K Pro: 1920x1080 RGB32
                // request returns S_OK, but GetCurrentMediaType immediately
                // reports 3840x2160. The fps half is the matching backstop
                // for the cascade's per-resolution fps filter (uniqueFps in
                // nitlink-menu.html): if the cascade is bypassed and a
                // non-existent (resolution, fps) combo is sent, the guard
                // rejects it the same way as a dimension lie.
                if (enforceOverrideDims) {
                    ComPtr<IMFMediaType> negotiatedType;
                    UINT32 negW = 0, negH = 0;
                    if (SUCCEEDED(m_reader->GetCurrentMediaType(
                            MF_SOURCE_READER_FIRST_VIDEO_STREAM, &negotiatedType)) &&
                        SUCCEEDED(MFGetAttributeSize(negotiatedType.Get(),
                            MF_MT_FRAME_SIZE, &negW, &negH))) {
                        const UINT32 negFps = GetFpsFromMediaType(negotiatedType.Get());
                        const bool dimMismatch = (negW != m_format.width ||
                                                  negH != m_format.height);
                        const bool fpsMismatch = (m_format.fps > 0 && negFps > 0 &&
                                                  negFps != m_format.fps);
                        if (dimMismatch || fpsMismatch) {
                            std::wstringstream ss;
                            ss << L"Output format " << attempt.name
                               << L" returned S_OK but driver substituted "
                               << negW << L"x" << negH << L"@" << negFps << L"fps"
                               << L" (override requested " << m_format.width << L"x"
                               << m_format.height << L"@" << m_format.fps << L"fps);"
                               << L" rejecting attempt";
                            DebugLog(ss.str());
                            continue;
                        }
                    }
                }
                DebugLog(std::wstring(L"Output format accepted: ") + attempt.name);
                m_format.subtype = attempt.subtype;
                if (attempt.subtype == MFVideoFormat_NV12) {
                    m_format.stride = m_format.width;
                } else if (attempt.subtype == MFVideoFormat_P010) {
                    // P010 layout: 16-bit Y plane (2 bytes per pixel) +
                    // half-res interleaved UV plane (each row half height,
                    // 2 bytes per chroma pair). Stride for upload is the
                    // Y-plane row stride.
                    m_format.stride = m_format.width * 2;
                } else {
                    m_format.stride = m_format.width * 4;
                }
                return true;
            }
            std::wstringstream ss;
            ss << L"Output format " << attempt.name
               << L" rejected (HRESULT 0x" << std::hex << hrAttempt << L")";
            DebugLog(ss.str());
        }
        return false;
    };

    bool gotFormat = tryAttempts(/*enforceOverrideDims=*/ overrideRequested);

    // Override fallback: if the user-specified combination did not
    // negotiate, restore native-best dimensions and retry with the
    // standard attempts list. m_overrideSpec is NOT cleared from
    // config; the persisted choice stays so that the next source
    // change retries it. The override is removed only by explicitly
    // picking "Auto".
    if (!gotFormat && overrideRequested) {
        DebugLog(L"Manual override unachievable; falling back to automatic negotiation");
        m_fallbackNotice = L"Capture format unavailable, reverted to automatic.";
        m_format.width  = nativeBestW;
        m_format.height = nativeBestH;
        m_format.fps    = nativeBestFps;
        rebuildStandardAttempts();
        gotFormat = tryAttempts(/*enforceOverrideDims=*/ false);
    }

    if (!gotFormat) {
        DebugLog(L"All output format attempts failed");
        return false;
    }

    // Verify what was actually negotiated
    ComPtr<IMFMediaType> actualType;
    if (SUCCEEDED(m_reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actualType))) {
        GUID subtype;
        actualType->GetGUID(MF_MT_SUBTYPE, &subtype);
        UINT32 w, h;
        MFGetAttributeSize(actualType.Get(), MF_MT_FRAME_SIZE, &w, &h);

        // CRITICAL: write back the ACTUAL negotiated dimensions to m_format.
        // The original request used whatever `m_format` had (e.g. 4K), but the
        // driver may have silently downgraded. The 4K S over USB does exactly
        // this: it accepts a 4K RGB32 request but actually delivers 1080p RGB32
        // because USB bandwidth can't sustain 4K @ 60Hz RGB. Without updating
        // m_format, the frame buffer gets sized for the request
        // (4K) but the capture worker writes 1080p frames into it. Result:
        // sampling random memory addresses as if they were pixels, which
        // produces the classic "green stretched garbage" you saw on 4K S.
        m_format.width  = w;
        m_format.height = h;

        // Also read back the actual frame rate. If MF silently downgraded
        // (e.g. driver can't sustain 4K@60 even though it advertised the
        // format), m_format.fps now reflects what's actually being delivered.
        const UINT32 negFps = GetFpsFromMediaType(actualType.Get());
        if (negFps > 0) m_format.fps = negFps;

        // Detect row order empirically. MF_MT_DEFAULT_STRIDE is a signed int32
        // where negative = bottom-up (legacy GDI convention, older drivers),
        // positive = top-down (modern convention). Some Elgato drivers behave
        // differently here: 4K Pro PCIe vs 4K S USB report different signs.
        // Without this query, the orientation would be guessed and one of the
        // two would render upside down. Planar formats (P010, NV12) are
        // always top-down so this only really matters for RGB32/ARGB32.
        // Default to top-down if the driver doesn't expose the attribute.
        INT32 mfStride = 0;
        if (SUCCEEDED(actualType->GetUINT32(MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&mfStride)))) {
            m_format.topDown = (mfStride >= 0);
            std::wstringstream sd;
            sd << L"  Row order: " << (m_format.topDown ? L"top-down" : L"bottom-up")
               << L" (MF_MT_DEFAULT_STRIDE=" << mfStride << L")";
            DebugLog(sd.str());
        } else {
            m_format.topDown = true; // safe default for modern drivers
            DebugLog(L"  Row order: top-down (MF_MT_DEFAULT_STRIDE not exposed by driver, assuming top-down)");
        }

        std::wstringstream ss;
        ss << L"Negotiated output: " << w << L"x" << h << L" @ " << m_format.fps << L"fps, format ";
        if (subtype == MFVideoFormat_RGB32)      ss << L"RGB32/BGRA";
        else if (subtype == MFVideoFormat_NV12)  ss << L"NV12";
        else if (subtype == MFVideoFormat_YUY2)  ss << L"YUY2";
        else if (subtype == MFVideoFormat_P010)  ss << L"P010 (10-bit HDR10)";
        else                                     ss << L"unknown";
        DebugLog(ss.str());

        if (!(m_requestP010 && IsEqualGUID(subtype, MFVideoFormat_P010))) {
            m_p010SelectionNotice = {};
        }

        // === HDR DIAGNOSTIC ===
        // Query every color-related attribute MF exposes for full visibility
        // into what the capture driver reports about the signal. Some
        // capture drivers don't set these (they just pass pixels through),
        // but Elgato's Media Foundation driver often does signal HDR via
        // MF_MT_VIDEO_PRIMARIES, MF_MT_TRANSFER_FUNCTION, and the HDR
        // metadata attributes. Reading these reveals whether the bytes
        // arriving are PQ-encoded HDR10, HLG, or just sRGB SDR.
        UINT32 transferFn = 0;
        if (SUCCEEDED(actualType->GetUINT32(MF_MT_TRANSFER_FUNCTION, &transferFn))) {
            const wchar_t* name = L"unknown";
            switch (transferFn) {
                case MFVideoTransFunc_10:        name = L"Linear (1.0)"; break;
                case MFVideoTransFunc_18:        name = L"Gamma 1.8"; break;
                case MFVideoTransFunc_20:        name = L"Gamma 2.0"; break;
                case MFVideoTransFunc_22:        name = L"Gamma 2.2"; break;
                case MFVideoTransFunc_709:       name = L"BT.709"; break;
                case MFVideoTransFunc_240M:      name = L"SMPTE 240M"; break;
                case MFVideoTransFunc_sRGB:      name = L"sRGB"; break;
                case MFVideoTransFunc_28:        name = L"Gamma 2.8"; break;
                case MFVideoTransFunc_Log_100:   name = L"Log 100"; break;
                case MFVideoTransFunc_Log_316:   name = L"Log 316"; break;
                case MFVideoTransFunc_2020_const:name = L"BT.2020 const"; break;
                case MFVideoTransFunc_2020:      name = L"BT.2020"; break;
                case MFVideoTransFunc_26:        name = L"Gamma 2.6"; break;
                case MFVideoTransFunc_2084:      name = L"SMPTE ST.2084 / PQ (HDR10!)"; break;
                case MFVideoTransFunc_HLG:       name = L"HLG (Hybrid Log-Gamma)"; break;
            }
            std::wstringstream s;
            s << L"  Transfer function: " << transferFn << L" (" << name << L")";
            DebugLog(s.str());
        } else {
            DebugLog(L"  Transfer function: NOT SET by driver");
        }

        UINT32 primaries = 0;
        if (SUCCEEDED(actualType->GetUINT32(MF_MT_VIDEO_PRIMARIES, &primaries))) {
            const wchar_t* name = L"unknown";
            switch (primaries) {
                case MFVideoPrimaries_BT709:     name = L"BT.709 (HDTV/sRGB)"; break;
                case MFVideoPrimaries_BT470_2_SysM: name = L"BT.470 SysM"; break;
                case MFVideoPrimaries_BT470_2_SysBG: name = L"BT.470 SysBG"; break;
                case MFVideoPrimaries_SMPTE170M: name = L"SMPTE 170M"; break;
                case MFVideoPrimaries_SMPTE240M: name = L"SMPTE 240M"; break;
                case MFVideoPrimaries_EBU3213:   name = L"EBU 3213"; break;
                case MFVideoPrimaries_SMPTE_C:   name = L"SMPTE C"; break;
                case MFVideoPrimaries_BT2020:    name = L"BT.2020 (HDR wide gamut!)"; break;
                case MFVideoPrimaries_XYZ:       name = L"XYZ"; break;
            }
            std::wstringstream s;
            s << L"  Video primaries: " << primaries << L" (" << name << L")";
            DebugLog(s.str());
        } else {
            DebugLog(L"  Video primaries: NOT SET by driver");
        }

        UINT32 colorSpace = 0;
        if (SUCCEEDED(actualType->GetUINT32(MF_MT_YUV_MATRIX, &colorSpace))) {
            std::wstringstream s; s << L"  YUV matrix: " << colorSpace;
            DebugLog(s.str());
        }
        UINT32 nominalRange = 0;
        if (SUCCEEDED(actualType->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &nominalRange))) {
            const wchar_t* name = L"unknown";
            switch (nominalRange) {
                case MFNominalRange_0_255:  name = L"0-255 (full)"; break;
                case MFNominalRange_16_235: name = L"16-235 (limited/TV)"; break;
                case MFNominalRange_48_208: name = L"48-208"; break;
                case MFNominalRange_64_127: name = L"64-127"; break;
            }
            std::wstringstream s;
            s << L"  Nominal range: " << nominalRange << L" (" << name << L")";
            DebugLog(s.str());
            // Tell the shader whether to do the limited-to-full expansion.
            // 4K Pro typically reports 16-235 -> fullRange=false -> expand.
            // 4K S NV12 reports 0-255 -> fullRange=true -> skip the expand
            // step (the driver already did it). Without this the 4K S
            // would double-expand, crushing blacks.
            m_format.fullRange = (nominalRange == MFNominalRange_0_255);
        } else {
            DebugLog(L"  Nominal range: NOT SET, assuming limited (16-235)");
            m_format.fullRange = false; // safest default: assume HDMI limited
        }

        // Try reading HDR mastering metadata if present. The driver may
        // attach MaxCLL / MaxFALL and the SMPTE 2086 display metadata when
        // the upstream signal carries an HDR InfoFrame.
        UINT32 maxCLL = 0;
        if (SUCCEEDED(actualType->GetUINT32(MF_MT_MAX_LUMINANCE_LEVEL, &maxCLL))) {
            std::wstringstream s; s << L"  MaxCLL (peak content light): " << maxCLL << L" nits";
            DebugLog(s.str());
        }
        UINT32 maxFALL = 0;
        if (SUCCEEDED(actualType->GetUINT32(MF_MT_MAX_FRAME_AVERAGE_LUMINANCE_LEVEL, &maxFALL))) {
            std::wstringstream s; s << L"  MaxFALL (avg frame brightness): " << maxFALL << L" nits";
            DebugLog(s.str());
        }
        // === END HDR DIAGNOSTIC ===
    }

    // Every format field is now finalized. Commit the negotiated working state
    // (m_format) into the mutex-guarded published copy so GetOutputFormat()
    // hands every other thread a complete, consistent struct. This is the
    // single synchronization point for the published format.
    PublishFormat();

    return true;
}

void CaptureDevice::PublishFormat()
{
    std::lock_guard<std::mutex> lock(m_formatMutex);
    m_publishedFormat = m_format;
}

bool CaptureDevice::NegotiateFormat(IMFMediaSource* source)
{
    ComPtr<IMFPresentationDescriptor> pd;
    HRESULT hr = source->CreatePresentationDescriptor(&pd);
    if (FAILED(hr)) return false;

    DWORD streamCount = 0;
    hr = pd->GetStreamDescriptorCount(&streamCount);
    if (FAILED(hr)) return false;

    const CaptureDevicePolicy policy = GetCaptureDevicePolicy(m_deviceName);
    const bool gamingP010Auto = m_requestP010 &&
                                m_overrideSpec.isFullAuto() &&
                                policy.preferHighFpsP010;

    for (DWORD i = 0; i < streamCount; i++) {
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        hr = pd->GetStreamDescriptorByIndex(i, &selected, &sd);
        if (FAILED(hr)) continue;
        if (!selected) continue;

        ComPtr<IMFMediaTypeHandler> handler;
        hr = sd->GetMediaTypeHandler(&handler);
        if (FAILED(hr)) continue;

        DWORD typeCount = 0;
        hr = handler->GetMediaTypeCount(&typeCount);
        if (FAILED(hr)) continue;

        const uint32_t targetWidth = m_overrideSpec.width > 0
            ? m_overrideSpec.width : m_format.width;
        const uint32_t targetHeight = m_overrideSpec.height > 0
            ? m_overrideSpec.height : m_format.height;
        const uint32_t targetFps = m_overrideSpec.fps > 0
            ? m_overrideSpec.fps : m_format.fps;

        uint32_t bestWidth = 0, bestHeight = 0, bestFps = 0;
        std::vector<P010Candidate> p010Candidates;

        // When the caller requested P010 (HDR10 capture), restrict the
        // best-format search to entries whose subtype is actually P010.
        // Otherwise FindBestFormat picks the device's largest resolution
        // by raw width: which on the 4K S is 4K@60 NV12, where P010 isn't
        // published, and the subsequent NegotiateFormat() call into MF
        // rejects with MF_E_INVALIDMEDIATYPE (0xc00d36b4).
        //
        // Existing Elgato and SDR selection remains largest width, then FPS.
        // Only GC553Pro full-auto P010 uses the gaming-oriented selector.
        //
        // README's "Known limitations" section documents this: "Elgato 4K S:
        // 1080p HDR or 4K SDR, not both."
        for (DWORD t = 0; t < typeCount; t++) {
            ComPtr<IMFMediaType> type;
            hr = handler->GetMediaTypeByIndex(t, &type);
            if (FAILED(hr)) continue;

            GUID subtype = {};
            if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) continue;
            if (m_requestP010 && !IsEqualGUID(subtype, MFVideoFormat_P010)) continue;

            UINT32 w = 0, h = 0;
            if (FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE,
                                          &w, &h))) continue;

            UINT32 fps = GetFpsFromMediaType(type.Get());

            if (gamingP010Auto) {
                p010Candidates.push_back({w, h, fps});
                continue;
            }

            if (w > bestWidth || (w == bestWidth && fps > bestFps)) {
                bestWidth = w;
                bestHeight = h;
                bestFps = fps;
            }
        }

        if (gamingP010Auto) {
            const P010SelectionResult selection = SelectGamingP010Candidate(
                p010Candidates, targetWidth, targetHeight, targetFps);
            if (selection.index != static_cast<size_t>(-1)) {
                const auto& best = p010Candidates[selection.index];
                m_format.width = best.width;
                m_format.height = best.height;
                m_format.fps = best.fps;

                std::wstringstream selectionLog;
                selectionLog << L"P010 Auto selected: "
                             << best.width << L"x" << best.height << L" @ "
                             << best.fps << L" FPS; reason="
                             << P010SelectionReasonText(selection.reason);
                DebugLog(selectionLog.str());

                if (targetWidth > 0 && targetHeight > 0 &&
                    (best.width != targetWidth || best.height != targetHeight ||
                     (targetFps > 0 && best.fps != targetFps))) {
                    m_p010SelectionNotice = {
                        true, best.width, best.height, best.fps
                    };
                }
                return true;
            }
            DebugLog(L"P010 Auto policy found no native P010 candidates");
        }

        if (bestWidth > 0) {
            m_format.width = bestWidth;
            m_format.height = bestHeight;
            m_format.fps = bestFps;

            std::wstringstream ss;
            ss << L"Native best format: " << bestWidth << L"x" << bestHeight << L" @ " << bestFps << L"fps";
            DebugLog(ss.str());
            return true;
        }
    }

    m_format.width = 1920;
    m_format.height = 1080;
    m_format.fps = 60;
    return true;
}

// Map a Media Foundation video subtype GUID to a human-readable name.
// Includes everything relevant for HDR work (P010 for 10-bit BT.2020,
// the 8-bit YUV variants for fallback, and the RGB formats currently
// in use). Unknown GUIDs are returned as their first-DWORD ASCII signature
// when it looks like a FourCC, else "UNKNOWN".
static std::wstring SubtypeToName(const GUID& g) {
    // Common Media Foundation video subtypes. These GUIDs are stable across
    // Windows versions (defined in mfapi.h) so they can be matched directly.
    if (IsEqualGUID(g, MFVideoFormat_P010))   return L"P010 (10-bit YUV 4:2:0, BT.2020-PQ-friendly)";
    if (IsEqualGUID(g, MFVideoFormat_P016))   return L"P016 (16-bit YUV 4:2:0)";
    if (IsEqualGUID(g, MFVideoFormat_P210))   return L"P210 (10-bit YUV 4:2:2)";
    if (IsEqualGUID(g, MFVideoFormat_P216))   return L"P216 (16-bit YUV 4:2:2)";
    if (IsEqualGUID(g, MFVideoFormat_NV12))   return L"NV12 (8-bit YUV 4:2:0)";
    if (IsEqualGUID(g, MFVideoFormat_NV11))   return L"NV11";
    if (IsEqualGUID(g, MFVideoFormat_YV12))   return L"YV12";
    if (IsEqualGUID(g, MFVideoFormat_I420))   return L"I420 (8-bit YUV 4:2:0)";
    if (IsEqualGUID(g, MFVideoFormat_IYUV))   return L"IYUV";
    if (IsEqualGUID(g, MFVideoFormat_YUY2))   return L"YUY2 (8-bit YUV 4:2:2)";
    if (IsEqualGUID(g, MFVideoFormat_UYVY))   return L"UYVY";
    if (IsEqualGUID(g, MFVideoFormat_Y210))   return L"Y210 (10-bit YUV 4:2:2 packed)";
    if (IsEqualGUID(g, MFVideoFormat_Y216))   return L"Y216";
    if (IsEqualGUID(g, MFVideoFormat_Y410))   return L"Y410 (10-bit YUV 4:4:4 packed)";
    if (IsEqualGUID(g, MFVideoFormat_Y416))   return L"Y416";
    if (IsEqualGUID(g, MFVideoFormat_RGB24))  return L"RGB24";
    if (IsEqualGUID(g, MFVideoFormat_RGB32))  return L"RGB32 (8-bit BGRA, the SDR default)";
    if (IsEqualGUID(g, MFVideoFormat_ARGB32)) return L"ARGB32";
    if (IsEqualGUID(g, MFVideoFormat_RGB555)) return L"RGB555";
    if (IsEqualGUID(g, MFVideoFormat_RGB565)) return L"RGB565";
    if (IsEqualGUID(g, MFVideoFormat_AYUV))   return L"AYUV";
    if (IsEqualGUID(g, MFVideoFormat_MJPG))   return L"MJPEG (compressed)";
    if (IsEqualGUID(g, MFVideoFormat_H264))   return L"H264 (compressed)";
    if (IsEqualGUID(g, MFVideoFormat_HEVC))   return L"HEVC (compressed)";

    // Fall back to a readable representation. Most MF video subtypes have
    // a FourCC in the first 4 bytes of the GUID (e.g. 'P010' = 0x30313050)
    // so if those bytes look like printable ASCII they're surfaced.
    DWORD fourcc = g.Data1;
    char fc[5] = {
        static_cast<char>( fourcc        & 0xFF),
        static_cast<char>((fourcc >>  8) & 0xFF),
        static_cast<char>((fourcc >> 16) & 0xFF),
        static_cast<char>((fourcc >> 24) & 0xFF),
        '\0'
    };
    bool printable = true;
    for (int i = 0; i < 4; i++) {
        if (fc[i] < 0x20 || fc[i] > 0x7E) { printable = false; break; }
    }
    if (printable) {
        std::wstring s = L"FourCC '";
        for (int i = 0; i < 4; i++) s += static_cast<wchar_t>(fc[i]);
        s += L"' (unknown to NitLink)";
        return s;
    }
    return L"UNKNOWN";
}

bool CaptureDevice::LogAvailableFormats()
{
    if (m_dshow) {
        // The DirectShow backend does not populate the MF format cache;
        // the F1 picker stays empty while the marker is present.
        return true;
    }

    // Clear the cache up front so repeated calls (e.g. after a format
    // reconcile re-Open) do not stack duplicate entries. If the source is
    // gone the cache stays empty and the F1 dropdowns fall back to
    // a single "Auto" option.
    m_availableFormats.clear();

    if (!m_source) {
        DebugLog(L"LogAvailableFormats: no media source open");
        return false;
    }

    ComPtr<IMFPresentationDescriptor> pd;
    HRESULT hr = m_source->CreatePresentationDescriptor(&pd);
    if (FAILED(hr)) {
        DebugLog(L"LogAvailableFormats: CreatePresentationDescriptor failed");
        return false;
    }

    DWORD streamCount = 0;
    pd->GetStreamDescriptorCount(&streamCount);

    auto Log = [](const std::wstring& msg) {
        OutputDebugStringW((L"[NitLink/Formats] " + msg + L"\n").c_str());
    };

    Log(L"==================== Available formats ====================");

    DWORD totalLogged = 0;
    bool sawP010 = false;
    bool sawNV12 = false;
    bool sawTenBit = false;

    for (DWORD si = 0; si < streamCount; si++) {
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        pd->GetStreamDescriptorByIndex(si, &selected, &sd);
        if (!selected) continue;

        ComPtr<IMFMediaTypeHandler> handler;
        sd->GetMediaTypeHandler(&handler);

        // Verify this is a video stream (skip audio if any)
        GUID major{};
        handler->GetMajorType(&major);
        if (!IsEqualGUID(major, MFMediaType_Video)) continue;

        DWORD typeCount = 0;
        handler->GetMediaTypeCount(&typeCount);
        {
            std::wstringstream ss;
            ss << L"Stream " << si << L" has " << typeCount << L" media types:";
            Log(ss.str());
        }

        for (DWORD t = 0; t < typeCount; t++) {
            ComPtr<IMFMediaType> type;
            handler->GetMediaTypeByIndex(t, &type);

            GUID subtype{};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);

            UINT32 w = 0, h = 0;
            MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);

            UINT32 fps = GetFpsFromMediaType(type.Get());

            UINT32 interlace = 0;
            type->GetUINT32(MF_MT_INTERLACE_MODE, &interlace);

            // Track gold-standard formats for the summary at the end
            if (IsEqualGUID(subtype, MFVideoFormat_P010)) sawP010 = true;
            if (IsEqualGUID(subtype, MFVideoFormat_NV12)) sawNV12 = true;
            if (IsEqualGUID(subtype, MFVideoFormat_P010) ||
                IsEqualGUID(subtype, MFVideoFormat_P210) ||
                IsEqualGUID(subtype, MFVideoFormat_Y210) ||
                IsEqualGUID(subtype, MFVideoFormat_Y410)) {
                sawTenBit = true;
            }

            std::wstringstream ss;
            ss << L"  [" << t << L"] " << w << L"x" << h << L" @ " << fps
               << L"fps  " << SubtypeToName(subtype);
            if (interlace != MFVideoInterlace_Progressive) ss << L"  (interlaced)";
            Log(ss.str());
            totalLogged++;

            // Cache the entry for the F1 Source picker's override dropdowns.
            // Interlaced entries are kept here; the UI filters them out
            // (no consumer-grade target source sends interlaced, but
            // exposing them in the diagnostic view stays consistent with
            // the [NitLink/Formats] log above).
            AvailableFormat af;
            af.width      = w;
            af.height     = h;
            af.fps        = fps;
            af.subtype    = subtype;
            af.interlaced = (interlace != MFVideoInterlace_Progressive);
            m_availableFormats.push_back(af);
        }
    }

    Log(L"------------------------- Summary -------------------------");
    {
        std::wstringstream ss;
        ss << L"Total media types: " << totalLogged;
        Log(ss.str());
    }
    Log(sawP010    ? L"  P010 (10-bit BT.2020 PQ): YES, real HDR10 path is viable"
                   : L"  P010 (10-bit BT.2020 PQ): no, will need fallback");
    Log(sawTenBit  ? L"  Any 10-bit format: YES"
                   : L"  Any 10-bit format: no, device exposes only 8-bit");
    Log(sawNV12    ? L"  NV12 (8-bit YUV): YES" : L"  NV12 (8-bit YUV): no");
    Log(L"===========================================================");

    return totalLogged > 0;
}

void CaptureDevice::Close()
{
    m_availableFormats.clear();
    if (m_dshow) {
        m_dshow->Close();
        m_dshow.reset();
        m_capturing = false;
        return;
    }

    StopCapture();
    m_reader.Reset();
    m_readState.reset();
    if (m_source) {
        m_source->Shutdown();
        m_source.Reset();
    }
}

bool CaptureDevice::StartCapture(FrameCallback callback)
{
    if (m_dshow) {
        const bool ok = m_dshow->StartCapture(std::move(callback));
        m_capturing = ok;
        return ok;
    }

    if (m_capturing) return false;
    if (!m_reader || !m_readState) return false;
    if (m_captureThread.joinable()) StopCapture();

    {
        std::lock_guard<std::mutex> lock(m_readState->mutex);
        // Restarting this reader is safe only once its previous flush has
        // completed. Normal format/device changes create a fresh reader.
        if (m_readState->flushing || m_readState->readerFailed) return false;
        m_readState->stopped = false;
        m_readState->completed = false;
        m_readState->sample.Reset();
    }

    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_callback = std::move(callback);
    }
    m_capturing = true;
    m_captureThread = std::thread(&CaptureDevice::CaptureLoop, this);

    return true;
}

void CaptureDevice::StopCapture()
{
    if (m_dshow) {
        m_dshow->StopCapture();
        m_capturing = false;
        return;
    }

    // Separate the worker's lifetime from the outstanding MF request. First
    // clear m_capturing, mark the shared state stopped and wake its condition
    // variable. The worker can leave its wait even if the card supplies no
    // further sample, and a late completion cannot publish another frame.
    // Join next: no frame callback may still be using the application's
    // FrameBuffer when StopCapture returns, and the worker must be unable to
    // issue another ReadSample behind the cancellation performed below.
    //
    // Only after the join does Flush cancel the outstanding reader request.
    // The async reader reports completion through OnFlush; shutdown does not
    // wait for that callback, since doing so would make exit depend on another
    // MF completion from a stalled source. The callback's shared-state
    // ownership makes a later OnFlush safe even after Close or destruction.
    // Set flushing before calling Flush so an immediate completion can clear
    // it; StartCapture rejects reuse until it clears. A reader that reported
    // MF_SOURCE_READERF_ERROR cannot receive Flush or any other reader method:
    // its recovery goes through a fresh Open in ReconcileCaptureFormat.
    // Reference: https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsourcereader-flush
    m_capturing = false;
    const bool hadWorker = m_captureThread.joinable();
    if (m_readState) {
        {
            std::lock_guard<std::mutex> lock(m_readState->mutex);
            m_readState->stopped = true;
            m_readState->sample.Reset();
        }
        m_readState->ready.notify_all();
    }
    if (hadWorker) {
        m_captureThread.join();
        if (m_reader && m_readState) {
            bool canFlush;
            {
                std::lock_guard<std::mutex> lock(m_readState->mutex);
                canFlush = !m_readState->readerFailed;
                m_readState->flushing = canFlush;
            }
            if (canFlush && FAILED(m_reader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM)))
                m_needsReopen = true;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_callback = {};
    }
}

void CaptureDevice::CaptureLoop()
{
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comHr)) {
        m_needsReopen = true;
        m_capturing = false;
        return;
    }
    bool loggedFirstFrame = false;
    bool probedDeviceTimestamp = false;

    // Retry budget for transient ReadSample failures. PS5 console transitions
    // (boot logo, source switch, SDR<->HDR boundary, dashboard->game launch)
    // routinely produce a few frames of HDMI signal drop, which the Media
    // Foundation source reader surfaces as a FAILED(hr) on ReadSample. The
    // previous loop broke on the first failure, permanently killing the
    // capture thread for what was a recoverable hiccup. The loop now sleeps
    // briefly and retries; only after kMaxConsecutiveFailures back-to-back
    // failures does it flag for a full reopen.
    int consecutiveFailures = 0;
    constexpr int kMaxConsecutiveFailures = 5;
    constexpr auto kFailureSleep = std::chrono::milliseconds(75);

    while (m_capturing) {
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;

        HRESULT hr = m_reader->ReadSample(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM,
            0, nullptr, nullptr, nullptr, nullptr
        );
        if (SUCCEEDED(hr)) {
            std::unique_lock<std::mutex> lock(m_readState->mutex);
            m_readState->ready.wait(lock, [this] {
                return m_readState->stopped || m_readState->completed;
            });
            if (m_readState->stopped) break;
            hr = m_readState->status;
            flags = m_readState->flags;
            timestamp = m_readState->timestamp;
            sample = std::move(m_readState->sample);
            m_readState->completed = false;
        }
        if (!m_capturing) break;

        // ---- Reader fatal error ----
        // MF_SOURCE_READERF_ERROR carries the contract "do not make any further
        // calls to IMFSourceReader methods." It applies even when OnReadSample
        // also reports a failing HRESULT, so this branch precedes the ordinary
        // failure retry budget. Retrying in place is impossible: recovery
        // requires a fresh source reader created by a fresh Open().
        //
        // OnReadSample also latches readerFailed, preventing StopCapture from
        // issuing Flush and StartCapture from reusing this reader. Setting
        // m_needsReopen hands recovery to Application::Run, which consumes it
        // through ConsumeNeedsReopen and forces ReconcileCaptureFormat to run
        // the StopCapture -> Close -> Open -> StartCapture sequence.
        // Reference: https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/ne-mfreadwrite-mf_source_reader_flag
        if (flags & MF_SOURCE_READERF_ERROR) {
            DebugLog(L"ReadSample flagged MF_SOURCE_READERF_ERROR, flagging force-reopen");
            m_needsReopen = true;
            break;
        }

        // ---- Transient call-level failure ----
        // Don't kill the thread on the first failure. Sleep briefly to avoid
        // burning CPU in a tight failure loop, then retry. After enough
        // consecutive failures the reader is presumed wedged and the
        // application is flagged to force a full reopen via
        // ReconcileCaptureFormat.
        if (FAILED(hr)) {
            consecutiveFailures++;
            std::wstringstream ss;
            ss << L"ReadSample failed (HRESULT 0x" << std::hex << hr
               << std::dec << L", consecutive=" << consecutiveFailures << L")";
            DebugLog(ss.str());
            if (consecutiveFailures >= kMaxConsecutiveFailures) {
                DebugLog(L"ReadSample retry budget exhausted, flagging force-reopen");
                m_needsReopen = true;
                break;
            }
            std::unique_lock<std::mutex> lock(m_readState->mutex);
            m_readState->ready.wait_for(lock, kFailureSleep, [this] {
                return m_readState->stopped;
            });
            continue;
        }
        consecutiveFailures = 0;

        // ---- End-of-stream ----
        // MF_SOURCE_READERF_ENDOFSTREAM means the capture card's selected stream
        // has ended, not merely that one HDMI frame is missing. A gap
        // can arrive as STREAMTICK or an empty sample; an ended stream needs
        // the capture session reopened rather than another ReadSample on the
        // same session. Signal loss or device removal can require that restart
        // even when the desired pixel format has not changed.
        //
        // Set m_needsReopen and leave the loop. CaptureLoop's common exit clears
        // m_capturing, so IsCapturing reports the stopped worker. Application::Run
        // consumes the flag through ConsumeNeedsReopen and forces
        // ReconcileCaptureFormat to StopCapture, Close, Open and StartCapture.
        // If Open fails while the source is absent, that function's retry
        // deadline continues recovery without requiring another stream event.
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            DebugLog(L"ReadSample: end of stream");
            m_needsReopen = true;
            break;
        }

        // ---- Format changed underneath the reader ----
        // Either the source switched media type (PS5 dashboard -> HDR game
        // launch, console SDR<->HDR boundary) or the native type changed
        // (driver renegotiated after signal recovery). The capture loop
        // cannot keep writing into m_frameBuffer with the prior stride/format
        // assumption: the next frame's bytes mean something different
        // now. Flag for reconcile and exit; the application will rebuild
        // the frame buffer at the new negotiated format.
        if (flags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED |
                     MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)) {
            DebugLog(L"ReadSample flagged media-type change, flagging force-reopen");
            m_needsReopen = true;
            break;
        }

        // ---- Stream tick (timing-only gap) ----
        // No media data this call. Common during brief signal dropouts
        // where the source reader keeps timing alive without delivering
        // actual frames. Just continue: not a failure.
        if (flags & MF_SOURCE_READERF_STREAMTICK) continue;
        if (!sample) continue;


        ComPtr<IMFMediaBuffer> buffer;
        hr = sample->ConvertToContiguousBuffer(&buffer);
        if (FAILED(hr)) continue;

        BYTE* rawData = nullptr;
        DWORD maxLen = 0, currentLen = 0;
        hr = buffer->Lock(&rawData, &maxLen, &currentLen);
        if (SUCCEEDED(hr)) {
            if (!loggedFirstFrame) {
                const CaptureFormat fmt = GetOutputFormat();
                std::wstringstream ss;
                ss << L"First frame received: " << currentLen << L" bytes for "
                   << fmt.width << L"x" << fmt.height
                   << L" (expected BGRA: " << (fmt.width * fmt.height * 4)
                   << L", expected NV12: " << (fmt.width * fmt.height * 3 / 2) << L")";
                DebugLog(ss.str());
                loggedFirstFrame = true;
            }

            // Read the device hardware timestamp on every sample. Per Microsoft
            // docs MFSampleExtension_DeviceTimestamp is in QPC 100ns units which
            // shares an epoch with steady_clock on Windows, so it is
            // directly comparable to arrivalWallNs for real card-to-app
            // delivery latency.
            // If the driver doesn't populate it (some non-Elgato cards), the
            // value stays 0 and the consumer side treats that as "unavailable".
            UINT64 deviceTs = 0;
            HRESULT dtHr = sample->GetUINT64(MFSampleExtension_DeviceTimestamp, &deviceTs);
            if (!probedDeviceTimestamp) {
                probedDeviceTimestamp = true;
                std::wstringstream ss;
                if (SUCCEEDED(dtHr)) {
                    ss << L"DeviceTimestamp: SUPPORTED (first value=" << deviceTs << L" 100ns ticks)";
                } else {
                    ss << L"DeviceTimestamp: NOT SUPPORTED (hr=0x" << std::hex << dtHr
                       << L"); MF delivery latency will be unavailable on this device";
                }
                DebugLog(ss.str());
            }

            FrameCallback cb;
            {
                std::lock_guard<std::mutex> lock(m_callbackMutex);
                cb = m_callback;
            }
            if (cb) {
                const int64_t arrivalWallNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                cb(rawData, currentLen, timestamp, arrivalWallNs, deviceTs);
            }
            buffer->Unlock();
        }
    }
    m_capturing = false;
    CoUninitialize();
}

} // namespace NitLink
