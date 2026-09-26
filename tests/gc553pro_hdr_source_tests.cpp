#include "capture/gc553pro_hdr_source.h"
#include "app/capture_output_policy.h"

#include <array>
#include <iostream>

using namespace NitLink;

int main() {
    // The two byte-identical HDR and two byte-identical SDR live responses
    // differed at EOTF [7] and four opaque trailer bytes.
    std::array<uint8_t, 37> hdr{
        0xA1,0x22,0x00,0x00,0x87,0x01,0x1A,0x02};
    hdr[34] = 0xDF; hdr[35] = 0x06; hdr[36] = 0xB4;
    auto sdr = hdr;
    sdr[7] = 0x00;
    sdr[34] = 0x7F; sdr[35] = 0x05; sdr[36] = 0x17;
    if (DecodeGc553ProHdrResponse(hdr.data(), hdr.size()).state !=
        Gc553ProSourceHdrState::Hdr10Pq) return 1;
    if (DecodeGc553ProHdrResponse(sdr.data(), sdr.size()).state !=
        Gc553ProSourceHdrState::Sdr) return 2;
    if (DecodeGc553ProHdrResponse(hdr.data(), 36).state !=
        Gc553ProSourceHdrState::Unknown) return 3;
    hdr[4] = 0x00;
    if (DecodeGc553ProHdrResponse(hdr.data(), hdr.size()).state !=
        Gc553ProSourceHdrState::Unknown) return 4;
    hdr[4] = 0x87;
    hdr[6] = 0x19;
    if (DecodeGc553ProHdrResponse(hdr.data(), hdr.size()).state !=
        Gc553ProSourceHdrState::Unknown) return 5;
    hdr[6] = 0x1A;
    hdr[8] = 0x01;
    if (DecodeGc553ProHdrResponse(hdr.data(), hdr.size()).state !=
        Gc553ProSourceHdrState::Unknown) return 17;
    hdr[8] = 0x00;
    for (uint8_t eotf : {uint8_t{1}, uint8_t{3}}) {
        hdr[7] = eotf;
        if (DecodeGc553ProHdrResponse(hdr.data(), hdr.size()).state !=
            Gc553ProSourceHdrState::OtherHdr) return 6;
    }
    hdr[7] = 0x04;
    if (DecodeGc553ProHdrResponse(hdr.data(), hdr.size()).state !=
        Gc553ProSourceHdrState::Unknown) return 7;
    if (KeepLastGc553ProSourceState(Gc553ProSourceHdrState::Hdr10Pq,
            Gc553ProSourceHdrState::Unknown) !=
        Gc553ProSourceHdrState::Hdr10Pq) return 8;
    if (KeepLastGc553ProSourceState(Gc553ProSourceHdrState::Unknown,
            Gc553ProSourceHdrState::Sdr) !=
        Gc553ProSourceHdrState::Sdr) return 9;

    constexpr auto autoPolicy = [](NegotiatedCaptureFormatKind format,
                                    Gc553ProSourceHdrState state) {
        return DecideGc553ProSourceOutputPolicy(
            format, false, CaptureFormatPreference::Auto, true, state);
    };
    if (!autoPolicy(NegotiatedCaptureFormatKind::NV12,
                    Gc553ProSourceHdrState::Hdr10Pq).reopenCapture) return 10;
    if (autoPolicy(NegotiatedCaptureFormatKind::P010,
                   Gc553ProSourceHdrState::Sdr).desiredCaptureIsP010) return 11;
    if (!autoPolicy(NegotiatedCaptureFormatKind::P010,
                    Gc553ProSourceHdrState::Sdr).reopenCapture) return 12;
    if (autoPolicy(NegotiatedCaptureFormatKind::P010,
                   Gc553ProSourceHdrState::OtherHdr).reopenCapture) return 13;
    if (!autoPolicy(NegotiatedCaptureFormatKind::P010,
                    Gc553ProSourceHdrState::Unknown).desiredCaptureIsP010) return 14;
    if (DecideGc553ProSourceOutputPolicy(
            NegotiatedCaptureFormatKind::NV12, false,
            CaptureFormatPreference::ManualNV12, true,
            Gc553ProSourceHdrState::Hdr10Pq).reopenCapture) return 15;
    if (DecideGc553ProSourceOutputPolicy(
            NegotiatedCaptureFormatKind::P010, false,
            CaptureFormatPreference::ManualP010, true,
            Gc553ProSourceHdrState::Sdr).reopenCapture) return 16;
    // SDR startup stays NV12 even if HDR output is preferred. Source HDR10
    // selects P010, and a confirmed return to SDR selects NV12 again.
    if (autoPolicy(NegotiatedCaptureFormatKind::NV12,
                   Gc553ProSourceHdrState::Sdr).desiredCaptureIsP010) return 18;
    if (DecideGc553ProSourceOutputPolicy(
            NegotiatedCaptureFormatKind::NV12, true,
            CaptureFormatPreference::Auto, true,
            Gc553ProSourceHdrState::Sdr).reopenCapture) return 19;
    if (!autoPolicy(NegotiatedCaptureFormatKind::NV12,
                    Gc553ProSourceHdrState::Hdr10Pq).desiredCaptureIsP010) return 20;
    if (autoPolicy(NegotiatedCaptureFormatKind::P010,
                   Gc553ProSourceHdrState::Hdr10Pq).reopenCapture) return 21;
    if (!autoPolicy(NegotiatedCaptureFormatKind::P010,
                    Gc553ProSourceHdrState::OtherHdr).desiredCaptureIsP010) return 22;
    if (autoPolicy(NegotiatedCaptureFormatKind::NV12,
                   Gc553ProSourceHdrState::OtherHdr).reopenCapture) return 23;
    // Source EOTF, capture subtype and output preference are independent.
    if (RendererInputIsHdr10(true, true, false, true)) return 24;
    if (!RendererInputIsHdr10(true, true, true, false)) return 25;
    if (!RendererInputIsHdr10(false, true, false, true)) return 26;
    if (!RendererInputIsHdr10(true, false, false, true)) return 27;
    if (RendererInputIsHdr10(true, true, false, false)) return 28;

    Gc553ProSourceStateDebouncer stateDebouncer(
        Gc553ProSourceHdrState::Hdr10Pq);
    Gc553ProSourceHdrState committed{};
    stateDebouncer.Observe(Gc553ProSourceHdrState::Sdr, 1000);
    if (stateDebouncer.PublishIfSettled(1749, &committed)) return 39;
    if (stateDebouncer.StableState() != Gc553ProSourceHdrState::Hdr10Pq) return 40;
    // Unknown leaves the candidate and its original deadline untouched, and
    // cannot create a candidate or set the published source state itself.
    stateDebouncer.Observe(Gc553ProSourceHdrState::Unknown, 1750);
    if (stateDebouncer.StableState() != Gc553ProSourceHdrState::Hdr10Pq) return 41;
    if (!stateDebouncer.PublishIfSettled(1750, &committed) ||
        committed != Gc553ProSourceHdrState::Sdr) return 42;
    Gc553ProSourceStateDebouncer unknownOnly;
    unknownOnly.Observe(Gc553ProSourceHdrState::Unknown, 5000);
    if (unknownOnly.PublishIfSettled(6000, &committed) ||
        unknownOnly.StableState() != Gc553ProSourceHdrState::Unknown) return 43;

    // Only a valid opposite state cancels/restarts the timer. An unknown gap
    // does not reset it, and the reverse SDR -> HDR transition uses the same
    // 750 ms settle window.
    stateDebouncer.Observe(Gc553ProSourceHdrState::Hdr10Pq, 6000);
    stateDebouncer.Observe(Gc553ProSourceHdrState::Unknown, 6300);
    stateDebouncer.Observe(Gc553ProSourceHdrState::Sdr, 6400);
    if (stateDebouncer.StableState() != Gc553ProSourceHdrState::Sdr ||
        stateDebouncer.PublishIfSettled(7000, &committed)) return 44;
    stateDebouncer.Observe(Gc553ProSourceHdrState::Hdr10Pq, 7000);
    if (stateDebouncer.PublishIfSettled(7749, &committed)) return 45;
    stateDebouncer.Observe(Gc553ProSourceHdrState::Unknown, 7750);
    if (!stateDebouncer.PublishIfSettled(7750, &committed) ||
        committed != Gc553ProSourceHdrState::Hdr10Pq) return 46;

    Gc553ProSourceFrameSync frameSync;
    frameSync.Begin(100);
    if (!frameSync.Hold(99) || !frameSync.Hold(100) ||
        !frameSync.Hold(0) || frameSync.Ready(100) ||
        !frameSync.Ready(101)) return 29;
    frameSync.Begin(200); // A newer EOTF update supersedes the pending one.
    if (!frameSync.Hold(150) || !frameSync.Ready(201)) return 30;
    frameSync.Reset();
    if (frameSync.pending || frameSync.Hold(0) || frameSync.Ready(201)) return 31;

    constexpr auto showSwitchToast = [](bool gc, bool autoSource,
                                        CaptureFormatPreference preference,
                                        NegotiatedCaptureFormatKind actual,
                                        Gc553ProSourceHdrState source,
                                        bool requestingP010) {
        return IsGc553ProAutoNv12ToP010Reopen(
            gc, autoSource, preference, actual, source, requestingP010);
    };
    if (!showSwitchToast(true, true, CaptureFormatPreference::Auto,
                         NegotiatedCaptureFormatKind::NV12,
                         Gc553ProSourceHdrState::Hdr10Pq, true)) return 32;
    if (showSwitchToast(true, true, CaptureFormatPreference::Auto,
                        NegotiatedCaptureFormatKind::P010,
                        Gc553ProSourceHdrState::Hdr10Pq, true)) return 33;
    if (showSwitchToast(true, true, CaptureFormatPreference::Auto,
                        NegotiatedCaptureFormatKind::NV12,
                        Gc553ProSourceHdrState::Sdr, true)) return 34;
    if (showSwitchToast(true, true, CaptureFormatPreference::ManualP010,
                        NegotiatedCaptureFormatKind::NV12,
                        Gc553ProSourceHdrState::Hdr10Pq, true)) return 35;
    if (showSwitchToast(false, true, CaptureFormatPreference::Auto,
                        NegotiatedCaptureFormatKind::NV12,
                        Gc553ProSourceHdrState::Hdr10Pq, true)) return 36;
    if (showSwitchToast(true, false, CaptureFormatPreference::Auto,
                        NegotiatedCaptureFormatKind::NV12,
                        Gc553ProSourceHdrState::Hdr10Pq, true)) return 37;
    if (showSwitchToast(true, true, CaptureFormatPreference::Auto,
                        NegotiatedCaptureFormatKind::NV12,
                        Gc553ProSourceHdrState::Hdr10Pq, false)) return 38;
    std::cout << "GC553Pro HDR source tests passed\n";
    return 0;
}
