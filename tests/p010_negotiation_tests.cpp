#include "capture/capture_device.h"
#include "capture/dshow_capture.h"
#include "capture/elgato_4kx_source.h"
#include "app/capture_output_policy.h"

#include <wrl/implements.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace NitLink {

struct CaptureDeviceFormatTests {
    static bool Negotiate(CaptureDevice& device, IMFMediaSource* source,
                          const wchar_t* name, bool hdr,
                          const CaptureDevice::OverrideSpec& overrideSpec = {})
    {
        device.m_deviceName = name;
        device.RequestP010(hdr);
        device.SetFormatOverride(overrideSpec);
        return device.NegotiateFormat(source);
    }

    static CaptureFormat Format(const CaptureDevice& device)
    {
        return device.m_format;
    }

    static CaptureFormat ReadDShowRate(int64_t interval)
    {
        DShowCapture device;
        device.ReadFrameRateFromInterval(interval);
        return device.GetOutputFormat();
    }

    static HRESULT WriteRate(CaptureDevice& device, IMFMediaType* output)
    {
        return device.SetOutputFrameRate(output);
    }

    static void ReadbackRateAndPublish(CaptureDevice& device, IMFMediaType* actual)
    {
        device.UpdateFrameRateFromActual(actual);
        device.PublishFormat();
    }

    static void ClearPublishedFormat(CaptureDevice& device)
    {
        device.ClearPublishedFormat();
    }

    static void SeedP010Notice(CaptureDevice& device,
                               const P010SelectionNotice& notice)
    {
        device.m_p010SelectionNotice = notice;
    }

    static void UpdateNoticeFromActual(CaptureDevice& device,
                                       const CaptureFormat& actual)
    {
        device.RequestP010(true);
        device.UpdateP010SelectionNoticeFromActual(actual);
    }

    static P010SelectionNotice Notice(const CaptureDevice& device)
    {
        return device.m_p010SelectionNotice;
    }

    static void SeedPublishedP010(CaptureDevice& device,
                                  const DeviceInfo& info)
    {
        device.m_deviceName = info.name;
        device.m_deviceIdentity = info.symbolicLink;
        device.m_format = {};
        device.m_format.subtype = MFVideoFormat_P010;
        device.PublishFormat();
    }

    static bool HasPublishedFormatForDevice(const CaptureDevice& device,
                                            const DeviceInfo& info)
    {
        return device.HasPublishedFormatForDevice(info);
    }

    static ComPtr<IMFMediaType> Pin(CaptureDevice& device,
                                    IMFMediaSource* source, const GUID& subtype,
                                    const CaptureDevice::OverrideSpec& overrideSpec)
    {
        device.SetFormatOverride(overrideSpec);
        return device.PinNativeMediaType(source, subtype);
    }

    static CaptureDevice::OverrideSpec Override(const CaptureDevice& device)
    {
        return device.m_overrideSpec;
    }
};

} // namespace NitLink

namespace {

void Check(HRESULT hr)
{
    if (FAILED(hr)) throw std::runtime_error("Media Foundation fixture failed");
}

bool Expect(bool condition, const char* label)
{
    std::cout << (condition ? "PASS " : "FAIL ") << label << '\n';
    return condition;
}

class DescriptorSource final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFMediaSource> {
public:
    explicit DescriptorSource(ComPtr<IMFPresentationDescriptor> descriptor)
        : m_descriptor(descriptor) {}

    HRESULT STDMETHODCALLTYPE CreatePresentationDescriptor(
        IMFPresentationDescriptor** descriptor) override
    {
        return m_descriptor.CopyTo(descriptor);
    }

    HRESULT STDMETHODCALLTYPE GetEvent(DWORD, IMFMediaEvent**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE BeginGetEvent(IMFAsyncCallback*, IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EndGetEvent(IMFAsyncResult*, IMFMediaEvent**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueueEvent(MediaEventType, REFGUID, HRESULT, const PROPVARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Start(IMFPresentationDescriptor*, const GUID*, const PROPVARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Stop() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Pause() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Shutdown() override { return S_OK; }

private:
    ComPtr<IMFPresentationDescriptor> m_descriptor;
};

struct Mode {
    GUID subtype;
    UINT32 width;
    UINT32 height;
    UINT32 numerator;
    UINT32 denominator;
    bool hasRate = true;
    UINT32 interlace = MFVideoInterlace_Unknown;  // Unknown leaves MF_MT_INTERLACE_MODE unset
};

struct Stream {
    std::vector<Mode> modes;
    bool selected = true;
};

ComPtr<IMFMediaSource> MakeSource(const std::vector<Stream>& streams)
{
    std::vector<ComPtr<IMFStreamDescriptor>> ownedStreams;
    std::vector<IMFStreamDescriptor*> rawStreams;
    for (const auto& stream : streams) {
        std::vector<ComPtr<IMFMediaType>> ownedTypes;
        std::vector<IMFMediaType*> rawTypes;
        for (const auto& mode : stream.modes) {
            ComPtr<IMFMediaType> type;
            Check(MFCreateMediaType(&type));
            Check(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
            Check(type->SetGUID(MF_MT_SUBTYPE, mode.subtype));
            Check(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, mode.width, mode.height));
            if (mode.hasRate) {
                Check(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE,
                                         mode.numerator, mode.denominator));
            }
            if (mode.interlace != MFVideoInterlace_Unknown) {
                Check(type->SetUINT32(MF_MT_INTERLACE_MODE, mode.interlace));
            }
            rawTypes.push_back(type.Get());
            ownedTypes.push_back(type);
        }
        ComPtr<IMFStreamDescriptor> descriptor;
        Check(MFCreateStreamDescriptor(static_cast<DWORD>(ownedStreams.size()),
            static_cast<DWORD>(rawTypes.size()), rawTypes.data(), &descriptor));
        rawStreams.push_back(descriptor.Get());
        ownedStreams.push_back(descriptor);
    }
    ComPtr<IMFPresentationDescriptor> descriptor;
    Check(MFCreatePresentationDescriptor(static_cast<DWORD>(rawStreams.size()),
                                         rawStreams.data(), &descriptor));
    for (DWORD i = 0; i < static_cast<DWORD>(streams.size()); ++i) {
        Check(streams[i].selected ? descriptor->SelectStream(i)
                                 : descriptor->DeselectStream(i));
    }
    return Microsoft::WRL::Make<DescriptorSource>(descriptor);
}

bool ExpectRate(NitLink::CaptureDevice& device, UINT32 numerator,
                UINT32 denominator, const char* label)
{
    ComPtr<IMFMediaType> output;
    Check(MFCreateMediaType(&output));
    Check(NitLink::CaptureDeviceFormatTests::WriteRate(device, output.Get()));
    UINT32 actualNumerator = 0, actualDenominator = 0;
    Check(MFGetAttributeRatio(output.Get(), MF_MT_FRAME_RATE,
                              &actualNumerator, &actualDenominator));
    return Expect(actualNumerator == numerator && actualDenominator == denominator, label);
}

bool RunChecks()
{
    using namespace NitLink;
    using Access = CaptureDeviceFormatTests;
    constexpr auto gc553 = L"AVerMedia GC553Pro";
    bool pass = true;
    const Mode nv12 = {MFVideoFormat_NV12, 3840, 2160, 60, 1};
    const Mode p010 = {MFVideoFormat_P010, 1920, 1080, 60, 1};

    for (const int64_t interval : {83333LL, 166666LL, 333667LL}) {
        const auto format = Access::ReadDShowRate(interval);
        pass &= Expect(format.fpsNumerator == 10000000 &&
                       format.fpsDenominator == static_cast<uint32_t>(interval) &&
                       format.fps == 10000000 / interval,
                       "DirectShow publishes the negotiated interval, not default 60/1");
    }
    for (const int64_t interval : {0LL, -1LL, INT64_MAX}) {
        const auto format = Access::ReadDShowRate(interval);
        pass &= Expect(format.fpsNumerator == 0 && format.fpsDenominator == 1 &&
                       format.fps == 0,
                       "invalid or unrepresentable DirectShow rate is unknown");
    }

    {
        CaptureDevice device;
        const auto published = device.GetOutputFormat();
        pass &= Expect(published.width == 0 && published.height == 0 &&
                       published.fps == 0 && published.fpsNumerator == 0 &&
                       published.fpsDenominator == 1 &&
                       device.GetPresentPacingRateHint() == 0,
                       "unopened device publishes unknown dimensions and rate");
        const auto working = Access::Format(device);
        pass &= Expect(working.width == 3840 && working.height == 2160 &&
                       working.fps == 60,
                       "unpublished metadata does not change negotiation defaults");
    }

    // The output readback must replace a requested 120, including when MF
    // substitutes another rate or does not report MF_MT_FRAME_RATE at all.
    for (const Mode actual : {
             Mode{MFVideoFormat_NV12, 1920, 1080, 120, 1},
             Mode{MFVideoFormat_NV12, 1920, 1080, 60, 1},
             Mode{MFVideoFormat_NV12, 1920, 1080, 120000, 1001},
             Mode{MFVideoFormat_NV12, 1920, 1080, 10000000, 83333},
             Mode{MFVideoFormat_NV12, 1920, 1080, 0, 1},
             Mode{MFVideoFormat_NV12, 1920, 1080, 120, 0},
             Mode{MFVideoFormat_NV12, 1920, 1080, 120, 1, false}}) {
        CaptureDevice device;
        auto source = MakeSource({{{{MFVideoFormat_NV12, 1920, 1080, 120, 1}}}});
        CaptureDevice::OverrideSpec manual;
        manual.width = 1920;
        manual.height = 1080;
        manual.fps = manual.fpsNumerator = 120;
        pass &= Expect(Access::Negotiate(device, source.Get(), gc553, false, manual),
                       "manual 1080p120 Auto request selects native SDR mode");
        pass &= ExpectRate(device, 120, 1, "outgoing request asks for 120/1");
        ComPtr<IMFMediaType> actualType;
        Check(MFCreateMediaType(&actualType));
        if (actual.hasRate) {
            Check(MFSetAttributeRatio(actualType.Get(), MF_MT_FRAME_RATE,
                                      actual.numerator, actual.denominator));
        }
        Access::ReadbackRateAndPublish(device, actualType.Get());
        const auto published = device.GetOutputFormat();
        const bool known = actual.hasRate && actual.numerator > 0 && actual.denominator > 0;
        pass &= Expect(published.fpsNumerator == (known ? actual.numerator : 0) &&
                       published.fpsDenominator == (known ? actual.denominator : 1) &&
                       published.fps == (known ? actual.numerator / actual.denominator : 0),
                       "published FPS uses actual MF readback, never a missing-rate request fallback");
        pass &= Expect(device.GetPresentPacingRateHint() ==
                           (known ? actual.numerator / actual.denominator : 120),
                       "unknown MF statistics preserve the pre-existing present-cap hint");
        Access::ClearPublishedFormat(device);
        const auto cleared = device.GetOutputFormat();
        pass &= Expect(cleared.width == 0 && cleared.height == 0 &&
                       cleared.fps == 0 && cleared.fpsNumerator == 0 &&
                       cleared.fpsDenominator == 1 &&
                       device.GetPresentPacingRateHint() == 0,
                       "cleared session cannot publish defaults or stale rate hints");
    }

    for (const UINT32 numerator : {60000u, 30000u, 24000u, 120000u}) {
        CaptureDevice device;
        auto source = MakeSource({{{{MFVideoFormat_P010, 1920, 1080, numerator, 1001}}}});
        pass &= Expect(Access::Negotiate(device, source.Get(), gc553, true),
                       "fractional native mode negotiates");
        pass &= ExpectRate(device, numerator, 1001, "outgoing request retains native ratio");
    }

    {
        CaptureDevice device;
        auto source = MakeSource({{{{MFVideoFormat_P010, 2560, 1440, 30, 1}, p010}}});
        pass &= Expect(Access::Negotiate(device, source.Get(), gc553, true),
                       "integer native modes negotiate");
        const auto format = Access::Format(device);
        pass &= Expect(format.width == 1920 && format.height == 1080,
                       "GC553Pro keeps high-FPS selection");
        pass &= ExpectRate(device, 60, 1, "integer native rate unchanged");
    }

    for (const CaptureDevice::OverrideSpec overrideSpec : {
             CaptureDevice::OverrideSpec{}, {1920, 1080, 60, L"P010"}}) {
        CaptureDevice device;
        auto source = MakeSource({{{nv12}}});
        pass &= Expect(!Access::Negotiate(device, source.Get(), gc553, true, overrideSpec),
                       "NV12-only enumeration rejects GC553Pro HDR request");
        pass &= Expect(Access::Negotiate(device, source.Get(), gc553, false),
                       "SDR retry negotiates after absent P010");
        const auto format = Access::Format(device);
        pass &= Expect(format.width == 3840 && format.height == 2160,
                       "SDR retry uses native dimensions");
        pass &= ExpectRate(device, 60, 1, "SDR retry uses SDR rate");
    }

    for (const Mode invalid : {
             Mode{MFVideoFormat_P010, 1920, 1080, 0, 1},
             Mode{MFVideoFormat_P010, 1920, 1080, 60000, 0},
             Mode{MFVideoFormat_P010, 1920, 1080, 60, 1, false},
             Mode{MFVideoFormat_P010, 0, 1080, 60, 1}}) {
        CaptureDevice device;
        auto source = MakeSource({{{invalid}}});
        pass &= Expect(!Access::Negotiate(device, source.Get(), gc553, true),
                       "incomplete native P010 mode rejected");
    }

    {
        CaptureDevice device;
        auto source = MakeSource({{{nv12}}, {{p010}}});
        pass &= Expect(Access::Negotiate(device, source.Get(), gc553, true),
                       "later selected stream supplies native P010");
        source = MakeSource({{{nv12}}, {{p010}, false}});
        pass &= Expect(!Access::Negotiate(device, source.Get(), gc553, true),
                       "unselected P010 stream does not grant HDR capability");
    }

    {
        CaptureDevice device;
        auto source = MakeSource({{{{MFVideoFormat_P010, 1920, 1080, 60000, 1001}}}});
        pass &= Expect(Access::Negotiate(device, source.Get(), gc553, true),
                       "fractional rate initially selected");
        source = MakeSource({{{{MFVideoFormat_NV12, 1920, 1080, 120, 1}}}});
        pass &= Expect(Access::Negotiate(device, source.Get(), gc553, false),
                       "switch to SDR negotiates");
        pass &= ExpectRate(device, 120, 1, "previous HDR rate cannot leak into SDR");
    }

    {
        CaptureDevice device;
        CaptureDevice::OverrideSpec overrideSpec;
        overrideSpec.width = 1920;
        overrideSpec.height = 1080;
        overrideSpec.fps = 59;
        overrideSpec.fpsNumerator = 60000;
        overrideSpec.fpsDenominator = 1001;
        overrideSpec.format = L"P010";
        auto source = MakeSource({{{{MFVideoFormat_P010, 1920, 1080,
                                     60000, 1001}}}});
        pass &= Expect(Access::Negotiate(device, source.Get(), gc553, true,
                                         overrideSpec),
                       "manual fractional override negotiates");
        pass &= ExpectRate(device, 60000, 1001,
                           "manual override keeps native 60000/1001");
    }

    {
        CaptureDevice device;
        CaptureDevice::OverrideSpec legacyOverride;
        legacyOverride.width = 1920;
        legacyOverride.height = 1080;
        legacyOverride.fps = 59;
        legacyOverride.fpsNumerator = 0;
        legacyOverride.fpsDenominator = 1;
        legacyOverride.format = L"P010";
        auto source = MakeSource({{{{MFVideoFormat_P010, 1920, 1080,
                                     60000, 1001}}}});
        pass &= Expect(Access::Negotiate(device, source.Get(), gc553, true,
                                         legacyOverride),
                       "legacy integer-only 59 FPS override negotiates");
        pass &= ExpectRate(device, 60000, 1001,
                           "legacy 59 FPS resolves to native 60000/1001");
    }

    {
        CaptureDevice device;
        Access::SeedP010Notice(device, {true, 2560, 1440, 30, 30, 1});
        CaptureFormat actual;
        actual.width = 1920;
        actual.height = 1080;
        actual.fps = 59;
        actual.fpsNumerator = 60000;
        actual.fpsDenominator = 1001;
        actual.subtype = MFVideoFormat_P010;
        Access::UpdateNoticeFromActual(device, actual);
        const auto notice = Access::Notice(device);
        pass &= Expect(notice.available && notice.width == 1920 &&
                       notice.height == 1080 && notice.fpsNumerator == 60000 &&
                       notice.fpsDenominator == 1001,
                       "P010 selection notice uses actual readback");
    }

    {
        const DeviceInfo gc553SessionA{
            L"AVerMedia GC553Pro", L"\\\\?\\usb#vid_07ca&pid_313a#session", 0};
        CaptureDevice device;
        Access::SeedPublishedP010(device, gc553SessionA);
        pass &= Expect(Access::HasPublishedFormatForDevice(device,
                                                            gc553SessionA),
                       "session A publishes P010 for GC553Pro");

        // Close ends session A. A same-device session B must start unknown
        // until its own Open/readback publishes a format.
        device.Close();
        pass &= Expect(!Access::HasPublishedFormatForDevice(device,
                                                             gc553SessionA),
                       "same-device session B cannot retain session A format before readback");

        Access::SeedPublishedP010(device, gc553SessionA);
        pass &= Expect(Access::HasPublishedFormatForDevice(device,
                                                            gc553SessionA),
                       "session B may publish its own P010 after readback");
    }

    {
        Source4KProMode mode;
        mode.detected = true;
        mode.width = 2560;
        mode.height = 1440;
        mode.fps = 60;
        mode.hdrValid = true;
        const Mode xP010 = {MFVideoFormat_P010, 2560, 1440, 60, 1};
        const Mode xNV12 = {MFVideoFormat_NV12, 2560, 1440, 60, 1};
        for (bool hdr : {true, false}) {
            mode.hdrActive = hdr;
            bool sourceHDR = false, available = false;
            Apply4KXSourceHdr(mode, sourceHDR, available);
            for (bool output : {false, true}) {
                const auto policy = Decide4KXSourceCapturePolicy(
                    NegotiatedCaptureFormatKind::Other, false, output,
                    CaptureFormatPreference::Auto, sourceHDR, available, false);
                CaptureDevice device;
                auto source = MakeSource({{{xNV12, xP010}}});
                CaptureDevice::OverrideSpec ov;
                ov.width = 2560; ov.height = 1440; ov.fps = 60;
                pass &= Expect(Access::Negotiate(device, source.Get(), L"Elgato 4K X",
                                                policy.desiredCaptureIsP010, ov),
                               "4K X source policy selects the advertised 1440p60 native mode");
                pass &= Expect(policy.desiredCaptureIsP010 == hdr,
                               "4K X requests P010 only for valid HDR regardless of output preference");
                pass &= Expect(Access::Format(device).width == 2560 &&
                               Access::Format(device).height == 1440,
                               "4K X keeps 1440p dimensions");
                pass &= ExpectRate(device, 60, 1, "4K X keeps 60 Hz at 1440p");
            }
        }
        CaptureDevice fallbackDevice;
        auto onlySDR = MakeSource({{{xNV12}}});
        pass &= Expect(Access::Negotiate(fallbackDevice, onlySDR.Get(), L"Elgato 4K X", true),
                       "4K X enumeration without P010 retains the generic Open fallback path");
    }
    for (const auto name : {L"Elgato Game Capture 4K Pro", L"Elgato 4K S", L"Generic capture card"}) {
        CaptureDevice device;
        auto source = MakeSource({{{{MFVideoFormat_P010, 2560, 1440, 30, 1}, p010}}});
        pass &= Expect(Access::Negotiate(device, source.Get(), name, true),
                       "existing non-GC553Pro negotiation succeeds");
        pass &= Expect(Access::Format(device).width == 2560,
                       "existing largest-width policy unchanged");
        pass &= ExpectRate(device, 30, 1, "existing integer request unchanged");
        source = MakeSource({{{nv12}}});
        pass &= Expect(Access::Negotiate(device, source.Get(), name, true),
                       "existing generic fallback behavior unchanged");
    }

    // YUY2 -> NV12 repack: luma copied exactly, chroma averaged per row pair.
    {
        const uint8_t yuy2[] = {
            10, 100, 20, 150,   30, 110, 40, 160,   // row 0: Y0 U Y1 V | Y0 U Y1 V
            50, 102, 60, 154,   70, 114, 80, 166,   // row 1
        };
        uint8_t nv12[4 * 2 * 3 / 2] = {};
        ConvertYuy2ToNv12(yuy2, 8, 4, 2, nv12);
        const uint8_t expected[] = {10, 20, 30, 40, 50, 60, 70, 80,
                                    101, 152, 112, 163};
        pass &= Expect(std::equal(std::begin(expected), std::end(expected), nv12),
                       "YUY2 repack keeps every luma sample and averages chroma rows");
    }

    // Manual YUY2: the picked progressive native mode is locked on the
    // source. Mode list mirrors a generic USB card that publishes YUY2 and
    // MJPEG; the larger MJPEG mode must never be picked for a YUY2 override.
    {
        constexpr UINT32 kProgressive = MFVideoInterlace_Progressive;
        const std::vector<Stream> usbCard = {{{
            {MFVideoFormat_YUY2, 1280, 720, 60, 1, true, kProgressive},
            {MFVideoFormat_YUY2, 1920, 1080, 30, 1, true, kProgressive},
            {MFVideoFormat_YUY2, 1920, 1080, 60, 1, true, kProgressive},
            {MFVideoFormat_MJPG, 1920, 1080, 60, 1, true, kProgressive},
            {MFVideoFormat_MJPG, 2560, 1440, 60, 1, true, kProgressive},
        }}};
        const auto expectPinned = [&](IMFMediaType* pinned, IMFMediaSource* source,
                                      const GUID& subtype, UINT32 width, UINT32 height,
                                      UINT32 fps, const char* label) {
            ComPtr<IMFPresentationDescriptor> pd;
            ComPtr<IMFStreamDescriptor> sd;
            ComPtr<IMFMediaTypeHandler> handler;
            ComPtr<IMFMediaType> current;
            BOOL selected = FALSE;
            GUID currentSubtype{};
            UINT32 w = 0, h = 0, num = 0, den = 0;
            const bool ok = pinned &&
                SUCCEEDED(source->CreatePresentationDescriptor(&pd)) &&
                SUCCEEDED(pd->GetStreamDescriptorByIndex(0, &selected, &sd)) &&
                SUCCEEDED(sd->GetMediaTypeHandler(&handler)) &&
                SUCCEEDED(handler->GetCurrentMediaType(&current)) &&
                SUCCEEDED(current->GetGUID(MF_MT_SUBTYPE, &currentSubtype)) &&
                SUCCEEDED(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h)) &&
                SUCCEEDED(MFGetAttributeRatio(current.Get(), MF_MT_FRAME_RATE, &num, &den)) &&
                IsEqualGUID(currentSubtype, subtype) && w == width && h == height &&
                den != 0 && num / den == fps;
            return Expect(ok, label);
        };

        {
            CaptureDevice device;
            auto source = MakeSource(usbCard);
            CaptureDevice::OverrideSpec spec;
            spec.width = 1920; spec.height = 1080;
            spec.fps = 30; spec.fpsNumerator = 30; spec.fpsDenominator = 1;
            spec.format = L"YUY2";
            auto pinned = Access::Pin(device, source.Get(), MFVideoFormat_YUY2, spec);
            pass &= expectPinned(pinned.Get(), source.Get(), MFVideoFormat_YUY2,
                                 1920, 1080, 30,
                                 "YUY2 1080p30 pick honours the exact rate");
        }
        {
            CaptureDevice device;
            auto source = MakeSource(usbCard);
            CaptureDevice::OverrideSpec spec;
            spec.format = L"YUY2";  // size and rate Auto
            auto pinned = Access::Pin(device, source.Get(), MFVideoFormat_YUY2, spec);
            pass &= expectPinned(pinned.Get(), source.Get(), MFVideoFormat_YUY2,
                                 1920, 1080, 60,
                                 "YUY2 with size/rate Auto picks the largest, fastest YUY2 mode");
        }
        {
            CaptureDevice device;
            auto source = MakeSource(usbCard);
            CaptureDevice::OverrideSpec spec;
            spec.width = 1920; spec.height = 1080;
            spec.format = L"YUY2";  // rate Auto
            auto pinned = Access::Pin(device, source.Get(), MFVideoFormat_YUY2, spec);
            pass &= expectPinned(pinned.Get(), source.Get(), MFVideoFormat_YUY2,
                                 1920, 1080, 60,
                                 "YUY2 1080p with rate Auto picks 60 over 30");
        }
        {
            CaptureDevice device;
            auto source = MakeSource(usbCard);
            CaptureDevice::OverrideSpec spec;
            spec.width = 2560; spec.height = 1440;
            spec.format = L"YUY2";  // 1440p exists only as MJPEG
            auto pinned = Access::Pin(device, source.Get(), MFVideoFormat_YUY2, spec);
            pass &= Expect(!pinned, "YUY2 at a size the card only has in MJPEG is not pinned");
        }

        // Interlacing is decided by the media type alone: the repack never
        // deinterlaces, so only modes typed progressive may be pinned.
        {
            CaptureDevice device;
            auto source = MakeSource({{{
                {MFVideoFormat_YUY2, 1920, 1080, 60, 1, true,
                 MFVideoInterlace_FieldInterleavedUpperFirst},
            }}});
            CaptureDevice::OverrideSpec spec;
            spec.format = L"YUY2";
            auto pinned = Access::Pin(device, source.Get(), MFVideoFormat_YUY2, spec);
            pass &= Expect(!pinned, "interlaced YUY2 mode is not pinned");
        }
        {
            CaptureDevice device;
            auto source = MakeSource({{{
                {MFVideoFormat_YUY2, 1920, 1080, 60, 1},  // interlace mode not set
            }}});
            CaptureDevice::OverrideSpec spec;
            spec.format = L"YUY2";
            auto pinned = Access::Pin(device, source.Get(), MFVideoFormat_YUY2, spec);
            pass &= Expect(!pinned, "YUY2 mode without an interlace mode is not pinned");
        }
        {
            CaptureDevice device;
            auto source = MakeSource({{{
                {MFVideoFormat_YUY2, 1920, 1080, 30, 1, true, kProgressive},
                {MFVideoFormat_YUY2, 1920, 1080, 60, 1, true,
                 MFVideoInterlace_MixedInterlaceOrProgressive},
            }}});
            CaptureDevice::OverrideSpec spec;
            spec.format = L"YUY2";  // rate Auto would prefer 60
            auto pinned = Access::Pin(device, source.Get(), MFVideoFormat_YUY2, spec);
            pass &= expectPinned(pinned.Get(), source.Get(), MFVideoFormat_YUY2,
                                 1920, 1080, 30,
                                 "rate Auto skips a faster mixed-interlace mode for the progressive one");
        }
        {
            // A legacy integer-only rate is resolved by NegotiateFormat
            // before the pin. 59 must land on the card's 60000/1001 YUY2.
            CaptureDevice device;
            auto source = MakeSource({{{
                {MFVideoFormat_YUY2, 1920, 1080, 60000, 1001, true, kProgressive},
                {MFVideoFormat_MJPG, 1920, 1080, 60, 1, true, kProgressive},
            }}});
            CaptureDevice::OverrideSpec spec;
            spec.width = 1920; spec.height = 1080; spec.fps = 59;
            spec.format = L"YUY2";
            pass &= Expect(Access::Negotiate(device, source.Get(), L"Generic capture card",
                                             false, spec),
                           "legacy 59 FPS YUY2 override negotiates");
            const auto resolved = Access::Override(device);
            pass &= Expect(resolved.fpsNumerator == 60000 && resolved.fpsDenominator == 1001,
                           "legacy 59 FPS YUY2 resolves to native 60000/1001");
            auto pinned = Access::Pin(device, source.Get(), MFVideoFormat_YUY2, resolved);
            pass &= expectPinned(pinned.Get(), source.Get(), MFVideoFormat_YUY2,
                                 1920, 1080, 59,
                                 "resolved legacy YUY2 rate pins the 60000/1001 mode");
        }
    }
    return pass;
}

} // namespace

int main()
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 1;
    const HRESULT mf = MFStartup(MF_VERSION);
    if (FAILED(mf)) {
        CoUninitialize();
        return 1;
    }
    bool pass = false;
    try {
        pass = RunChecks();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    if (FAILED(MFShutdown())) pass = false;
    CoUninitialize();
    return pass ? 0 : 1;
}
