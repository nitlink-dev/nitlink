#include "app/present_cap_policy.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    using namespace NitLink;
    try {
        const auto before = ChoosePresentCap({0, 120.0, 60.0});
        const auto after = ChoosePresentCap({0, 120.0, 120.0});
        Require(before.effectiveHz == 117.0, "1080p60 retains the existing automatic headroom cap");
        Require(after.effectiveHz == 120.0,
                "1080p60 -> 1080p120 must not turn Auto into uncapped Present");
        Require(ChoosePresentCap({0, 120.0, 60.0}).effectiveHz == before.effectiveHz,
                "1080p120 -> 1080p60 restores the headroom cap");

        unsigned combinations = 0;
        for (double capture : {60.0, 120.0}) {
            for (double monitor : {60.0, 120.0, 144.0, 180.0}) {
                for (int configured : {0, -1, 95}) {
                    for (bool vsync : {false, true}) {
                        for (bool lowLatency : {false, true}) {
                            for (int pacing : {kPacingRefresh, kPacingCaptured, kPacingUnique}) {
                                for (bool rateKnown : {false, true}) {
                                    for (bool marker : {false, true}) {
                                        PresentCapInput input{configured, monitor,
                                            rateKnown ? capture : 0.0, pacing, vsync, marker, 141.0};
                                        const auto decision = ChoosePresentCap(input);
                                        double expected = 0.0;
                                        if (!vsync && pacing == kPacingRefresh) {
                                            if (marker) expected = 141.0;
                                            else if (configured > 0) expected = configured;
                                            else if (configured == 0) {
                                                expected = !rateKnown || capture <= monitor - 3.0
                                                    ? monitor - 3.0 : monitor;
                                            }
                                        }
                                        Require(decision.effectiveHz == expected,
                                                "cap matrix must preserve explicit caps, marker priority and bypasses");
                                        const auto wait = ChoosePresentWaitPoint(pacing, lowLatency);
                                        Require(wait == (pacing != kPacingRefresh ? PresentWaitPoint::None
                                                : lowLatency ? PresentWaitPoint::BeforeCapture : PresentWaitPoint::BeforeRender),
                                                "low latency only changes the location of the same refresh-mode wait");
                                        ++combinations;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        Require(ChoosePresentCap({0, 60.0, 120.0}).effectiveHz == 60.0,
                "capture faster than the monitor still has an automatic display ceiling");
        Require(ChoosePresentCap({0, 180.0, 120.0}).effectiveHz == 177.0,
                "a real 180Hz display must retain 177Hz for a 120fps source");
        Require(ChoosePresentCap({0, 119.998, 10000000.0 / 83333.0}).effectiveHz == 119.998,
                "fractional MF and display rates must not disable Auto");
        Require(ChoosePresentCap({0, 120.0, 0.0}).effectiveHz == 117.0,
                "unknown MF rate is not the user's requested or measured capture rate");
        Require(ChoosePresentCap({0, 120.0, 120.0}).effectiveHz == 120.0,
                "a retained operational hint for missing MF metadata cannot remove Auto");
        for (double bad : {0.0, 1.0, 49.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
            const auto decision = ChoosePresentCap({0, bad, 120.0});
            Require(decision.effectiveHz == 0.0 && decision.reason == PresentCapReason::UnknownDisplay,
                    "unknown display refresh must be reported honestly without inventing a refresh rate");
            Require(ChoosePresentCap({95, bad, 120.0}).effectiveHz == 95.0,
                    "manual caps do not depend on querying display refresh");
        }
        Require(ChoosePresentCap({0, 120.0, 0.0, kPacingRefresh, false, true, 117.0}).effectiveHz == 117.0,
                "the legacy marker default remains authoritative");
        std::cout << "PASS Present cap transition, " << combinations
                  << " combinations, fractional/unknown rates and wait locations\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
