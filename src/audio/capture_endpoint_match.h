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

// Container ID Windows gives software / virtual devices (virtual cameras and
// microphones). Every such device shares it, so it says nothing about pairing.
inline constexpr GUID kNullContainerId =
    {0x00000000, 0x0000, 0x0000, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};

// A container ID identifies one physical device only when it is not
// missing, not the virtual-device container, and not the PC's own container
// (shared by every built-in device: motherboard audio, GPU HDMI audio, and
// possibly an internal PCIe capture card).
inline bool IsPairableContainer(const GUID& id, const GUID& computerContainer)
{
    return !IsEqualGUID(id, GUID{}) &&
           !IsEqualGUID(id, kNullContainerId) &&
           !IsEqualGUID(id, computerContainer);
}

// Returns the index of the endpoint to capture from, or -1.
//   1. Case-insensitive substring match on the name hint. This is the
//      original rule (Elgato and AverMedia name their audio after the
//      card) and keeps priority so those cards behave exactly as before.
//   2. Otherwise, the endpoint on the same physical device as the video
//      (same container ID). Generic USB cards name their audio function
//      differently from their video one, e.g. "REDRAGON 4K HD VIDEO CAPTURE"
//      vs "Digital Audio Interface (REDRAGON AUDIO CAPTURE)", but Windows
//      gives both functions of one USB device the same container ID.
inline int ChooseCaptureEndpoint(const std::vector<CaptureEndpointInfo>& endpoints,
                                 const std::wstring& nameHint,
                                 const GUID& videoContainer,
                                 const GUID& computerContainer)
{
    if (!nameHint.empty()) {
        std::wstring hint = nameHint;
        std::transform(hint.begin(), hint.end(), hint.begin(), ::towlower);
        for (size_t i = 0; i < endpoints.size(); ++i) {
            std::wstring name = endpoints[i].friendlyName;
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            if (name.find(hint) != std::wstring::npos) return static_cast<int>(i);
        }
    }
    if (IsPairableContainer(videoContainer, computerContainer)) {
        for (size_t i = 0; i < endpoints.size(); ++i) {
            if (IsEqualGUID(endpoints[i].containerId, videoContainer)) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

} // namespace NitLink
