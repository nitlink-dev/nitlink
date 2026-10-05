#include "capture/gc553pro_vtem.h"
#include "app/hdmi_source_display.h"
#include "app/config.h"
#include "app/capture_output_policy.h"
#include "app/presentation_state.h"
#include "app/source_cadence.h"

#include <array>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <tuple>

using namespace NitLink;
namespace {
size_t checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
using Frame = std::array<uint8_t, 17>;
// Exact clean hardware OFF and ON_IDLE/ON_ACTIVE replies; no hardware query.
constexpr Frame off{0xA1,0x0E,0,0,0,0,0,0,0,0,0,0,0,0,0x29,0,0x28};
constexpr Frame on {0xA1,0x0E,0,0,0,0,0,1,0,0,0,0,0,0,0x29,0,0x27};
void Checksum(Frame& frame) {
    unsigned sum = 0;
    for (size_t i = 0; i + 1 < frame.size(); ++i) sum += frame[i];
    frame.back() = static_cast<uint8_t>(0u - sum);
}
HdmiSourceVrrState Decode(const Frame& frame) {
    return DecodeGc553ProSourceVrr(DecodeGc553ProVtemResponse(frame.data(), frame.size()));
}
void ParserTests() {
    const auto info = DecodeGc553ProVtemResponse(on.data(), on.size());
    Check(info.valid && info.body.size() == 12, "17-byte response has 12-byte body");
    for (size_t i = 0; i < info.body.size(); ++i)
        Check(info.body[i] == on[4 + i], "body offsets preserved exactly");
    Check(Decode(off) == HdmiSourceVrrState::Disabled, "body[3]=0 disabled");
    Check(Decode(on) == HdmiSourceVrrState::Enabled, "idle and active body[3]=1 enabled");
    for (unsigned value = 0; value < 256; ++value) {
        auto frame = on;
        frame[7] = static_cast<uint8_t>(value);
        Checksum(frame);
        Check(Decode(frame) == (value == 0 ? HdmiSourceVrrState::Disabled :
              value == 1 ? HdmiSourceVrrState::Enabled : HdmiSourceVrrState::Unknown),
              "only empirical 0/1 mapping accepted");
    }
    // Other nonzero bytes cannot independently establish VRR enabled.
    for (size_t i = 4; i < 16; ++i) {
        if (i == 7) continue;
        auto frame = off;
        frame[i] ^= 0xff;
        Checksum(frame);
        Check(Decode(frame) == HdmiSourceVrrState::Disabled, "no arbitrary nonzero-body heuristic");
    }
    for (size_t size = 0; size < on.size(); ++size)
        Check(!DecodeGc553ProVtemResponse(on.data(), size).valid, "all truncated sizes rejected");
    auto oversized = std::array<uint8_t, 18>{};
    std::copy(on.begin(), on.end(), oversized.begin());
    Check(!DecodeGc553ProVtemResponse(oversized.data(), oversized.size()).valid, "oversized rejected");
    for (size_t i = 0; i < on.size(); ++i) {
        auto frame = on;
        frame[i] ^= 1;
        Check(Decode(frame) == HdmiSourceVrrState::Unknown, "all single-byte corruptions rejected");
    }
    for (size_t i = 0; i < 4; ++i) {
        auto frame = on;
        frame[i] ^= 1;
        Checksum(frame);
        Check(Decode(frame) == HdmiSourceVrrState::Unknown, "wrong envelope rejected with valid LRC");
    }
    std::array<uint8_t, 45> staleTiming{0xA1, 0x2A};
    Check(!DecodeGc553ProVtemResponse(staleTiming.data(), staleTiming.size()).valid,
          "stale RawTiming response rejected");
    Frame stalePadded{0xA1,0x2A}; Checksum(stalePadded);
    Check(Decode(stalePadded) == HdmiSourceVrrState::Unknown, "stale padded timing rejected");
    Frame ready{0xA0,0x06}; Checksum(ready);
    Check(Decode(ready) == HdmiSourceVrrState::Unknown, "A0 06 ready response rejected");
    Check(!DecodeGc553ProVtemResponse(kGc553ProVtemRequest.data(), kGc553ProVtemRequest.size()).valid,
          "request echo rejected");
    Check(Decode(Frame{}) == HdmiSourceVrrState::Unknown, "all-zero response rejected");
    Check(!DecodeGc553ProVtemResponse(nullptr, 17).valid, "null rejected");
    Check(DecodeGc553ProSourceVrr({}) == HdmiSourceVrrState::Unknown, "invalid transport never disabled");
}
void SettleTests() {
    using V = HdmiSourceVrrState;
    HdmiSourceVrrStateDebouncer monitor;
    V published = V::Unknown;
    monitor.Observe(V::Enabled, 100);
    Check(!monitor.PublishIfSettled(849, &published), "750 ms window required");
    monitor.Observe(V::Unknown, 500);
    Check(monitor.PublishIfSettled(850, &published) && published == V::Enabled,
          "unknown preserves original candidate deadline");
    Check(!monitor.PublishIfSettled(2000, &published), "transition published once");
    monitor.Observe(V::Unknown, 2500);
    Check(monitor.StableState() == V::Enabled, "invalid read retains enabled");
    monitor.Observe(V::Disabled, 3000);
    monitor.Observe(V::Enabled, 3400);
    Check(!monitor.PublishIfSettled(4000, &published), "valid stable sample cancels candidate");
    monitor.Observe(V::Disabled, 4100);
    monitor.Observe(V::Disabled, 4400);
    Check(!monitor.PublishIfSettled(4849, &published), "same candidate does not restart deadline");
    Check(monitor.PublishIfSettled(4850, &published) && published == V::Disabled, "valid disabled settles");
    HdmiSourceVrrStateDebouncer newSession;
    newSession.Observe(V::Unknown, 0);
    Check(!newSession.PublishIfSettled(10000, &published) && newSession.StableState() == V::Unknown,
          "new unsupported session remains unknown");
    HdmiSourceVrrStateDebouncer seeded(V::Enabled);
    seeded.Observe(V::Enabled, 0);
    Check(!seeded.PublishIfSettled(1000, &published), "initial probe does not generate redundant transition");
    seeded.Observe(V::Disabled, 10);
    Check(!seeded.PublishIfSettled(9, &published), "clock reversal does not publish");
}
void DisplayAndIsolationTests() {
    using V = HdmiSourceVrrState;
    const auto identity = GetEffectiveHdmiSourceLabel("switch2");
    const auto signal = FormatHdmiSourceTiming({1920,1080,6000,true});
    Check(ComposeHdmiWindowTitle(identity, signal, L"HDR", V::Enabled) ==
          L"NitLink - Switch 2 - 1920x1080 @ 60Hz [HDR] [VRR]", "enabled title composition");
    Check(ComposeHdmiPresenceSignal(signal, L"HDR", V::Enabled) ==
          L"1920x1080 @ 60Hz · HDR · VRR", "viewer signal composition");
    Check(ComposeHdmiPresenceState(identity, signal, L"HDR", V::Enabled) ==
          L"Switch 2 · 1920x1080 @ 60Hz · HDR · VRR", "game state composition");
    for (auto state : {V::Unknown, V::Disabled}) {
        Check(ComposeHdmiWindowTitle(identity, signal, L"HDR", state).find(L"VRR") == std::wstring::npos,
              "unknown/disabled title has no VRR");
        Check(ComposeHdmiPresenceSignal(signal, L"HDR", state).find(L"VRR") == std::wstring::npos,
              "unknown/disabled presence has no VRR");
    }
    Check(ComposeHdmiWindowTitle(identity, {}, {}, V::Enabled) == L"NitLink - Switch 2 [VRR]",
          "VRR supplies neither timing nor HDR");
    // Exercise the actual metadata application route against all existing
    // pacing/VSync/HDR choices and both negotiated capture subtypes.
    for (int pacing : {kPacingRefresh, kPacingCaptured, kPacingUnique})
    for (bool vsync : {false, true})
    for (bool hdrOutput : {false, true})
    for (auto actual : {NegotiatedCaptureFormatKind::NV12, NegotiatedCaptureFormatKind::P010})
    for (auto source : {Gc553ProSourceHdrState::Unknown, Gc553ProSourceHdrState::Sdr,
                        Gc553ProSourceHdrState::Hdr10Pq}) {
        Config config;
        config.presentPacing = pacing; config.vsync = vsync; config.hdrEnabled = hdrOutput;
        config.lowLatency = false; config.presentCapHz = 117; config.hdrAutoFromSource = true;
        CaptureFormatOverride capture{2560,1440,120,L"NV12",120,1};
        SourceCadence cadence;
        cadence.OnClassifiedFrame(true); cadence.OnClassifiedFrame(false);
        const auto snapshot = [&] {
            const auto policy = DecideGc553ProSourceOutputPolicy(actual, config.hdrEnabled,
                CaptureFormatPreference::Auto, config.hdrAutoFromSource, source);
            return std::tuple{config.presentPacing, config.vsync, config.lowLatency, config.presentCapHz,
                config.hdrEnabled, config.hdrAutoFromSource, config.nisEnabled, config.colorExpansion,
                capture.width, capture.height, capture.fps, capture.format, capture.fpsNumerator,
                capture.fpsDenominator, policy.desiredCaptureIsP010, policy.reopenCapture, policy.hdrRejected,
                RendererInputIsHdr10(true, config.hdrAutoFromSource, source, actual == NegotiatedCaptureFormatKind::P010),
                DecidePresentation(false, false, true, false), cadence.CadenceFrames(), cadence.ShouldHold()};
        };
        const auto baseline = snapshot();
        V current = V::Unknown;
        int refreshes = 0;
        std::wstring title, discord;
        const auto refresh = [&] {
            ++refreshes;
            title = ComposeHdmiWindowTitle(identity, signal, L"HDR", current);
            discord = ComposeHdmiPresenceSignal(signal, L"HDR", current);
        };
        for (auto observation : {V::Enabled, V::Unknown, V::Enabled, V::Disabled, V::Unknown}) {
            ApplyHdmiSourceVrrMetadata(current, observation, refresh);
            Check(snapshot() == baseline, "metadata cannot mutate capture/HDR/pacing/presentation policy");
        }
        Check(refreshes == 2 && current == V::Disabled && title.find(L"VRR") == std::wstring::npos &&
              discord.find(L"VRR") == std::wstring::npos, "only valid transitions refresh title and Discord");
        Check(ConfirmedHdmiSourceRange(true, true, true) == HdmiConfirmedRange::Hdr,
              "source HDR remains independent of output and metadata");
    }
}
}
int main() {
    try {
        ParserTests(); SettleTests(); DisplayAndIsolationTests();
        std::cout << checks << " GC553Pro source VRR production checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n'; return 1;
    }
}
