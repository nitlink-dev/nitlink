#include "audio/capture_endpoint_match.h"

#include <iostream>

namespace {

bool Expect(bool condition, const char* label)
{
    std::cout << (condition ? "PASS " : "FAIL ") << label << '\n';
    return condition;
}

// Arbitrary example container IDs.
constexpr GUID kCardContainer = {0x5E1A7C42, 0x9B3D, 0x4F21, {0x8A, 0x6C, 0x1D, 0x2E, 0x3F, 0x40, 0x51, 0x62}};
constexpr GUID kUsbMicContainer = {0x2C7F1B90, 0x4D6E, 0x4A1B, {0x93, 0x05, 0x6F, 0x7A, 0x8B, 0x9C, 0xAD, 0xBE}};
constexpr GUID kOtherRootContainer = {0x0B5D2A4C, 0x1E2F, 0x4A6B, {0x9C, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}};

} // namespace

int main()
{
    using namespace NitLink;
    bool pass = true;

    // Built-in and virtual devices report kNullContainerId, as does the root
    // device node on the systems checked.
    const std::vector<CaptureEndpointInfo> endpoints = {
        {L"Microphone (Virtual Microphone)", kNullContainerId},
        {L"Microphone (2- USB Podcast Microphone)", kUsbMicContainer},
        {L"Microphone (Virtual Camera Audio)", kNullContainerId},
        {L"Microphone (Realtek(R) Audio)", kNullContainerId},
        {L"Digital Audio Interface (2- REDRAGON AUDIO CAPTURE)", kCardContainer},
    };

    {
        const auto c = ChooseCaptureEndpoint(endpoints, L"REDRAGON 4K HD VIDEO CAPTURE",
                                             kCardContainer, kNullContainerId);
        pass &= Expect(c.index == 4 && !c.byName && c.ambiguousContainerMatches.empty(),
                       "generic USB card pairs with its audio by physical device");
    }

    {
        auto withElgato = endpoints;
        withElgato.push_back({L"Game Capture 4K X (Elgato 4K X)", kNullContainerId});
        const auto c = ChooseCaptureEndpoint(withElgato, L"Elgato 4K X", kCardContainer,
                                             kNullContainerId);
        pass &= Expect(c.index == 5 && c.byName,
                       "a name match keeps priority (Elgato behaviour unchanged)");
    }

    pass &= Expect(ChooseCaptureEndpoint(endpoints, L"Software Webcam", kNullContainerId,
                                         kNullContainerId).index == -1,
                   "the computer / virtual-device container never pairs (no onboard or virtual mic)");

    {
        auto otherRoot = endpoints;
        otherRoot.push_back({L"Microphone (Onboard Audio)", kOtherRootContainer});
        pass &= Expect(ChooseCaptureEndpoint(otherRoot, L"Internal PCIe Capture",
                                             kOtherRootContainer, kOtherRootContainer).index == -1,
                       "a root container other than the null one never pairs either");
    }

    pass &= Expect(ChooseCaptureEndpoint(endpoints, L"Unknown Card", GUID{},
                                         kNullContainerId).index == -1,
                   "no name match and no container gives no audio, not a random mic");

    {
        const auto c = ChooseCaptureEndpoint(endpoints, L"redragon audio", GUID{},
                                             kNullContainerId);
        pass &= Expect(c.index == 4 && c.byName, "name match stays case-insensitive");
    }

    // Two endpoints on the card's container: pick none and report both.
    auto twoOnCard = endpoints;
    twoOnCard.push_back({L"Line In (2- USB Capture Mic Input)", kCardContainer});
    {
        const auto c = ChooseCaptureEndpoint(twoOnCard, L"REDRAGON 4K HD VIDEO CAPTURE",
                                             kCardContainer, kNullContainerId);
        pass &= Expect(c.index == -1 && c.ambiguousContainerMatches == std::vector<int>{4, 5},
                       "an ambiguous container picks no endpoint and lists the candidates");
    }
    {
        const auto c = ChooseCaptureEndpoint(twoOnCard, L"redragon audio", kCardContainer,
                                             kNullContainerId);
        pass &= Expect(c.index == 4 && c.byName && c.ambiguousContainerMatches.empty(),
                       "a name match still wins when the container is ambiguous");
    }

    return pass ? 0 : 1;
}
