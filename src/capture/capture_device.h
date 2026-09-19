#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <string>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>
#include <cstdint>
#include <vector>
#include <memory>

using Microsoft::WRL::ComPtr;

namespace NitLink {

class DShowCapture;  // DirectShow capture backend (dshow_capture.h)
struct CaptureReadState;

struct CaptureFormat {
    uint32_t width  = 3840;
    uint32_t height = 2160;
    uint32_t fps    = 60;
    uint32_t stride = 0; // bytes per row
    GUID     subtype{};  // MF media subtype (NV12, YUY2, RGB32, etc.)
    // Row order of the captured frames. Media Foundation defaults to
    // bottom-up for legacy RGB formats (GDI convention) but some drivers
    // (e.g. Elgato 4K S) deliver top-down RGB32 instead. Determined by
    // querying MF_MT_DEFAULT_STRIDE: negative stride = bottom-up,
    // positive (or absent) = top-down. The renderer uses this to choose
    // whether to apply the V-flip in the BGRA shader path. Planar formats
    // like P010/NV12 are always top-down regardless of this flag.
    bool     topDown = true;
    // Pixel value range of the captured frames. Most HDMI sources (PS5,
    // Xbox) send LIMITED range (16-235): that's the HDMI spec default.
    // Some drivers convert the YUV input to FULL range (0-255) before
    // handing it off; the Elgato 4K S does this on its NV12 output
    // path. Determined by querying MF_MT_VIDEO_NOMINAL_RANGE on the
    // negotiated media type. The shaders use this to decide whether to
    // apply the limited-to-full range expansion. Getting this wrong crushes
    // blacks (treating full as limited) or washes out the image
    // (treating limited as full).
    bool     fullRange = false;
};

struct DeviceInfo {
    std::wstring name;
    std::wstring symbolicLink;
    uint32_t     index = 0;
};

enum class CaptureDeviceFamily {
    Generic,
    AverMediaGC553Pro,
};

struct CaptureDevicePolicy {
    CaptureDeviceFamily family = CaptureDeviceFamily::Generic;
    bool preferHighFpsP010 = false;
};

CaptureDevicePolicy GetCaptureDevicePolicy(const std::wstring& deviceName);

struct P010SelectionNotice {
    bool available = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
};

// One native media type as exposed by the capture source. Populated during
// LogAvailableFormats(), consumed by the F1 Source picker's manual override
// dropdowns. Interlaced entries are kept in the list (the user-facing UI
// filters them out) so that diagnostic / debug surfaces still see the full
// set the device offers.
struct AvailableFormat {
    uint32_t width      = 0;
    uint32_t height     = 0;
    uint32_t fps        = 0;
    GUID     subtype{};
    bool     interlaced = false;
};

// Callback: raw frame data, size in bytes, presentation timestamp (100ns units),
// arrival wall-clock (steady_clock nanoseconds, captured when MF handed the frame),
// device hardware timestamp (MFSampleExtension_DeviceTimestamp, QPC 100ns units;
// 0 if the driver does not populate the attribute).
using FrameCallback = std::function<void(const uint8_t*, uint32_t, int64_t, int64_t, uint64_t)>;

class CaptureDevice {
public:
    CaptureDevice();
    ~CaptureDevice();

    bool Open(const DeviceInfo& device);
    void Close();

    bool StartCapture(FrameCallback callback);
    void StopCapture();

    // Request the P010 (10-bit BT.2020 PQ) output format on next Open().
    // Must be called BEFORE Open(): changing it later has no effect on the
    // already-opened reader. The caller can request it from trusted Elgato
    // source detection or an explicit GC553Pro manual HDR preference; MF
    // remains the authority on whether native P010 is exposed and accepted.
    // If P010 negotiation fails inside Open(), Open() returns false; the
    // caller should retry with this flag false to get the SDR pipeline.
    void RequestP010(bool want) { m_requestP010 = want; }
    bool IsP010Requested() const { return m_requestP010; }

    // Manual format override from the F1 Source picker. Each numeric
    // field at 0 means "Auto" for that dimension; empty format string
    // means Auto for format. Same lifecycle as RequestP010: set before
    // Open(), applies to that Open call. If the requested combination
    // cannot be negotiated on the live source, Open falls back to
    // automatic (using NegotiateFormat's native-best + standard
    // attempts) and sets m_fallbackNotice so the JSON state push can
    // surface a toast.
    struct OverrideSpec {
        uint32_t     width  = 0;
        uint32_t     height = 0;
        uint32_t     fps    = 0;
        std::wstring format;  // "NV12" / "P010" / "BGRA" / "" for Auto

        bool isFullAuto() const {
            return width == 0 && height == 0 && fps == 0 && format.empty();
        }
    };
    void SetFormatOverride(const OverrideSpec& ov) { m_overrideSpec = ov; }

    // True if the capture worker observed a fatal stream condition since the
    // last call: MF_SOURCE_READERF_ERROR (reader permanently failed),
    // MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED / _NATIVEMEDIATYPECHANGED
    // (the source renegotiated format underneath the reader: common across
    // PS5 SDR<->HDR transitions and after HDMI signal recovery), or repeated
    // ReadSample failures that exceeded the in-loop retry budget. The
    // application's run loop polls this and routes through ReconcileCaptureFormat
    // with force=true to perform a clean teardown + re-Open. Test-and-clear
    // semantics: each call returns the current state and resets it to false.
    bool ConsumeNeedsReopen() { return m_needsReopen.exchange(false); }

    // Thread-safe snapshot of the negotiated output format. m_format itself is
    // main-thread negotiation working state (written field-by-field across
    // NegotiateFormat and Open); the finalized values are committed once at the
    // end of a successful Open() under m_formatMutex. Every reader outside that
    // negotiation (the render loop, the capture worker's first-frame
    // diagnostic, the HUD stats path) goes through this accessor so it always
    // sees a complete, consistent struct instead of a half-updated one.
    CaptureFormat GetOutputFormat() const {
        std::lock_guard<std::mutex> lock(m_formatMutex);
        return m_publishedFormat;
    }
    std::wstring  GetDeviceName()   const { return m_deviceName; }
    bool          IsCapturing()     const { return m_capturing; }

    // Enumerate every native media type this device exposes via Media
    // Foundation. Two responsibilities:
    //   1. Log each type to OutputDebugString under [NitLink/Formats] tag
    //      for diagnostics.
    //   2. Populate the m_availableFormats cache so the F1 Source picker
    //      can render the manual override dropdowns. The cache is cleared
    //      at the top of the function so repeated calls do not duplicate.
    //
    // Walks the IMFStreamDescriptor for the selected video stream, iterates
    // every media type the handler exposes.
    //
    // Safe to call after Open() succeeds. Returns true if at least one
    // format was enumerated, false on COM failures. Non-const because the
    // cache population is real state mutation.
    bool LogAvailableFormats();

    // Read-only view of the cache populated by LogAvailableFormats(). Empty
    // before Open() / LogAvailableFormats() has been called. Order matches
    // the enumeration order Media Foundation returns (which is also the
    // order shown in the [NitLink/Formats] log lines).
    const std::vector<AvailableFormat>& GetAvailableFormats() const {
        return m_availableFormats;
    }

    // Test-and-clear: returns any pending user-facing notice produced by
    // format negotiation (e.g. "Capture format unavailable, reverted to
    // automatic" when the user's manual override could not be honored)
    // and clears the internal state. Same pattern as ConsumeNeedsReopen.
    // Called by PushSettingsState; the wstring is forwarded to JS as the
    // "notification" field of the state blob so the menu can render a
    // transient toast. Empty return = no pending notice.
    //
    // Main-thread-only: written from format negotiation (called during
    // Open / ReconcileCaptureFormat on the main thread) and read by
    // PushSettingsState (also main thread). No atomic needed.
    std::wstring ConsumeFallbackNotice() {
        std::wstring out;
        out.swap(m_fallbackNotice);
        return out;
    }

    P010SelectionNotice ConsumeP010SelectionNotice() {
        const P010SelectionNotice out = m_p010SelectionNotice;
        m_p010SelectionNotice = {};
        return out;
    }

private:
    void CaptureLoop();
    bool NegotiateFormat(IMFMediaSource* source);

    // Commit the finalized m_format into the mutex-guarded m_publishedFormat.
    // Called once at the end of a successful Open(), after every format field
    // has been negotiated. This is the single synchronization point that makes
    // GetOutputFormat() safe to call from any thread.
    void PublishFormat();

    ComPtr<IMFMediaSource>  m_source;
    ComPtr<IMFSourceReader> m_reader;
    // Shared only with the reader callback. Late callbacks never dereference
    // CaptureDevice, and the worker's wait can be cancelled without a sample.
    std::shared_ptr<CaptureReadState> m_readState;

    // Negotiation working state. Written field-by-field throughout
    // NegotiateFormat() and Open()'s output-format negotiation, all on the main
    // thread while no capture worker is running. Other threads do NOT read this
    // directly; they read m_publishedFormat via GetOutputFormat().
    CaptureFormat   m_format;

    // Thread-safe published copy of m_format, committed by PublishFormat() and
    // read under m_formatMutex by GetOutputFormat(). Decouples the multi-field
    // struct read from the scattered writes so readers never observe a torn
    // (partially updated) value.
    CaptureFormat        m_publishedFormat;
    mutable std::mutex   m_formatMutex;

    std::wstring    m_deviceName;

    // The frame callback and a mutex guarding swaps of it. StartCapture
    // assigns m_callback; the capture worker reads + invokes it once per
    // frame. Today the join barrier in StopCapture (always called before any
    // re-StartCapture, on the main thread) makes a torn swap unreachable, but
    // that relies on an undocumented "lifecycle calls are main-thread-only"
    // invariant. The mutex makes the swap robustly safe regardless: the worker
    // copies m_callback into a local under the lock, releases, then invokes the
    // local, so the lock is never held across the per-frame callback work.
    // The lambda only captures `this`, so the copy fits std::function's small-
    // buffer optimization and is allocation-free.
    FrameCallback      m_callback;
    mutable std::mutex m_callbackMutex;
    
    std::thread       m_captureThread;
    std::atomic<bool> m_capturing{false};

    // Set via RequestP010() before Open(). When true, Open() will try
    // MFVideoFormat_P010 first (and ONLY P010: no fallback within HDR
    // path, the caller decides whether to retry without P010).
    bool m_requestP010 = false;

    // Set via SetFormatOverride() before Open(). When any field is
    // non-default, Open() tries to negotiate exactly the requested
    // resolution / fps / format and falls back to automatic with a
    // fallback notice if unachievable. See OverrideSpec above.
    OverrideSpec m_overrideSpec;

    // Set by CaptureLoop on fatal stream conditions; consumed by the
    // application's run loop via ConsumeNeedsReopen(). When set, the worker
    // thread has already exited and the application is expected to force a
    // reconcile to bring capture back up.
    std::atomic<bool> m_needsReopen{false};

    // Cache of every native media type the source exposes. Populated by
    // LogAvailableFormats(). Backs the JSON push that powers the F1 Source
    // picker's resolution / framerate / format cascade dropdowns. Cleared
    // and refilled on each LogAvailableFormats() call so a re-Open after a
    // format reconcile shows the live device's current advertised set.
    std::vector<AvailableFormat> m_availableFormats;

    // One-shot user-facing notice produced by format negotiation when a
    // manual override cannot be honored. Set inside the negotiation path
    // (Open / ReconcileCaptureFormat), consumed and cleared by the JSON
    // push via ConsumeFallbackNotice(). See that accessor's doc comment.
    std::wstring m_fallbackNotice;

    // Published only after Open confirms that the negotiated output remains
    // P010, preventing a failed attempt followed by SDR fallback from showing
    // a stale native-mode notice.
    P010SelectionNotice m_p010SelectionNotice;

    // Optional DirectShow capture backend, created only when USE_DSHOW.txt is
    // present next to the exe. When non-null, every public method delegates to
    // it and the Media Foundation members above stay unused.
    std::unique_ptr<DShowCapture> m_dshow;
};

} // namespace NitLink
