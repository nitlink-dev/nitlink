#include "p010_format_selector.h"

#include <climits>

namespace NitLink {

static uint64_t Area(const P010Candidate& candidate)
{
    return static_cast<uint64_t>(candidate.width) * candidate.height;
}

P010SelectionResult SelectGamingP010Candidate(
    const std::vector<P010Candidate>& candidates,
    uint32_t targetWidth,
    uint32_t targetHeight,
    uint32_t targetFps)
{
    P010SelectionResult result;
    if (candidates.empty()) return result;

    const bool targetDimensionsKnown = targetWidth > 0 && targetHeight > 0;
    if (targetDimensionsKnown) {
        if (targetFps > 0) {
            for (size_t i = 0; i < candidates.size(); ++i) {
                const auto& candidate = candidates[i];
                if (candidate.width == targetWidth &&
                    candidate.height == targetHeight &&
                    candidate.fps == targetFps) {
                    result.index = i;
                    result.reason = P010SelectionReason::ExactTarget;
                    return result;
                }
            }
        }

        size_t bestResolution = static_cast<size_t>(-1);
        uint32_t bestFpsDistance = UINT32_MAX;
        for (size_t i = 0; i < candidates.size(); ++i) {
            const auto& candidate = candidates[i];
            if (candidate.width != targetWidth ||
                candidate.height != targetHeight) {
                continue;
            }
            const uint32_t distance = targetFps > candidate.fps
                ? targetFps - candidate.fps : candidate.fps - targetFps;
            if (bestResolution == static_cast<size_t>(-1) ||
                distance < bestFpsDistance ||
                (distance == bestFpsDistance &&
                 candidate.fps > candidates[bestResolution].fps)) {
                bestResolution = i;
                bestFpsDistance = distance;
            }
        }
        if (bestResolution != static_cast<size_t>(-1)) {
            result.index = bestResolution;
            result.reason = P010SelectionReason::TargetResolution;
            return result;
        }
    }

    for (const auto& candidate : candidates) {
        if (candidate.fps >= 60) {
            result.usedHighFpsTier = true;
            break;
        }
    }

    size_t best = static_cast<size_t>(-1);
    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& candidate = candidates[i];
        if (result.usedHighFpsTier && candidate.fps < 60) continue;
        if (best == static_cast<size_t>(-1) ||
            Area(candidate) > Area(candidates[best]) ||
            (Area(candidate) == Area(candidates[best]) &&
             candidate.fps > candidates[best].fps)) {
            best = i;
        }
    }

    result.index = best;
    result.reason = result.usedHighFpsTier
        ? P010SelectionReason::HighFpsTier
        : P010SelectionReason::HighestResolutionFallback;
    return result;
}

const wchar_t* P010SelectionReasonText(P010SelectionReason reason)
{
    switch (reason) {
    case P010SelectionReason::ExactTarget:
        return L"exact target resolution/FPS";
    case P010SelectionReason::TargetResolution:
        return L"target resolution with closest native FPS";
    case P010SelectionReason::HighFpsTier:
        return L"native P010 >=60 FPS tier, then highest resolution";
    case P010SelectionReason::HighestResolutionFallback:
        return L"no native P010 >=60 FPS mode, highest resolution fallback";
    case P010SelectionReason::None:
        return L"no native P010 candidate";
    }
    return L"no native P010 candidate";
}

} // namespace NitLink
