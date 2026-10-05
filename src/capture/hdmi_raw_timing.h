#pragma once

#include <cstddef>
#include <cstdint>

namespace NitLink {

// HDMI input timing, never the Media Foundation capture-output format.
struct HdmiSourceTiming {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t refreshRateHz100 = 0;
    bool available = false;
    bool operator==(const HdmiSourceTiming&) const = default;
};

// Uses the same offsets as the existing Elgato 4K X decoder: active height
// at byte 10, active width at byte 12, and refresh frequency in Hz*100 at 18.
// The caller validates the transport envelope before using these fields.
constexpr HdmiSourceTiming ReadHdmiRawTimingFields(const uint8_t* data, size_t size) noexcept {
    if (!data || size < 20) return {};
    const auto u16 = [data](size_t offset) {
        return uint32_t(data[offset]) | (uint32_t(data[offset + 1]) << 8);
    };
    return {u16(12), u16(10), u16(18), u16(12) != 0 && u16(10) != 0};
}

// GC553Pro's captured response is 4 envelope bytes + 40 SDK bytes + checksum.
// Reject echoes, truncated packets and corrupt/implausible source timings.
constexpr HdmiSourceTiming DecodeGc553ProTimingResponse(const uint8_t* data, size_t size) noexcept {
    if (!data || size != 45 || data[0] != 0xa1 || data[1] != 0x2a ||
        data[2] != 0 || data[3] != 0) return {};
    unsigned checksum = 0;
    for (size_t i = 0; i < size; ++i) checksum += data[i];
    if ((checksum & 0xff) != 0) return {};
    const auto timing = ReadHdmiRawTimingFields(data, size);
    if (!timing.available || !timing.refreshRateHz100 ||
        timing.width > 16384 || timing.height > 16384) return {};
    return timing;
}

} // namespace NitLink
