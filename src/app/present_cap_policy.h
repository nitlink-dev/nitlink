#pragma once

#include "config.h"
#include <cmath>

namespace NitLink {

enum class PresentCapReason {
    VSync, SourcePacing, Marker, Manual, Off, AutoHeadroom, AutoDisplayCeiling, UnknownDisplay
};

struct PresentCapInput {
    int configuredHz = 0;
    double monitorHz = 0.0;
    // A negotiation/pacing hint, never the measured Capture FPS or Content FPS.
    double sourceRateHint = 0.0;
    int pacing = kPacingRefresh;
    bool vsync = false;
    bool markerPinned = false;
    double markerHz = 0.0;
};

struct PresentCapDecision {
    double requestedHz = 0.0;
    double effectiveHz = 0.0;
    PresentCapReason reason = PresentCapReason::Off;
};

inline PresentCapDecision ChoosePresentCap(const PresentCapInput& input)
{
    // These paths already have their own pacing. A marker stays stored in the
    // renderer but is bypassed by VSync or by skipping the refresh-mode wait.
    if (input.vsync) return {0.0, 0.0, PresentCapReason::VSync};
    if (input.pacing != kPacingRefresh) return {0.0, 0.0, PresentCapReason::SourcePacing};

    PresentCapDecision result;
    if (input.configuredHz >= 30) {
        result = {static_cast<double>(input.configuredHz), 0.0, PresentCapReason::Manual};
    } else if (input.configuredHz < 0) {
        result.reason = PresentCapReason::Off;
    } else if (!std::isfinite(input.monitorHz) || input.monitorHz < 50.0) {
        result.reason = PresentCapReason::UnknownDisplay;
    } else {
        const double candidate = input.monitorHz - 3.0;
        const double source = std::isfinite(input.sourceRateHint) && input.sourceRateHint > 0.0
            ? input.sourceRateHint : 0.0;
        if (candidate >= source) {
            result = {candidate, 0.0, PresentCapReason::AutoHeadroom};
        } else {
            // Equal/faster capture must not turn Auto into unlimited Present.
            // Use the display ceiling when VRR headroom would skip source
            // frames, including sources faster than the display can show.
            result = {input.monitorHz, 0.0, PresentCapReason::AutoDisplayCeiling};
        }
    }
    result.effectiveHz = result.requestedHz;
    if (input.markerPinned) {
        result.effectiveHz = input.markerHz;
        result.reason = PresentCapReason::Marker;
    }
    return result;
}

inline const wchar_t* PresentCapReasonName(PresentCapReason reason)
{
    switch (reason) {
    case PresentCapReason::VSync: return L"VSync bypass";
    case PresentCapReason::SourcePacing: return L"source/capture pacing bypass";
    case PresentCapReason::Marker: return L"VRR_CAP.txt override";
    case PresentCapReason::Manual: return L"manual present_cap_hz";
    case PresentCapReason::Off: return L"explicit uncapped";
    case PresentCapReason::AutoHeadroom: return L"auto monitor minus 3";
    case PresentCapReason::AutoDisplayCeiling: return L"auto monitor ceiling (source exceeds headroom)";
    case PresentCapReason::UnknownDisplay: return L"monitor refresh unknown or under 50 Hz";
    }
    return L"unknown";
}

// Preserve the existing low-latency wait location; both locations use the
// renderer's same cap/waitable implementation.
enum class PresentWaitPoint { None, BeforeCapture, BeforeRender };

inline PresentWaitPoint ChoosePresentWaitPoint(int pacing, bool lowLatency)
{
    if (pacing != kPacingRefresh) return PresentWaitPoint::None;
    return lowLatency ? PresentWaitPoint::BeforeCapture : PresentWaitPoint::BeforeRender;
}

} // namespace NitLink
