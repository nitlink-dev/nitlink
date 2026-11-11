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
constexpr GUID kComputer = {0x0B5D2A4C, 0x1E2F, 0x4A6B, {0x9C, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}};

} // namespace

int main()
{
    using namespace NitLink;
    bool pass = true;

    const std::vector<CaptureEndpointInfo> endpoints = {
        {L"Microphone (Virtual Microphone)", kNullContainerId},
        {L"Microphone (2- USB Podcast Microphone)", kUsbMicContainer},
        {L"Microphone (Virtual Camera Audio)", kNullContainerId},
        {L"Microphone (Realtek(R) Audio)", kComputer},
        {L"Digital Audio Interface (2- REDRAGON AUDIO CAPTURE)", kCardContainer},
    };

    pass &= Expect(ChooseCaptureEndpoint(endpoints, L"REDRAGON 4K HD VIDEO CAPTURE",
                                         kCardContainer, kComputer) == 4,
                   "generic USB card pairs with its audio by physical device");

    auto withElgato = endpoints;
    withElgato.push_back({L"Game Capture 4K X (Elgato 4K X)", kComputer});
    pass &= Expect(ChooseCaptureEndpoint(withElgato, L"Elgato 4K X", kCardContainer, kComputer) == 5,
                   "a name match keeps priority (Elgato behaviour unchanged)");

    pass &= Expect(ChooseCaptureEndpoint(endpoints, L"Software Webcam", kNullContainerId,
                                         kComputer) == -1,
                   "virtual-device container never pairs with an unrelated virtual mic");

    pass &= Expect(ChooseCaptureEndpoint(endpoints, L"Internal PCIe Capture", kComputer,
                                         kComputer) == -1,
                   "the PC's own container never pairs with the motherboard mic");

    pass &= Expect(ChooseCaptureEndpoint(endpoints, L"Unknown Card", GUID{},
                                         kComputer) == -1,
                   "no name match and no container gives no audio, not a random mic");

    pass &= Expect(ChooseCaptureEndpoint(endpoints, L"redragon audio", GUID{},
                                         kComputer) == 4,
                   "name match stays case-insensitive");

    return pass ? 0 : 1;
}
