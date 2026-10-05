#pragma once

#include <cstdint>

namespace NitLink {

// Read-only HDMI INPUT VRR enabled/signaling state, including idle scenes.
// It does not describe instantaneous refresh variation or display-side VRR.
enum class HdmiSourceVrrState : uint8_t { Unknown, Disabled, Enabled };

// Same 750 ms settle/last-valid behavior as the existing source HDR monitor,
// in its own state machine. Unknown is never evidence of VRR being disabled.
class HdmiSourceVrrStateDebouncer {
public:
    static constexpr int64_t SettleWindowMs = 750;
    explicit constexpr HdmiSourceVrrStateDebouncer(
        HdmiSourceVrrState initial = HdmiSourceVrrState::Unknown) noexcept : m_stable(initial) {}

    constexpr void Observe(HdmiSourceVrrState sample, int64_t nowMs) noexcept {
        if (sample != HdmiSourceVrrState::Disabled && sample != HdmiSourceVrrState::Enabled) return;
        if (sample == m_stable) {
            m_candidate = HdmiSourceVrrState::Unknown;
            m_candidateSinceMs = 0;
        } else if (sample != m_candidate) {
            m_candidate = sample;
            m_candidateSinceMs = nowMs;
        }
    }
    constexpr bool PublishIfSettled(int64_t nowMs, HdmiSourceVrrState* outState) noexcept {
        if (m_candidate == HdmiSourceVrrState::Unknown || nowMs < m_candidateSinceMs ||
            nowMs - m_candidateSinceMs < SettleWindowMs) return false;
        m_stable = m_candidate;
        m_candidate = HdmiSourceVrrState::Unknown;
        m_candidateSinceMs = 0;
        if (outState) *outState = m_stable;
        return true;
    }
    constexpr HdmiSourceVrrState StableState() const noexcept { return m_stable; }

private:
    HdmiSourceVrrState m_stable;
    HdmiSourceVrrState m_candidate = HdmiSourceVrrState::Unknown;
    int64_t m_candidateSinceMs = 0;
};

} // namespace NitLink
