#pragma once

#include "capture/hdmi_raw_timing.h"
#include "capture/hdmi_source_vrr.h"
#include <string>
#include <string_view>

namespace NitLink {

enum class HdmiConfirmedRange { Unknown, Sdr, Hdr };

// Source EOTF, independent of HDR10 display-output preference/capture subtype.
constexpr HdmiConfirmedRange ConfirmedHdmiSourceRange(
    bool detectionAvailable, bool knownSourceState, bool sourceIsHdr10) noexcept {
    if (!detectionAvailable || !knownSourceState) return HdmiConfirmedRange::Unknown;
    return sourceIsHdr10 ? HdmiConfirmedRange::Hdr : HdmiConfirmedRange::Sdr;
}

inline std::wstring FormatHdmiSourceTiming(const HdmiSourceTiming& timing) {
    if (!timing.available || !timing.width || !timing.height || !timing.refreshRateHz100) return {};
    auto frequency = std::to_wstring(timing.refreshRateHz100 / 100);
    const auto fraction = timing.refreshRateHz100 % 100;
    if (fraction) {
        frequency += L"." + std::to_wstring(fraction / 10) + std::to_wstring(fraction % 10);
        if (frequency.back() == L'0') frequency.pop_back();
    }
    return std::to_wstring(timing.width) + L"x" + std::to_wstring(timing.height) +
        L" @ " + frequency + L"Hz";
}

inline std::wstring ComposeHdmiWindowTitle(std::wstring_view identity,
    std::wstring_view signal, std::wstring_view confirmedRange,
    HdmiSourceVrrState sourceVrr = HdmiSourceVrrState::Unknown) {
    std::wstring title = L"NitLink";
    if (!identity.empty()) title += L" - " + std::wstring(identity);
    if (!signal.empty()) title += L" - " + std::wstring(signal);
    if (!confirmedRange.empty()) title += L" [" + std::wstring(confirmedRange) + L"]";
    if (sourceVrr == HdmiSourceVrrState::Enabled) title += L" [VRR]";
    return title;
}

// Viewer details contain the shared effective source identity; state is signal.
inline std::wstring ComposeHdmiPresenceSignal(std::wstring_view signal,
    std::wstring_view confirmedRange, HdmiSourceVrrState sourceVrr = HdmiSourceVrrState::Unknown) {
    std::wstring state;
    const auto append = [&state](std::wstring_view field) {
        if (field.empty()) return;
        if (!state.empty()) state += L" · ";
        state += field;
    };
    append(signal);
    append(confirmedRange);
    if (sourceVrr == HdmiSourceVrrState::Enabled) append(L"VRR");
    return state;
}

// A selected game keeps its title/art in details, with source+signal in state.
inline std::wstring ComposeHdmiPresenceState(std::wstring_view identity,
    std::wstring_view signal, std::wstring_view confirmedRange,
    HdmiSourceVrrState sourceVrr = HdmiSourceVrrState::Unknown) {
    std::wstring state(identity);
    const auto metadata = ComposeHdmiPresenceSignal(signal, confirmedRange, sourceVrr);
    if (!metadata.empty()) {
        if (!state.empty()) state += L" · ";
        state += metadata;
    }
    return state;
}

// The application supplies only a title/Discord refresh callback. Unknown
// observations retain last valid, and this route has no capture/policy inputs.
template<class RefreshLabels>
bool ApplyHdmiSourceVrrMetadata(HdmiSourceVrrState& current,
    HdmiSourceVrrState observation, RefreshLabels&& refreshLabels) {
    if ((observation != HdmiSourceVrrState::Disabled && observation != HdmiSourceVrrState::Enabled) ||
        observation == current) return false;
    current = observation;
    refreshLabels();
    return true;
}

} // namespace NitLink
