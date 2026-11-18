#pragma once

// Choosing which Windows audio capture endpoint carries the capture card's
// HDMI audio. Header-only so the rule can be unit-tested without devices.

#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace NitLink {

struct CaptureEndpointInfo {
    std::wstring friendlyName;
    GUID         containerId{};  // all-zero when the endpoint reports none
};

// Container ID that Windows reports for the computer itself. Built-in devices
// (onboard audio, GPU HDMI audio, network adapters) and software or virtual
// devices (virtual cameras and microphones) all carry it, and the root device
// node reports it too, so it never identifies one physical device.
inline constexpr GUID kNullContainerId =
    {0x00000000, 0x0000, 0x0000, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};

// A container ID identifies one physical device only when it is present, is
// not kNullContainerId, and is not the root device node's container
// (computerContainer, which equals kNullContainerId on the systems checked;
// it is compared separately in case a system reports another value).
inline bool IsPairableContainer(const GUID& id, const GUID& computerContainer)
{
    return !IsEqualGUID(id, GUID{}) &&
           !IsEqualGUID(id, kNullContainerId) &&
           !IsEqualGUID(id, computerContainer);
}

struct CaptureEndpointChoice {
    int  index  = -1;     // endpoint to capture from, or -1 for none
    bool byName = false;  // chosen by the name hint rather than the container
    // Set when two or more endpoints share the video device's container.
    // No endpoint is chosen then; the candidates are listed for the log.
    std::vector<int> ambiguousContainerMatches;
};

// Picks the endpoint to capture from.
//   1. Case-insensitive substring match on the name hint. This is the
//      original rule (Elgato and AverMedia name their audio after the
//      card) and keeps priority so those cards behave exactly as before.
//   2. Otherwise, the endpoint on the same physical device as the video
//      (same container ID). Generic USB cards name their audio function
//      differently from their video one, e.g. "REDRAGON 4K HD VIDEO CAPTURE"
//      vs "Digital Audio Interface (REDRAGON AUDIO CAPTURE)", but Windows
//      gives both functions of one USB device the same container ID. When
//      more than one endpoint shares that container (for example a card with
//      an extra microphone input), none is chosen.
inline CaptureEndpointChoice ChooseCaptureEndpoint(
    const std::vector<CaptureEndpointInfo>& endpoints,
    const std::wstring& nameHint,
    const GUID& videoContainer,
    const GUID& computerContainer)
{
    CaptureEndpointChoice choice;
    if (!nameHint.empty()) {
        std::wstring hint = nameHint;
        std::transform(hint.begin(), hint.end(), hint.begin(), ::towlower);
        for (size_t i = 0; i < endpoints.size(); ++i) {
            std::wstring name = endpoints[i].friendlyName;
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            if (name.find(hint) != std::wstring::npos) {
                choice.index = static_cast<int>(i);
                choice.byName = true;
                return choice;
            }
        }
    }
    if (IsPairableContainer(videoContainer, computerContainer)) {
        std::vector<int> matches;
        for (size_t i = 0; i < endpoints.size(); ++i) {
            if (IsEqualGUID(endpoints[i].containerId, videoContainer)) {
                matches.push_back(static_cast<int>(i));
            }
        }
        if (matches.size() == 1) {
            choice.index = matches.front();
        } else if (matches.size() > 1) {
            choice.ambiguousContainerMatches = std::move(matches);
        }
    }
    return choice;
}

} // namespace NitLink
