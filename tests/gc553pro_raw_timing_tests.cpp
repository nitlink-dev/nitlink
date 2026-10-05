#include "capture/hdmi_raw_timing.h"
#include "app/hdmi_source_display.h"

#include <array>
#include <iostream>
#include <stdexcept>

using namespace NitLink;
namespace {
size_t checks = 0;
void Check(bool condition, const char* label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}
void Checksum(std::array<uint8_t, 45>& packet) {
    unsigned sum = 0;
    for (size_t i = 0; i + 1 < packet.size(); ++i) sum += packet[i];
    packet.back() = static_cast<uint8_t>(0u - sum);
}
}
int main() {
    try {
        // Existing GC553Pro capture: gc553pro-control-transactions.csv,
        // response frame 88 / original USB frame 12079. No device query here.
        std::array<uint8_t, 45> packet{
            0xA1,0x2A,0x00,0x00,0x30,0x01,0x65,0x04,0x98,0x08,0x38,0x04,
            0x80,0x07,0x29,0x00,0xC0,0x00,0x70,0x17,0x5E,0x1A,0x10,0x05,
            0x00,0x2C,0x00,0x01,0x01,0x00,0x02,0x09,0x06,0x00,0xDF,0x06,
            0x20,0xEE,0xD9,0x08,0xCB,0xF5,0x24,0x14,0x30};
        const auto actual = DecodeGc553ProTimingResponse(packet.data(), packet.size());
        Check(actual.available && actual.width == 1920 && actual.height == 1080 &&
              actual.refreshRateHz100 == 6000, "captured 0x37 response is 1080p60 HDMI IN");
        Check(FormatHdmiSourceTiming(actual) == L"1920x1080 @ 60Hz", "captured signal label");
        // A second captured response (original frame 12333) differs only in
        // opaque trailer bytes; no decoder depends on those bytes.
        auto second = packet;
        second[40] = 0xDF; second[41] = 0x0F; second[42] = 0x25;
        second[43] = 0x14; second[44] = 0x01;
        Check(DecodeGc553ProTimingResponse(second.data(), second.size()) == actual,
              "second captured timing decodes identically");
        Check(!DecodeGc553ProTimingResponse(nullptr, 45).available, "null response rejected");
        for (size_t size = 0; size < packet.size(); ++size)
            Check(!DecodeGc553ProTimingResponse(packet.data(), size).available,
                  "every truncated length rejected");
        auto corrupt = packet;
        corrupt.back() ^= 1;
        Check(!DecodeGc553ProTimingResponse(corrupt.data(), corrupt.size()).available,
              "bad checksum rejected");
        for (size_t offset = 0; offset < 4; ++offset) {
            corrupt = packet;
            corrupt[offset] ^= 1;
            Checksum(corrupt);
            Check(!DecodeGc553ProTimingResponse(corrupt.data(), corrupt.size()).available,
                  "wrong envelope rejected despite valid checksum");
        }
        corrupt = packet;
        corrupt[12] = corrupt[13] = 0;
        Checksum(corrupt);
        Check(!DecodeGc553ProTimingResponse(corrupt.data(), corrupt.size()).available,
              "zero active size is unavailable");
        corrupt = packet;
        corrupt[18] = corrupt[19] = 0;
        Checksum(corrupt);
        Check(!DecodeGc553ProTimingResponse(corrupt.data(), corrupt.size()).available,
              "unknown frequency not invented");
        corrupt = packet;
        corrupt[12] = corrupt[13] = 0xff;
        Checksum(corrupt);
        Check(!DecodeGc553ProTimingResponse(corrupt.data(), corrupt.size()).available,
              "implausible active width rejected");
        corrupt = packet;
        corrupt[10] = corrupt[11] = 0xff;
        Checksum(corrupt);
        Check(!DecodeGc553ProTimingResponse(corrupt.data(), corrupt.size()).available,
              "implausible active height rejected");
        auto fractional = packet;
        fractional[18] = 0x6A; fractional[19] = 0x17; // 59.94 Hz
        Checksum(fractional);
        Check(FormatHdmiSourceTiming(DecodeGc553ProTimingResponse(
                  fractional.data(), fractional.size())) == L"1920x1080 @ 59.94Hz",
              "centihertz precision preserved");
        // Existing 4K X extraction and integer rounding remain identical.
        const auto shared = ReadHdmiRawTimingFields(fractional.data(), 20);
        Check(shared.width == 1920 && shared.height == 1080 &&
              (shared.refreshRateHz100 + 50) / 100 == 60,
              "shared 4K X fields preserve existing rounding");
        Check(!ReadHdmiRawTimingFields(packet.data(), 19).available,
              "shared decoder bounds check");
        Check(FormatHdmiSourceTiming({}).empty(),
              "unavailable source timing has no capture-output fallback");
        std::cout << checks << " GC553Pro raw timing checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
