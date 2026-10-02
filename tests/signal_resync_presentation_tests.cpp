#include "app/presentation_state.h"
#include "app/resync_frame_presentation.h"
#include "app/hdmi_source_display.h"
#include "app/capture_output_policy.h"
#include "app/config.h"
#include "app/localization.h"
#include "app/source_cadence.h"
#include "capture/frame_buffer.h"
#include "capture/placeholder_detector.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <vector>

using namespace NitLink;
namespace {
size_t checks = 0;
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
PresentationState AtGap(std::int64_t gapMs, bool stable = true, bool noSignal = false,
                        bool captureReady = true, bool formatTransition = false) {
    return DecidePresentation(noSignal, false, captureReady, formatTransition,
        ShouldShowSignalResync(true, stable, noSignal, gapMs, formatTransition));
}
void PresentationTests() {
    Check(kCaptureReacquireDebounceMs == 250, "reuse existing 250 ms debounce");
    for (auto gap : {-1, 0, 16, 33, 100, 249})
        Check(AtGap(gap) == PresentationState::Capture, "short frame misses retain Capture");
    Check(AtGap(250) == PresentationState::SignalResync, "stable capture enters resync at 250 ms");
    Check(AtGap(800) == PresentationState::SignalResync, "ongoing frame loss retains resync");
    Check(AtGap(0) == PresentationState::Capture, "first valid recovery frame removes resync immediately");
    Check(AtGap(250, true, false, false, true) == PresentationState::SignalResync,
          "existing HDR format recovery uses generic resync");
    for (auto gap : {0, 16, 249}) {
        Check(AtGap(gap, true, false, false, true) == PresentationState::SignalResync,
              "known capture transition does not wait for fallback debounce");
        Check(ShouldShowSignalResync(false, true, false, gap, true),
              "known backend transition is independent of frame-gap eligibility");
    }
    for (auto gap : {0, 250, 1500, 10000}) {
        Check(AtGap(gap, false, false, false) == PresentationState::WaitingForCapture,
              "startup/new capture device without stable history remains WaitingForCapture");
        Check(!ShouldShowSignalResync(true, false, false, gap), "startup cannot arm SignalResync");
    }
    Check(AtGap(10000, false, false, false, true) == PresentationState::Transition &&
          std::wstring(CaptureStatusLocalizationKey(PresentationState::Transition, true)) == L"overlay.waitingForSource",
          "startup format rebuild keeps ordinary waiting wording");
    Check(!ShouldShowSignalResync(false, true, false, 10000), "unrelated device has no new frame-loss hint");
    Check(!ShouldShowSignalResync(false, true, true, 0, true), "NoSignal suppresses known transition too");
    Check(!ShouldShowSignalResync(false, false, false, 0, true), "known transition never claims startup resync");
    for (auto gap : {16, 249, 250, 1000}) {
        Check(DecidePresentation(false, false, true, false,
              ShouldShowSignalResync(false, true, false, gap)) == PresentationState::Capture,
              "ineligible backend delivery gaps retain existing presentation");
    }
    for (auto gap : {16, 249}) {
        Check(AtGap(gap) == PresentationState::Capture && AtGap(0) == PresentationState::Capture,
              "short gap recovery never waits for 250 ms or a minimum overlay duration");
    }
    Check(AtGap(1500, false, true, false) == PresentationState::NoSignal, "persistent startup loss is NoSignal");
    Check(AtGap(6000, true, true) == PresentationState::NoSignal, "persistent loss obeys existing NoSignal decision");
    Check(DecidePresentation(false, true, true, true, true) == PresentationState::NoSignal,
          "confirmed placeholder/debounced loss has priority over resync");
    Check(DecidePresentation(true, false, false, true, true) == PresentationState::NoSignal,
          "NoSignal latch remains authoritative through rebuild");
    Check(AtGap(0) == PresentationState::Capture, "accepted real recovery may clear NoSignal and resync");
    Check(!CaptureStatusLocalizationKey(PresentationState::NoSignal, true) &&
          !CaptureStatusLocalizationKey(PresentationState::Capture, true), "status hint cannot replace NoSignal/capture renderer");
    Check(std::wstring(CaptureStatusLocalizationKey(PresentationState::WaitingForCapture, false)) ==
          L"overlay.initializingCapture", "capture initialization retains its own wording");
    auto& locale = Localization::Instance();
    const auto previous = locale.Preference();
    locale.SetPreference("zh-TW");
    Check(locale.Get(CaptureStatusLocalizationKey(PresentationState::SignalResync, true)) ==
          L"正在重新同步訊號…", "exact Traditional Chinese resync caption");
    locale.SetPreference("en-US");
    Check(locale.Get(CaptureStatusLocalizationKey(PresentationState::SignalResync, false)) ==
          L"Resynchronizing signal…", "generic English resync caption");
    locale.SetPreference(previous);
}
void EpisodeRegressionTests() {
    using S = PresentationState;
    std::vector<S> stateChanges;
    const auto expect = [&](std::int64_t now, std::int64_t lastGood,
                            bool transition, bool ready, bool noSignal,
                            S expected, const char* message) {
        const auto state = DecidePresentation(noSignal, false, ready, transition,
            ShouldShowSignalResync(true, true, noSignal, now - lastGood, transition));
        Check(state == expected, message);
        if (stateChanges.empty() || stateChanges.back() != state)
            stateChanges.push_back(state);
    };
    expect(1000,1000,false,true,false,S::Capture, "initial presentable Capture");
    expect(1016,1000,true,false,false,S::SignalResync,
           "actual format reopen starts resync before the fallback threshold");
    expect(1249,1000,true,false,false,S::SignalResync,
           "continuous immediate transition stays in resync before 250 ms");
    expect(1250,1000,true,false,false,S::SignalResync,
           "same-transition fallback threshold does not change immediate resync");
    expect(1500,1000,true,false,false,S::SignalResync,
           "ongoing immediate transition remains one resync presentation");
    expect(1600,1000,false,false,false,S::SignalResync,
           "immediate-to-fallback handoff has no intervening waiting or Capture");
    expect(1700,1700,false,true,false,S::Capture,
           "presentable recovery restores Capture immediately");
    expect(1716,1716,false,true,false,S::Capture, "fresh frames keep recovered Capture");
    expect(1732,1732,false,true,false,S::Capture, "another fresh frame keeps recovered Capture");
    expect(1982,1732,false,true,false,S::SignalResync,
           "a new interruption after real recovery begins another resync presentation");
    expect(6000,1732,true,false,true,S::NoSignal,
           "authoritative NoSignal overrides immediate and fallback resync");
    Check(stateChanges == std::vector<S>{S::Capture,S::SignalResync,S::Capture,
                                        S::SignalResync,S::NoSignal},
          "immediate plus fallback forms one continuous episode; real recovery separates the next episode");
    Check(AtGap(0,true,false,false) == S::WaitingForCapture,
          "accepted frame without a usable GPU surface does not claim recovered Capture");
    Check(AtGap(250,true,false,false) == S::SignalResync,
          "continued lack of a presentable frame retains the eligible fallback");
    Check(DecidePresentation(false,false,true,false,
              ShouldShowSignalResync(false,true,false,4000)) == S::Capture,
          "ineligible backend cannot enter the no-valid-frame fallback");
}
void ValidBlackAndInvalidFrames() {
    constexpr uint32_t width = 16, height = 16;
    std::vector<uint8_t> black(width * height * 3 / 2, 0);
    std::fill(black.begin() + width * height, black.end(), uint8_t{128});
    FrameBuffer frames(width, height, width);
    Check(frames.IsValid(), "real production frame buffer initialized");
    PlaceholderDetector detector;
    using Format = PlaceholderDetector::CaptureFormatKind;
    using Family = PlaceholderDetector::PlaceholderDeviceFamily;
    for (int i = 0; i < 150; ++i) {
        frames.Write(black.data(), static_cast<uint32_t>(black.size()), i * 160000,
                     1'000'000'000 + i * 16'000'000LL);
        FrameBuffer::FrameData frame;
        Check(frames.Read(frame), "static legal black continues to supply fresh frames");
        Check(!PlaceholderDetector::IsInvalidTransitionalFrame(
            frame.data, frame.size, width, height, Format::NV12), "legal neutral chroma is not invalid data");
        Check(!PlaceholderDetector::IsGc553ProNeutralTransitionalFrame(
            frame.data, frame.size, width, height, Format::NV12, Family::AverMediaGC553Pro, false, true),
            "full-range game black is not a transitional sample");
        const auto classification = detector.Process(PlaceholderDetector::Compute(
            frame.data, frame.size, width, height, Format::NV12), Format::NV12, Family::AverMediaGC553Pro);
        Check(PlaceholderDetector::ShouldUploadCaptureFrame(classification), "legal game black remains Real");
        Check(AtGap(0) == PresentationState::Capture, "every valid duplicate/black frame prevents resync");
    }
    Check(!frames.IsSignalActive(), "content-based inactivity can coexist with valid frame delivery");
    // Production presentation must use valid-frame delivery, never the hash
    // activity predicate above or a sampled all-black image heuristic.
    FrameBuffer::FrameData frame;
    Check(!frames.Read(frame) && AtGap(16) == PresentationState::Capture,
          "one empty frame-buffer read cannot trigger resync");
    std::fill(black.begin(), black.end(), uint8_t{0});
    frames.Write(black.data(), static_cast<uint32_t>(black.size()), 0);
    Check(frames.Read(frame) && PlaceholderDetector::IsInvalidTransitionalFrame(
        frame.data, frame.size, width, height, Format::NV12), "structurally invalid sample remains non-uploadable");
    Check(AtGap(250) == PresentationState::SignalResync, "continued absence of usable frames permits resync");
}
void MetadataTests() {
    using H = Gc553ProSourceHdrState;
    using V = HdmiSourceVrrState;
    // Existing validated production response fixtures, not a device probe.
    std::array<uint8_t,37> hdr{0xA1,0x22,0,0,0x87,0x01,0x1A,0x02};
    hdr[34]=0xDF; hdr[35]=0x06; hdr[36]=0xB4;
    auto sdr = hdr;
    sdr[7]=0; sdr[34]=0x7F; sdr[35]=0x05; sdr[36]=0x17;
    const std::array<uint8_t,17> on{0xA1,0x0E,0,0,0,0,0,1,0,0,0,0,0,0,0x29,0,0x27};
    const std::array<uint8_t,17> off{0xA1,0x0E,0,0,0,0,0,0,0,0,0,0,0,0,0x29,0,0x28};
    auto sourceHdr = DecodeGc553ProHdrResponse(hdr.data(), hdr.size()).state;
    auto sourceVrr = DecodeGc553ProSourceVrr(DecodeGc553ProVtemResponse(on.data(), on.size()));
    Check(sourceHdr == H::Hdr10Pq && sourceVrr == V::Enabled, "initial confirmed HDR and VRR");
    const auto label = [&] {
        return ComposeHdmiWindowTitle(L"Switch 2", L"1920x1080 @ 60Hz",
                                      sourceHdr == H::Hdr10Pq ? L"HDR" : L"SDR", sourceVrr);
    };
    const auto initialTitle = label();
    Gc553ProSourceStateDebouncer hdrMonitor(sourceHdr);
    HdmiSourceVrrStateDebouncer vrrMonitor(sourceVrr);
    int labelChanges = 0;
    for (auto gap : {16, 250, 500, 1000}) {
        const auto invalidHdr = DecodeGc553ProHdrResponse(nullptr, 0).state;
        const auto invalidVrr = DecodeGc553ProSourceVrr(DecodeGc553ProVtemResponse(nullptr, 0));
        hdrMonitor.Observe(invalidHdr, gap); vrrMonitor.Observe(invalidVrr, gap);
        sourceHdr = KeepLastGc553ProSourceState(sourceHdr, invalidHdr);
        ApplyHdmiSourceVrrMetadata(sourceVrr, invalidVrr, [&] { ++labelChanges; });
        Check(sourceHdr == H::Hdr10Pq && sourceVrr == V::Enabled && label() == initialTitle,
              "resync preserves last-valid HDR/VRR and title suffixes");
        Check(AtGap(gap) == (gap < 250 ? PresentationState::Capture : PresentationState::SignalResync),
              "only frame age controls resync, not failed metadata");
        for (const CaptureFrameHoldStatus cache : {
                CaptureFrameHoldStatus{true,true}, CaptureFrameHoldStatus{false,false}}) {
            DecideResyncFrameMode(AtGap(gap),cache);
            Check(sourceHdr==H::Hdr10Pq && sourceVrr==V::Enabled && label()==initialTitle &&
                  labelChanges==0,
                  "cached/fallback redraw preserves last-valid HDR/VRR and actual source labels");
        }
    }
    Check(labelChanges == 0 && !ShouldShowSignalResync(true, true, false, 0),
          "single metadata failure with valid capture cannot trigger hint");
    Check(!DecodeGc553ProTimingResponse(nullptr, 0).available &&
          !ShouldShowSignalResync(true, true, false, 16), "unavailable timing alone cannot trigger hint");
    hdrMonitor.Observe(DecodeGc553ProHdrResponse(sdr.data(), sdr.size()).state, 2000);
    vrrMonitor.Observe(DecodeGc553ProSourceVrr(DecodeGc553ProVtemResponse(off.data(), off.size())), 2000);
    Check(!hdrMonitor.PublishIfSettled(2749, &sourceHdr) && !vrrMonitor.PublishIfSettled(2749, &sourceVrr),
          "existing 750 ms metadata settle remains independent");
    Check(hdrMonitor.PublishIfSettled(2750, &sourceHdr) && sourceHdr == H::Sdr,
          "new valid 0x65 EOTF updates HDR normally");
    V nextVrr{};
    Check(vrrMonitor.PublishIfSettled(2750, &nextVrr) && nextVrr == V::Disabled,
          "new valid 0x7D field updates VRR normally");
    ApplyHdmiSourceVrrMetadata(sourceVrr, nextVrr, [&] { ++labelChanges; });
    Check(label() == L"NitLink - Switch 2 - 1920x1080 @ 60Hz [SDR]" && labelChanges == 1,
          "confirmed metadata refreshes source labels independently of presentation");
    Check(AtGap(0) == PresentationState::Capture, "capture recovery requires no metadata/three-second hold");
}
void FrameHoldTests() {
    using S = PresentationState;
    using M = ResyncFrameMode;
    using R = CaptureFrameHoldReason;
    // NV12 -> BGRA8 SDR, matching the real renderer's compatibility snapshot.
    CaptureFrameSemantics sdr{1920,1080,1,1920,1080,87,
        false,true,false,false,false,false,false,0,0,0};
    CaptureFrameHold hold;
    auto status = hold.Inspect(true, 1, sdr);
    Check(!status.available && !status.compatible && status.reason == R::NeverPresented,
          "renderer_has_frame/upload alone is not a successful Capture Present");
    Check(DecideResyncFrameMode(S::WaitingForCapture, status) == M::Fallback &&
          DecideResyncFrameMode(S::SignalResync, status) == M::Fallback,
          "startup/never-presented capture cannot freeze");
    hold.ObserveCompletedUpload(1);
    for (const auto flags : {std::array<bool,3>{false,true,true},
                             std::array<bool,3>{true,false,true},
                             std::array<bool,3>{true,true,false}}) {
        hold.ObservePresent(flags[0], flags[1], flags[2], 1, sdr);
        Check(!hold.Inspect(true, 1, sdr).available,
              "overlay/cached draw, failed NIS composite or failed/occluded Present is not live Capture");
    }
    hold.ObservePresent(true, true, true, 0, sdr);
    Check(!hold.HasPresentedFrame(), "uninitialized upload cannot produce a present receipt");
    hold.ObservePresent(true, true, true, 1, sdr);
    status = hold.Inspect(true, 1, sdr);
    Check(status.available && status.compatible &&
          DecideResyncFrameMode(AtGap(250), status) == M::Cached,
          "successful presentable Capture holds exact retained frame under resync overlay");
    Check(ResyncStatusBackgroundOpacity(M::Cached,true)>0 &&
          ResyncStatusBackgroundOpacity(M::Cached,true)<1,
          "status text uses a translucent scrim so the held image stays visible");
    Check(ResyncStatusBackgroundOpacity(M::Cached,false)==1 &&
          ResyncStatusBackgroundOpacity(M::Fallback,true)==1,
          "failed draw/fallback/startup retains the existing opaque status background");
    for (auto gap : {0,16,249}) {
        Check(DecideResyncFrameMode(AtGap(gap), status) == M::Live,
              "short interruption and first valid recovery remain immediately live");
    }
    for (auto state : {S::WaitingForCapture,S::Transition,S::NoSignal}) {
        Check(DecideResyncFrameMode(state, status) == M::Fallback,
              "cache cannot replace startup/transition or authoritative No Signal/custom image branch");
    }
    Check(DecideResyncFrameMode(DecidePresentation(true,false,true,true,true), status) == M::Fallback &&
          DecideResyncFrameMode(DecidePresentation(false,true,true,true,true), status) == M::Fallback,
          "No Signal latch and current loss retain priority even with a usable cache");
    Check(!CapturePresentationReady(false,true) && !CapturePresentationReady(true,false),
          "hold availability is independent of both existing recovery readiness conditions");
    Check(!hold.Inspect(false,1,sdr).compatible &&
          hold.Inspect(false,1,sdr).reason == R::ResourceUnavailable,
          "missing texture, either planar SRV, shader or output target rejects hold");
    Check(!hold.Inspect(true,2,sdr).available &&
          hold.Inspect(true,2,sdr).reason == R::UnpresentedUpload,
          "new upload skipped by pacing must not masquerade as last displayed frame");
    const auto reject = [&](const CaptureFrameSemantics& changed, R reason) {
        const auto rejected = hold.Inspect(true,1,changed);
        Check(rejected.available && !rejected.compatible && rejected.reason == reason &&
              DecideResyncFrameMode(S::SignalResync,rejected) == M::Fallback,
              "incompatible format/geometry/color conversion safely uses existing background");
        Check(DecideResyncFrameMode(S::Capture,rejected) == M::Live,
              "hold incompatibility never delays presentable live recovery");
    };
    auto changed=sdr; changed.format=2; reject(changed,R::InputFormatChanged);
    changed=sdr; changed.width=2560; reject(changed,R::InputSizeChanged);
    changed=sdr; changed.height=1440; reject(changed,R::InputSizeChanged);
    changed=sdr; changed.sourceHdr10=true; reject(changed,R::SourceColorChanged);
    changed=sdr; changed.topDown=false; reject(changed,R::SourceColorChanged);
    changed=sdr; changed.fullRange=true; reject(changed,R::SourceColorChanged);
    changed=sdr; changed.limitedChroma=true; reject(changed,R::SourceColorChanged);
    changed=sdr; changed.hdrOutput=true; reject(changed,R::OutputColorChanged);
    changed=sdr; changed.bt709Matrix=true; reject(changed,R::OutputColorChanged);
    changed=sdr; changed.outputFormat=24; reject(changed,R::OutputColorChanged);
    changed=sdr; changed.outputWidth=2560; reject(changed,R::OutputSizeChanged);
    changed=sdr; changed.outputHeight=1440; reject(changed,R::OutputSizeChanged);
    changed=sdr; changed.aspectOverride=4.0f/3; reject(changed,R::OutputSizeChanged);
    changed=sdr; changed.postInput=true; reject(changed,R::PostProcessChanged);
    changed=sdr; changed.colorExpansion=0.5f; reject(changed,R::ColorAdjustmentChanged);
    changed=sdr; changed.colorExpansionTarget=1; reject(changed,R::ColorAdjustmentChanged);
    for (bool sourceHdr : {false,true}) for (bool hdrOutput : {false,true}) {
        auto color=sdr; color.format=sourceHdr?2:1; color.sourceHdr10=sourceHdr;
        color.hdrOutput=hdrOutput; color.outputFormat=hdrOutput?24:87;
        hold.ObservePresent(true,true,true,1,color);
        Check(hold.Inspect(true,1,color).compatible,
              "matching SDR/HDR and HDR-to-SDR tone-map pipelines can hold raw inputs");
        auto other=color; other.sourceHdr10=!sourceHdr;
        Check(!hold.Inspect(true,1,other).compatible,
              "HDR and SDR inputs cannot be reinterpreted under a different EOTF flag");
        other=color; other.hdrOutput=!hdrOutput; other.outputFormat=hdrOutput?87:24;
        Check(!hold.Inspect(true,1,other).compatible,
              "either HDR output direction rejects the previous conversion until live Capture");
    }
    for (auto reason : {R::DeviceSwitch,R::CaptureSessionInvalidated,
                        R::ResourcesRecreated,R::TextureOverwritten}) {
        hold.ObserveCompletedUpload(1);
        hold.ObservePresent(true,true,true,1,sdr);
        hold.Invalidate(reason);
        status=hold.Inspect(true,1,sdr);
        Check(!status.available && !status.compatible && status.reason==reason &&
              DecideResyncFrameMode(S::SignalResync,status)==M::Fallback,
              "session/device/resource reset or partial WRITE_DISCARD cannot leak an old frame");
        hold.ObservePresent(false,true,true,1,sdr);
        Check(!hold.HasPresentedFrame(),"successful cached Present cannot restore an invalidated live receipt");
        Check(DecideResyncFrameMode(S::Capture,status)==M::Live,
              "live recovery never waits for cache restoration");
    }
    hold.Invalidate(R::TextureOverwritten);
    hold.ObservePresent(true,true,true,1,sdr);
    Check(!hold.HasPresentedFrame(),
          "partial/discarded upload cannot regain a receipt via stale HasFrame/serial and a later successful draw");
    hold.ObserveCompletedUpload(2);
    Check(!hold.HasPresentedFrame(), "complete upload alone still is not a displayed Capture frame");
    hold.ObservePresent(true,true,true,2,sdr);
    Check(hold.Inspect(true,2,sdr).compatible,
          "new complete upload followed by successful live Present restores hold safely");
}
void FrameHoldEpisodeTests() {
    using S = PresentationState;
    std::vector<S> stateChanges;
    CaptureFrameHold hold;
    CaptureFrameSemantics semantics{1920,1080,1,1920,1080,87};
    std::int64_t lastGood=1000;
    std::uint64_t serial=1;
    hold.ObserveCompletedUpload(serial);
    const auto render = [&](std::int64_t now, bool ready) {
        const auto state=AtGap(now-lastGood,true,false,ready);
        if (stateChanges.empty() || stateChanges.back()!=state) stateChanges.push_back(state);
        const auto mode=DecideResyncFrameMode(state,hold.Inspect(true,serial,semantics));
        hold.ObservePresent(mode==ResyncFrameMode::Live,true,true,serial,semantics);
        return std::tuple{state,mode};
    };
    render(lastGood,true);
    auto [state,mode]=render(1250,true);
    Check(state==PresentationState::SignalResync && mode==ResyncFrameMode::Cached &&
          stateChanges==std::vector<S>{S::Capture,S::SignalResync},
          "first gap begins first real episode with held frame");
    Check(ResyncStatusBackgroundOpacity(mode,true)==0.45f,
          "first episode shows a translucent hint over its safe presented frame");
    std::tie(state,mode)=render(1700,true);
    Check(state==S::SignalResync && stateChanges.size()==2,"cached redraw does not end or restart episode");

    hold.Invalidate(CaptureFrameHoldReason::CaptureSessionInvalidated);
    std::tie(state,mode)=render(1750,false);
    Check(state==S::SignalResync && mode==ResyncFrameMode::Fallback && stateChanges.size()==2 &&
          ResyncStatusBackgroundOpacity(mode,false)==1.0f,
          "unsafe format reopen falls back within the first episode without creating another episode");

    hold.Invalidate(CaptureFrameHoldReason::TextureOverwritten);
    ++serial; lastGood=1800;
    semantics.format=2; semantics.sourceHdr10=true;
    hold.ObserveCompletedUpload(serial);
    Check(!hold.HasPresentedFrame(),
          "fresh recovery upload alone cannot restore a receipt invalidated in the first episode");
    std::tie(state,mode)=render(lastGood,true);
    Check(state==PresentationState::Capture && mode==ResyncFrameMode::Live &&
          stateChanges==std::vector<S>{S::Capture,S::SignalResync,S::Capture},
          "first presentable new frame restores live output immediately, without hold delay");
    const auto recovered=hold.Inspect(true,serial,semantics);
    Check(recovered.available && recovered.compatible && recovered.reason==CaptureFrameHoldReason::None,
          "successful recovery Present replaces the first episode's invalid receipt and old color semantics");
    for (int i=1;i<=109;++i) {
        hold.Invalidate(CaptureFrameHoldReason::TextureOverwritten);
        ++serial; lastGood=1800+i*16;
        hold.ObserveCompletedUpload(serial); render(lastGood,true);
    }
    Check(hold.Inspect(true,serial,semantics).compatible &&
          !hold.Inspect(true,serial-1,semantics).available,
          "recovered Capture holds only its latest successfully uploaded and presented contents");
    std::tie(state,mode)=render(lastGood+250,true);
    Check(state==S::SignalResync && mode==ResyncFrameMode::Cached &&
          stateChanges==std::vector<S>{S::Capture,S::SignalResync,S::Capture,S::SignalResync},
          "real Capture recovery between interruptions keeps two separate episodes");
    const auto secondEpisodeCache=hold.Inspect(true,serial,semantics);
    Check(secondEpisodeCache.available && secondEpisodeCache.compatible &&
          secondEpisodeCache.reason==CaptureFrameHoldReason::None &&
          ResyncStatusBackgroundOpacity(mode,true)==0.45f,
          "second gap preserves the fresh safe receipt and shows another translucent hint");
    hold.Invalidate(CaptureFrameHoldReason::CaptureSessionInvalidated);
    std::tie(state,mode)=render(lastGood+1000,false);
    Check(state==PresentationState::SignalResync && mode==ResyncFrameMode::Fallback &&
          stateChanges.size()==4,
          "reopen invalidates hold without changing transition timing/state/episode count");
}
void PolicyIsolationTests() {
    for (int pacing : {kPacingRefresh,kPacingCaptured,kPacingUnique})
    for (bool vsync : {false,true})
    for (bool outputHdr : {false,true})
    for (auto format : {NegotiatedCaptureFormatKind::NV12,NegotiatedCaptureFormatKind::P010}) {
        Config config;
        config.presentPacing=pacing; config.vsync=vsync; config.hdrEnabled=outputHdr;
        config.lowLatency=false; config.presentCapHz=117;
        CaptureFormatOverride capture{2560,1440,120,L"P010",120,1};
        const auto sourceHdr = format == NegotiatedCaptureFormatKind::P010
            ? Gc553ProSourceHdrState::Hdr10Pq : Gc553ProSourceHdrState::Sdr;
        SourceCadence cadence;
        cadence.OnClassifiedFrame(true); cadence.OnClassifiedFrame(false);
        const auto snapshot = [&] {
            const auto policy = DecideGc553ProSourceOutputPolicy(format, config.hdrEnabled,
                CaptureFormatPreference::Auto, config.hdrAutoFromSource, sourceHdr);
            const auto output = DecideGc553ProOutputPolicy(format, config.hdrEnabled);
            return std::tuple{config.presentPacing,config.vsync,config.lowLatency,config.presentCapHz,
                config.hdrEnabled,config.hdrAutoFromSource,config.nisEnabled,config.colorExpansion,
                capture.width,capture.height,capture.fps,capture.format,capture.fpsNumerator,capture.fpsDenominator,
                policy.desiredCaptureIsP010,policy.reopenCapture,policy.hdrRejected,
                output.sdrFromHdrTonemap,cadence.CadenceFrames(),cadence.ShouldHold()};
        };
        const auto before = snapshot();
        for (auto gap : {0,16,249,250,1000}) {
            const auto state = AtGap(gap);
            Check(state == (gap < 250 ? PresentationState::Capture : PresentationState::SignalResync) &&
                  snapshot() == before, "resync changes only presentation, not capture/HDR/pacing/VSync policy");
            for (const CaptureFrameHoldStatus cache : {
                    CaptureFrameHoldStatus{true,true}, CaptureFrameHoldStatus{false,false}}) {
                const auto mode=DecideResyncFrameMode(state,cache);
                Check(mode==(state==PresentationState::Capture ? ResyncFrameMode::Live :
                            cache.compatible ? ResyncFrameMode::Cached : ResyncFrameMode::Fallback) &&
                      snapshot()==before,
                      "live/cached/fallback selection cannot modify capture reopen, pacing, VSync, HDR or tone mapping");
            }
        }
        const auto policy = DecideGc553ProSourceOutputPolicy(format, config.hdrEnabled,
            CaptureFormatPreference::Auto, config.hdrAutoFromSource, sourceHdr);
        Check(!policy.reopenCapture, "resync alone cannot request capture reopen/reconcile");
    }
}
}
int main() {
    try {
        PresentationTests(); EpisodeRegressionTests(); ValidBlackAndInvalidFrames(); MetadataTests();
        FrameHoldTests(); FrameHoldEpisodeTests(); PolicyIsolationTests();
        std::cout << checks << " SignalResync presentation checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n'; return 1;
    }
}
