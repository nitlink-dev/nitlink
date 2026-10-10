#pragma once

#include <chrono>

namespace NitLink {

// Advance the release schedule, not Present-return + interval. Render work
// belongs inside the interval. If an entire slot was missed, start a fresh
// interval instead of issuing a burst of catch-up submissions.
// The caller supplies a positive interval from the active cap; Off and
// unknown-refresh decisions bypass this clock in WaitForFrameReady.
inline std::chrono::steady_clock::time_point AdvancePresentCapDeadline(
    std::chrono::steady_clock::time_point deadline,
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration interval)
{
    if (deadline == std::chrono::steady_clock::time_point{} || now >= deadline + interval)
        return now + interval;
    return deadline + interval;
}

} // namespace NitLink
