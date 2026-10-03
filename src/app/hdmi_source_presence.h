#pragma once

#include <chrono>

namespace NitLink {

// Identity/display refresh keeps the activity clock; selecting a game starts
// a new activity.
constexpr std::chrono::system_clock::time_point HdmiSourceActivityStartTime(
    std::chrono::system_clock::time_point previous, bool preserveStartTime,
    std::chrono::system_clock::time_point now) noexcept {
    return preserveStartTime && previous.time_since_epoch().count() != 0 ? previous : now;
}

} // namespace NitLink
