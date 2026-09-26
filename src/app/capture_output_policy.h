#pragma once

#include <cstdint>
#include "capture/gc553pro_hdr_source.h"

namespace NitLink {

// RequestP010() describes a future Open() attempt. The negotiated subtype
// returned by CaptureDevice::GetOutputFormat() describes the stream that is
// actually running and is therefore the only input to this runtime policy.
enum class NegotiatedCaptureFormatKind {
    Other,
    NV12,
    P010,
};

// The user's format preference is kept separate from the negotiated stream.
// An empty format override is Auto, even when dimensions or FPS are pinned.
enum class CaptureFormatPreference {
    Auto,
    ManualNV12,
    ManualP010,
    ManualOther,
};

// Frame rate is one logical value. Any application-side integer-rate
// override (including 4K X HDR clamps and follow-source updates) must update
// the display FPS and the exact rational fields together.
constexpr void SetIntegerFrameRateFields(
    uint32_t& fps,
    uint32_t& fpsNumerator,
    uint32_t& fpsDenominator,
    uint32_t value) noexcept
{
    fps = value;
    fpsNumerator = value;
    fpsDenominator = 1;
}

// Keep the existing non-GC553Pro source-detection policy intact, except when
// the user has explicitly pinned a manual pixel format. A manual SDR format
// is an accepted constraint and must not be turned into a repeated P010
// request merely because the HDMI source reports HDR10.
constexpr bool ApplyManualFormatPreferenceToNonGcPolicy(
    bool sourcePolicyWantsP010,
    CaptureFormatPreference preference) noexcept
{
    if (preference == CaptureFormatPreference::ManualNV12 ||
        preference == CaptureFormatPreference::ManualOther) {
        return false;
    }
    if (preference == CaptureFormatPreference::ManualP010) {
        return true;
    }
    return sourcePolicyWantsP010;
}

// A published negotiated format can only be reused by the policy when it
// belongs to the currently selected capture device/session. A device switch
// must begin with an unknown negotiated subtype until the new Open() publishes
// its own format.
constexpr NegotiatedCaptureFormatKind ScopedNegotiatedCaptureFormat(
    bool belongsToCurrentDeviceSession,
    NegotiatedCaptureFormatKind actualFormat) noexcept
{
    return belongsToCurrentDeviceSession
        ? actualFormat
        : NegotiatedCaptureFormatKind::Other;
}

// Records a completed P010 request, not the mutable RequestP010 preference.
// The application resets it before closing the session or changing policy.
class NonGcP010FallbackState {
public:
    void Reset() noexcept { m_accepted = false; }
    void ObservePolicy(bool desiredP010, bool currentSessionKnown,
                       bool forceReopen) noexcept {
        if (!desiredP010 || !currentSessionKnown || forceReopen) Reset();
    }
    void CompleteOpen(bool attemptedP010, bool actualP010) noexcept {
        m_accepted = attemptedP010 && !actualP010;
    }
    bool Accepted() const noexcept { return m_accepted; }
private:
    bool m_accepted = false;
};

// A non-GC capture stream that honored a non-format override (for example,
// resolution-only with Format=Auto) may legitimately negotiate NV12 even when
// source policy prefers P010. Once that concrete fallback is running, the
// render-loop reconcile must not reopen it every iteration merely because the
// requested and negotiated subtypes differ. Explicit/forced reconciles still
// retry the preference, and a fully automatic capture remains policy-driven.
constexpr bool ShouldReopenNonGcCapture(
    bool desiredP010,
    NegotiatedCaptureFormatKind actualFormat,
    bool actualFormatKnown,
    CaptureFormatPreference preference,
    bool hasNonFormatOverride,
    bool acceptedP010FallbackForCurrentSession) noexcept
{
    const bool actualP010 = actualFormat == NegotiatedCaptureFormatKind::P010;
    const bool acceptedAutoFallback =
        preference == CaptureFormatPreference::Auto &&
        hasNonFormatOverride &&
        actualFormatKnown &&
        acceptedP010FallbackForCurrentSession &&
        desiredP010 &&
        !actualP010;

    return acceptedAutoFallback ? false : desiredP010 != actualP010;
}

struct Gc553ProOutputPolicy {
    bool desiredCaptureIsP010 = false;
    bool reopenCapture = false;
    bool sourceIsHDR10 = false;
    bool sdrFromHdrTonemap = false;
    bool hdrRejected = false;
};

// GC553Pro does not expose a supported vendor HDR source-state interface. This
// policy only decouples an already negotiated capture stream from the output
// preference; it does not claim that an SDR source is safe to reinterpret as
// P010.
constexpr Gc553ProOutputPolicy DecideGc553ProOutputPolicy(
    NegotiatedCaptureFormatKind actualFormat,
    bool hdrOutputEnabled,
    CaptureFormatPreference preference = CaptureFormatPreference::Auto) noexcept
{
    const bool actualP010 = actualFormat == NegotiatedCaptureFormatKind::P010;

    // A manually selected non-P010 format is a hard constraint. HDR output is
    // rejected by the caller instead of triggering a P010 request/reopen.
    if (preference == CaptureFormatPreference::ManualNV12 ||
        preference == CaptureFormatPreference::ManualOther) {
        return {
            false,
            false,
            actualP010,
            actualP010 && !hdrOutputEnabled,
            hdrOutputEnabled,
        };
    }

    // Manual P010 remains P010 for both output modes. If a previous attempt
    // did not actually negotiate P010, do not turn an output toggle into an
    // automatic retry loop; an explicit override/reopen can retry it.
    if (preference == CaptureFormatPreference::ManualP010) {
        return {
            true,
            false,
            actualP010,
            actualP010 && !hdrOutputEnabled,
            false,
        };
    }

    // Auto promotes NV12 to P010 only when HDR output is requested. Once P010
    // is actually running, retain it for SDR tone mapping as well.
    const bool desiredP010 = actualP010 || hdrOutputEnabled;
    return {
        desiredP010,
        desiredP010 != actualP010,
        actualP010,
        actualP010 && !hdrOutputEnabled,
        false,
    };
}

// Invalid reads never erase a known source state. This also prevents an
// initial default bool value from masquerading as an SDR probe.
constexpr Gc553ProSourceHdrState KeepLastGc553ProSourceState(
    Gc553ProSourceHdrState previous,
    Gc553ProSourceHdrState probe) noexcept {
    return probe == Gc553ProSourceHdrState::Unknown ? previous : probe;
}

struct Gc553ProSourceCapturePolicy {
    bool desiredCaptureIsP010 = false;
    bool reopenCapture = false;
    bool hdrRejected = false;
};

// GC553Pro Auto follows the confirmed source EOTF in both directions: HDR10/PQ
// selects P010 and SDR selects NV12. Other known HDR EOTFs keep the currently
// negotiated capture subtype because they are not SDR and do not have an
// established renderer policy. Unknown keeps the pre-existing startup/manual
// fallback; runtime Unknown reads are not published by the source poller.
// HDR output remains an independent user preference.
constexpr Gc553ProSourceCapturePolicy DecideGc553ProSourceOutputPolicy(
    NegotiatedCaptureFormatKind actualFormat,
    bool hdrOutputEnabled,
    CaptureFormatPreference preference,
    bool autoFromSource,
    Gc553ProSourceHdrState sourceState) noexcept {
    const auto manual = DecideGc553ProOutputPolicy(
        actualFormat, hdrOutputEnabled, preference);
    if (!autoFromSource || sourceState == Gc553ProSourceHdrState::Unknown ||
        preference != CaptureFormatPreference::Auto) {
        return {manual.desiredCaptureIsP010, manual.reopenCapture,
                manual.hdrRejected};
    }
    const bool actualP010 = actualFormat == NegotiatedCaptureFormatKind::P010;
    if (sourceState == Gc553ProSourceHdrState::OtherHdr)
        return {actualP010, false, false};
    const bool desiredP010 = sourceState == Gc553ProSourceHdrState::Hdr10Pq;
    return {desiredP010, desiredP010 != actualP010, false};
}

// The renderer's input HDR10 flag describes the source signal, not the MF
// subtype. Preserve the existing subtype-driven behavior for all other paths.
// SDR-in-P010 color decoding remains unverified for GC553Pro; this only avoids
// labeling SDR input as HDR10 and applying the known-PQ SDR tone map to it.
constexpr bool RendererInputIsHdr10(bool isGc553Pro, bool autoFromSource,
                                    bool sourceIsHdr10,
                                    bool captureIsP010) noexcept {
    return isGc553Pro && autoFromSource
        ? sourceIsHdr10 : captureIsP010;
}

// The source EOTF read is not attached to an MF sample. While a P010 stream is
// active, do not reinterpret the previously uploaded frame or a frame that
// was already delivered before the EOTF update was observed. The first later
// accepted Real frame may commit the new renderer input flag with its upload.
struct Gc553ProSourceFrameSync {
    bool pending = false;
    std::int64_t observedAtNs = 0;

    constexpr void Begin(std::int64_t nowNs) noexcept {
        pending = true;
        observedAtNs = nowNs;
    }
    constexpr bool Hold(std::int64_t frameArrivalNs) const noexcept {
        return pending && (frameArrivalNs <= 0 || frameArrivalNs <= observedAtNs);
    }
    constexpr bool Ready(std::int64_t frameArrivalNs) const noexcept {
        return pending && !Hold(frameArrivalNs);
    }
    constexpr void Reset() noexcept {
        pending = false;
        observedAtNs = 0;
    }
};

constexpr bool IsGc553ProAutoNv12ToP010Reopen(
    bool isGc553Pro, bool autoFromSource,
    CaptureFormatPreference preference, NegotiatedCaptureFormatKind actual,
    Gc553ProSourceHdrState source, bool requestingP010) noexcept {
    return isGc553Pro && autoFromSource &&
        preference == CaptureFormatPreference::Auto &&
        actual == NegotiatedCaptureFormatKind::NV12 &&
        source == Gc553ProSourceHdrState::Hdr10Pq && requestingP010;
}

} // namespace NitLink
