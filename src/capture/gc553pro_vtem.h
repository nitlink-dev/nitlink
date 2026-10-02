#pragma once

#include "hdmi_source_vrr.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace NitLink {

// Existing AT_Get_HdmiRX_VTEM read-only request; command 0x7D, no input.
inline constexpr std::array<uint8_t, 9> kGc553ProVtemRequest{
    0xA1,0x06,0x00,0x00,0x7D,0x00,0x00,0x00,0xDC};

struct Gc553ProVtemInfo {
    bool valid = false; // transport envelope/length/LRC, not freshness
    std::array<uint8_t, 12> body{};
};

constexpr Gc553ProVtemInfo DecodeGc553ProVtemResponse(const uint8_t* data, size_t size) noexcept {
    // Actual GC553Pro: 4 envelope bytes + 12 body bytes + LRC. Reject
    // request echoes, stale A1 2A/A0 06 replies, zero/empty and corrupt data.
    Gc553ProVtemInfo result;
    if (!data || size != 17 || data[0] != 0xA1 || data[1] != 0x0E ||
        data[2] != 0 || data[3] != 0) return result;
    unsigned sum = 0;
    for (size_t i = 0; i < size; ++i) sum += data[i];
    if ((sum & 0xff) != 0) return result;
    result.valid = true;
    for (size_t i = 0; i < result.body.size(); ++i) result.body[i] = data[4 + i];
    return result;
}

constexpr HdmiSourceVrrState DecodeGc553ProSourceVrr(const Gc553ProVtemInfo& info) noexcept {
    // Empirically validated on GC553Pro: two reversible OFF -> ON_IDLE ->
    // ON_ACTIVE -> OFF_RETURN cycles, 30 clean samples/phase, 240/240 valid,
    // fixed 1920x1080 HDR10. This is NOT a confirmed vendor SDK VRR_EN layout.
    // ON_IDLE=1 means enabled/signaling, not instantaneous refresh variation.
    // The 59.99/60.00 Hz delta remains a potential timing confound in the
    // evidence; refresh, identity, HDR and HDMI TX/follow mode are not inputs.
    if (!info.valid) return HdmiSourceVrrState::Unknown;
    switch (info.body[3]) { // outer[7], validated empirical field only
    case 0: return HdmiSourceVrrState::Disabled;
    case 1: return HdmiSourceVrrState::Enabled;
    default: return HdmiSourceVrrState::Unknown;
    }
}

} // namespace NitLink
