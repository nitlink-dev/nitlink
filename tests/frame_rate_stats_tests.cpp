#include "app/config.h"
#include "app/frame_rate_stats.h"
#include "app/localization.h"
#include "capture/frame_buffer.h"

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void CheckMotionSequence(int pacing)
{
    using namespace NitLink;
    // Each row is one second: still -> first motion -> sustained motion ->
    // still. Source pacing may really present less often during motion; only
    // capture pacing's *old display* invented a drop from content verdicts.
    const bool sourcePacing = pacing == kPacingUnique;
    const std::array<FrameRateCounters, 4> windows = {{
        {120, 0, 120}, {120, 3, sourcePacing ? 4u : 120u},
        {120, 64, sourcePacing ? 64u : 120u}, {120, 0, 120}
    }};
    FrameRateCounters total;
    for (const auto& window : windows) {
        const auto previous = total;
        total.captured += window.captured;
        total.content += window.content;
        total.presented += window.presented;
        const auto stats = SampleFrameRates(total, previous, 1.0, true);
        Require(stats.captureFps == 120, "motion cannot change the capture counter's meaning");
        Require(stats.contentFps == window.content,
                "content FPS includes genuine zero windows without capture fallback");
        Require(stats.HudFps() == window.presented,
                "HUD must show present FPS, including motion transitions, in every pacing mode");
    }
    const auto stopped = SampleFrameRates(total, total, 1.0, true);
    Require(stopped.captureFps == 0 && stopped.contentFps == 0 && stopped.HudFps() == 0,
            "all stopped counters must decay to zero");

    const auto noPresents = SampleFrameRates({120, 64, 0}, {}, 1.0, true);
    Require(noPresents.HudFps() == 0 && noPresents.captureFps == 120 &&
            noPresents.contentFps == 64, "zero present FPS must not fall back to another metric");
    const auto keepalive = SampleFrameRates({0, 0, 4}, {}, 1.0, true);
    Require(keepalive.captureFps == 0 && keepalive.contentFps == 0 &&
            keepalive.HudFps() == 4, "keepalive presents are independent of capture delivery");
}

void CheckDeliveryAndDrops()
{
    using namespace NitLink;
    FrameBuffer buffer(8, 8, 8);
    std::array<uint8_t, 96> picture{};
    for (int i = 0; i < 120; ++i) {
        buffer.Write(picture.data(), static_cast<uint32_t>(picture.size()), i);
    }
    Require(buffer.GetFramesWritten() == 120 && buffer.GetFramesDropped() == 119,
            "unconsumed frames still count as captured and separately count as dropped");
    const auto stats = SampleFrameRates({buffer.GetFramesWritten(), 0, 4}, {}, 1.0, true);
    Require(stats.captureFps == 120 && stats.contentFps == 0 && stats.HudFps() == 4,
            "capture rate cannot imply content or present throughput when frames are dropped");
}

} // namespace

int main()
{
    using namespace NitLink;
    try {
        for (int pacing : {kPacingRefresh, kPacingCaptured, kPacingUnique}) {
            CheckMotionSequence(pacing);
            std::cout << "PASS still/motion/still and zero FPS, pacing=" << pacing << '\n';
        }
        const auto unavailable = SampleFrameRates({120, 64, 60}, {}, 1.0, false);
        Require(!unavailable.contentAvailable && unavailable.contentFps == 0 &&
                unavailable.HudFps() == 60, "missing differ must not masquerade as capture/content FPS");
        const auto reset = SampleFrameRates({5, 2, 3}, {120, 64, 120}, 1.0, true);
        Require(reset.captureFps == 0 && reset.contentFps == 0 && reset.presentFps == 0,
                "counter resets must not underflow");
        const auto partialReset = SampleFrameRates({5, 66, 240}, {120, 64, 120}, 1.0, true);
        Require(partialReset.captureFps == 0 && partialReset.contentFps == 2 &&
                partialReset.presentFps == 120, "a capture reset cannot reset unrelated counters");
        const auto resumed = SampleFrameRates({125, 66, 123}, {5, 2, 3}, 1.0, true);
        Require(resumed.captureFps == 120 && resumed.contentFps == 64 && resumed.HudFps() == 120,
                "sampling recovers after a counter reset");
        const auto fractionalWindow = SampleFrameRates({180, 90, 144}, {}, 1.5, true);
        Require(fractionalWindow.captureFps == 120 && fractionalWindow.contentFps == 60 &&
                fractionalWindow.HudFps() == 96, "rates use the elapsed wall-clock window");
        const auto uncapped = SampleFrameRates({120, 59, 1576}, {}, 1.0, true);
        Require(uncapped.captureFps == 120 && uncapped.contentFps == 59 && uncapped.presentFps == 1576,
                "successful duplicate submissions above refresh must be counted honestly");
        const auto twoSeconds = SampleFrameRates({240, 118, 3152}, {}, 2.0, true);
        Require(twoSeconds.presentFps == 1576,
                "large Present FPS comes from submissions per real elapsed second, not window length");
        const auto saturated = SampleFrameRates(
            {std::numeric_limits<uint64_t>::max(), 0, 0}, {}, 0.5, true);
        Require(saturated.captureFps == std::numeric_limits<uint32_t>::max(),
                "out-of-range rates saturate before the integer conversion");
        for (double elapsed : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()}) {
            const auto invalid = SampleFrameRates({120, 60, 120}, {}, elapsed, true);
            Require(invalid.captureFps == 0 && invalid.contentFps == 0 && invalid.HudFps() == 0,
                    "invalid sample interval must not generate FPS");
        }
        CheckDeliveryAndDrops();
        auto& localization = Localization::Instance();
        for (const auto language : {LanguagePreference::English,
                                    LanguagePreference::TraditionalChinese}) {
            localization.SetPreference(language);
            const bool english = language == LanguagePreference::English;
            Require(localization.Get(L"overlay.presentRate") ==
                        (english ? L"Present rate" : L"呈現幀率") &&
                    localization.Get(L"overlay.captureFps") ==
                        (english ? L"Capture FPS" : L"擷取 FPS") &&
                    localization.Get(L"overlay.contentFps") ==
                        (english ? L"Content FPS (est.)" : L"內容 FPS（估計）"),
                    "both languages must label all three independent FPS metrics");
        }
        std::cout << "PASS independent rates, missing differ, resets, sample intervals and dropped frames\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
