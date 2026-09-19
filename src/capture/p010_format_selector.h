#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace NitLink {

struct P010Candidate {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
};

enum class P010SelectionReason {
    None,
    ExactTarget,
    TargetResolution,
    HighFpsTier,
    HighestResolutionFallback,
};

struct P010SelectionResult {
    size_t index = static_cast<size_t>(-1);
    bool usedHighFpsTier = false;
    P010SelectionReason reason = P010SelectionReason::None;
};

P010SelectionResult SelectGamingP010Candidate(
    const std::vector<P010Candidate>& candidates,
    uint32_t targetWidth,
    uint32_t targetHeight,
    uint32_t targetFps);

const wchar_t* P010SelectionReasonText(P010SelectionReason reason);

} // namespace NitLink
