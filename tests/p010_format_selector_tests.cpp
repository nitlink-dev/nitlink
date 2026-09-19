#include "capture/p010_format_selector.h"

#include <iostream>
#include <vector>

namespace {

bool Expect(bool condition, const char* name)
{
    if (condition) return true;
    std::cerr << "FAIL: " << name << '\n';
    return false;
}

} // namespace

int main()
{
    using namespace NitLink;
    bool pass = true;

    const std::vector<P010Candidate> gamingModes = {
        {2560, 1440, 30},
        {1920, 1080, 60},
        {1280, 720, 60},
    };

    auto result = SelectGamingP010Candidate(gamingModes, 2560, 1440, 30);
    pass &= Expect(result.index == 0 &&
                   result.reason == P010SelectionReason::ExactTarget,
                   "exact target");

    result = SelectGamingP010Candidate(gamingModes, 2560, 1440, 60);
    pass &= Expect(result.index == 0 &&
                   result.reason == P010SelectionReason::TargetResolution,
                   "target resolution closest FPS");

    result = SelectGamingP010Candidate(gamingModes, 3840, 2160, 60);
    pass &= Expect(result.index == 1 && result.usedHighFpsTier &&
                   result.reason == P010SelectionReason::HighFpsTier,
                   "gaming high-FPS tier");

    const std::vector<P010Candidate> lowFpsModes = {
        {2560, 1440, 30},
        {1920, 1080, 30},
    };
    result = SelectGamingP010Candidate(lowFpsModes, 3840, 2160, 60);
    pass &= Expect(result.index == 0 && !result.usedHighFpsTier &&
                   result.reason ==
                       P010SelectionReason::HighestResolutionFallback,
                   "highest-resolution fallback");

    result = SelectGamingP010Candidate({}, 1920, 1080, 60);
    pass &= Expect(result.index == static_cast<size_t>(-1) &&
                   result.reason == P010SelectionReason::None,
                   "empty candidates");

    std::cout << "P010 format selector " << (pass ? "PASS" : "FAIL") << '\n';
    return pass ? 0 : 1;
}
