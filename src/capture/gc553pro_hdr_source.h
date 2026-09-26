#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace NitLink {

enum class Gc553ProSourceHdrState : uint8_t {
    Unknown, Sdr, Hdr10Pq, OtherHdr,
};

struct Gc553ProHdrProbe {
    Gc553ProSourceHdrState state = Gc553ProSourceHdrState::Unknown;
    uint8_t eotf = 0xff;
};

// Require a valid new XU state to remain uncontradicted for 750 ms before
// publishing a source transition. Unknown reads preserve the candidate and
// never change the committed last-known-good state.
class Gc553ProSourceStateDebouncer {
public:
    static constexpr int64_t SettleWindowMs = 750;

    explicit constexpr Gc553ProSourceStateDebouncer(
        Gc553ProSourceHdrState initial = Gc553ProSourceHdrState::Unknown) noexcept
        : m_stable(initial) {}

    constexpr void Observe(Gc553ProSourceHdrState sample,
                           int64_t nowMs) noexcept {
        if (sample == Gc553ProSourceHdrState::Unknown) {
            // A failed / incomplete XU read is not evidence that the source
            // changed. Preserve the pending valid candidate and let its
            // original settle timer continue; Unknown is never a candidate.
            return;
        }
        if (sample == m_stable) {
            // A valid observation of the committed state contradicts the
            // pending candidate, so cancel it.
            m_candidate = Gc553ProSourceHdrState::Unknown;
            m_candidateSinceMs = 0;
            return;
        }
        if (sample != m_candidate) {
            m_candidate = sample;
            m_candidateSinceMs = nowMs;
        }
    }

    constexpr bool PublishIfSettled(
        int64_t nowMs, Gc553ProSourceHdrState* outCommitted) noexcept {
        if (m_candidate == Gc553ProSourceHdrState::Unknown ||
            nowMs < m_candidateSinceMs ||
            nowMs - m_candidateSinceMs < SettleWindowMs) return false;

        m_stable = m_candidate;
        m_candidate = Gc553ProSourceHdrState::Unknown;
        m_candidateSinceMs = 0;
        if (outCommitted) *outCommitted = m_stable;
        return true;
    }

    constexpr Gc553ProSourceHdrState StableState() const noexcept {
        return m_stable;
    }

private:
    Gc553ProSourceHdrState m_stable = Gc553ProSourceHdrState::Unknown;
    Gc553ProSourceHdrState m_candidate = Gc553ProSourceHdrState::Unknown;
    int64_t m_candidateSinceMs = 0;
};

// GC553Pro's observed mailbox record contains a DRM InfoFrame header at
// [4..6] and the EOTF at [7]. It is not the 4K X byte[4] HDR predicate.
Gc553ProHdrProbe DecodeGc553ProHdrResponse(const uint8_t* data,
                                              size_t size) noexcept;

// Create/use/destroy on the same thread. Keeps the DirectShow filter open
// between polls; COM references are released before CoUninitialize.
class Gc553ProHdrReader {
public:
    explicit Gc553ProHdrReader(const std::wstring& deviceName);
    ~Gc553ProHdrReader();
    Gc553ProHdrReader(const Gc553ProHdrReader&) = delete;
    Gc553ProHdrReader& operator=(const Gc553ProHdrReader&) = delete;
    Gc553ProHdrProbe Read();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace NitLink
