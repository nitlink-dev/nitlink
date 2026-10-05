#pragma once

#include "presentation_state.h"
#include "renderer/capture_frame_hold.h"

namespace NitLink {

enum class ResyncFrameMode { Live, Cached, Fallback };

// Called only after the existing state/recovery and pacing decisions. A retained
// frame cannot promote WaitingForCapture/NoSignal or delay live Capture.
inline ResyncFrameMode DecideResyncFrameMode(PresentationState state,
                                            const CaptureFrameHoldStatus& cache) {
    if (state == PresentationState::Capture) return ResyncFrameMode::Live;
    if (state == PresentationState::SignalResync && cache.available && cache.compatible)
        return ResyncFrameMode::Cached;
    return ResyncFrameMode::Fallback;
}

inline float ResyncStatusBackgroundOpacity(ResyncFrameMode mode, bool pictureDrawn) {
    return mode == ResyncFrameMode::Cached && pictureDrawn ? 0.45f : 1.0f;
}

} // namespace NitLink
