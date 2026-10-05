#pragma once

#include <cstdint>
#include <tuple>

namespace NitLink {

// Describes the raw capture texture's interpretation and its display pipeline.
// This is a compatibility check, never an input to capture or output policy.
struct CaptureFrameSemantics {
    std::uint32_t width = 0, height = 0, format = 0;
    std::uint32_t outputWidth = 0, outputHeight = 0, outputFormat = 0;
    bool sourceHdr10 = false, topDown = false, fullRange = false, limitedChroma = false;
    bool hdrOutput = false, bt709Matrix = false, postInput = false;
    float colorExpansion = 0, colorExpansionTarget = 0, aspectOverride = 0;
};

enum class CaptureFrameHoldReason {
    None, NeverPresented, CaptureSessionInvalidated, DeviceSwitch,
    ResourcesRecreated, TextureOverwritten, ResourceUnavailable, UnpresentedUpload,
    InputFormatChanged, InputSizeChanged, SourceColorChanged, OutputColorChanged,
    OutputSizeChanged, PostProcessChanged, ColorAdjustmentChanged
};

struct CaptureFrameHoldStatus {
    bool available = false;
    bool compatible = false;
    CaptureFrameHoldReason reason = CaptureFrameHoldReason::NeverPresented;
};

// No extra texture is retained: a receipt identifies which contents of the
// existing texture actually reached a successful Present. WRITE_DISCARD or a
// capture-session reset invalidates the receipt, independently of HasFrame.
class CaptureFrameHold {
public:
    bool HasPresentedFrame() const { return m_presented; }

    void Invalidate(CaptureFrameHoldReason reason) {
        m_presented = false;
        m_completedUploadSerial = 0;
        m_reason = reason;
    }

    void ObserveCompletedUpload(std::uint64_t uploadSerial) {
        m_completedUploadSerial = uploadSerial;
    }

    void ObservePresent(bool liveCaptureDraw, bool reachedBackbuffer, bool presentSucceeded,
                        std::uint64_t uploadSerial, const CaptureFrameSemantics& semantics) {
        if (!liveCaptureDraw || !reachedBackbuffer || !presentSucceeded ||
            !uploadSerial || uploadSerial != m_completedUploadSerial) return;
        m_presented = true;
        m_uploadSerial = uploadSerial;
        m_semantics = semantics;
        m_reason = CaptureFrameHoldReason::None;
    }

    CaptureFrameHoldStatus Inspect(bool resourcesReady, std::uint64_t uploadSerial,
                                   const CaptureFrameSemantics& now) const {
        using R = CaptureFrameHoldReason;
        if (!m_presented) return {false, false, m_reason};
        if (!resourcesReady) return {false, false, R::ResourceUnavailable};
        if (uploadSerial != m_uploadSerial) return {false, false, R::UnpresentedUpload};
        if (now.format != m_semantics.format) return {true, false, R::InputFormatChanged};
        if (std::tie(now.width, now.height) != std::tie(m_semantics.width, m_semantics.height))
            return {true, false, R::InputSizeChanged};
        if (std::tie(now.sourceHdr10, now.topDown, now.fullRange, now.limitedChroma) !=
            std::tie(m_semantics.sourceHdr10, m_semantics.topDown, m_semantics.fullRange, m_semantics.limitedChroma))
            return {true, false, R::SourceColorChanged};
        if (std::tie(now.hdrOutput, now.bt709Matrix, now.outputFormat) !=
            std::tie(m_semantics.hdrOutput, m_semantics.bt709Matrix, m_semantics.outputFormat))
            return {true, false, R::OutputColorChanged};
        if (std::tie(now.outputWidth, now.outputHeight, now.aspectOverride) !=
            std::tie(m_semantics.outputWidth, m_semantics.outputHeight, m_semantics.aspectOverride))
            return {true, false, R::OutputSizeChanged};
        if (now.postInput != m_semantics.postInput) return {true, false, R::PostProcessChanged};
        if (std::tie(now.colorExpansion, now.colorExpansionTarget) !=
            std::tie(m_semantics.colorExpansion, m_semantics.colorExpansionTarget))
            return {true, false, R::ColorAdjustmentChanged};
        return {true, true, R::None};
    }

private:
    bool m_presented = false;
    std::uint64_t m_uploadSerial = 0;
    std::uint64_t m_completedUploadSerial = 0;
    CaptureFrameSemantics m_semantics;
    CaptureFrameHoldReason m_reason = CaptureFrameHoldReason::NeverPresented;
};

} // namespace NitLink
