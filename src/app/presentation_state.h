#pragma once

#include <cstdint>

namespace NitLink {

// Single render decision used by both SDR and HDR paths. The no-signal latch
// deliberately has priority over transition and capture readiness so a
// format reconcile cannot expose an invalidated capture surface.
// Waiting and NoSignal are independent presentations; they do not require a
// fresh capture frame.
enum class PresentationState {
    WaitingForCapture,
    Capture,
    NoSignal,
    Transition,
    SignalResync,
};

// Existing no-valid-frame reacquire debounce, reused only as an eligible
// backend's fallback. A known capture transition need not wait for this timer.
inline constexpr std::int64_t kCaptureReacquireDebounceMs = 250;

constexpr bool ShouldShowSignalResync(bool frameGapFallbackEligible,
                                      bool hadStableCapture,
                                      bool noSignalPresentation,
                                      std::int64_t elapsedWithoutValidFrameMs,
                                      bool captureTransitionActive = false) noexcept
{
    return hadStableCapture && !noSignalPresentation &&
        (captureTransitionActive || (frameGapFallbackEligible &&
         elapsedWithoutValidFrameMs >= kCaptureReacquireDebounceMs));
}

// A classified Real frame is not presentable until the renderer has accepted
// it into the current GPU capture texture. This prevents the render loop from
// selecting Capture while DrawCaptureFrame still has no usable surface.
// This applies to the current capture session.
constexpr bool CapturePresentationReady(bool acceptedReal,
                                        bool rendererHasFrame) noexcept
{
    return acceptedReal && rendererHasFrame;
}

constexpr PresentationState DecidePresentation(bool noSignalLatched,
                                                bool showNoSignalNow,
                                                bool captureReady,
                                                bool transitionActive,
                                                bool signalResync = false) noexcept
{
    if (noSignalLatched || showNoSignalNow) return PresentationState::NoSignal;
    if (signalResync) return PresentationState::SignalResync;
    if (transitionActive) return PresentationState::Transition;
    if (captureReady) return PresentationState::Capture;
    return PresentationState::WaitingForCapture;
}

// The same localized presentation is used by SDR and HDR render paths.
// Startup format-reopen waits do not claim that a stable source was lost.
constexpr const wchar_t* CaptureStatusLocalizationKey(PresentationState state,
                                                       bool captureStarted) noexcept
{
    if (state == PresentationState::SignalResync) return L"overlay.resyncingSignal";
    if (state == PresentationState::WaitingForCapture || state == PresentationState::Transition)
        return captureStarted ? L"overlay.waitingForSource" : L"overlay.initializingCapture";
    return nullptr; // Capture and NoSignal have their own rendering.
}

// A zero-zone GC553Pro frame can be legal FULL-range black content. Keep it
// accepted as Real; this state only controls a temporary presentation hint.
// The hint ends when visible content arrives or the known placeholder is
// confirmed, and does not affect source liveness or detector classification.
struct StartupBlackFrameHint {
    static constexpr std::uint64_t kMaximumVisibleDurationMs = 3000;

    bool eligible = true;
    bool visible = false;
    bool activationStarted = false;
    std::uint64_t activatedAtMs = 0;

    constexpr void ObserveFrame(bool acceptedReal, bool zeroLumaZones) noexcept {
        if (!eligible || !acceptedReal) return;
        if (zeroLumaZones) {
            visible = true;
        } else {
            eligible = false;
            visible = false;
            activationStarted = false;
        }
    }

    constexpr void ConfirmPlaceholder() noexcept {
        eligible = false;
        visible = false;
        activationStarted = false;
    }

    constexpr void Reset() noexcept {
        eligible = true;
        visible = false;
        activationStarted = false;
        activatedAtMs = 0;
    }
};

constexpr bool ShouldDrawStartupBlackFrameHint(bool isGc553Pro,
                                               PresentationState state,
                                               StartupBlackFrameHint& hint,
                                               std::uint64_t nowMs) noexcept
{
    if (!isGc553Pro || state != PresentationState::Capture ||
        !hint.eligible || !hint.visible) {
        return false;
    }

    // Start the cap only when this presentation hint can actually be drawn,
    // not at application launch or while WaitingForCapture is still shown.
    if (!hint.activationStarted) {
        hint.activationStarted = true;
        hint.activatedAtMs = nowMs;
    }

    if (nowMs >= hint.activatedAtMs &&
        nowMs - hint.activatedAtMs >= StartupBlackFrameHint::kMaximumVisibleDurationMs) {
        hint.eligible = false;
        hint.visible = false;
        return false;
    }

    return true;
}

} // namespace NitLink
