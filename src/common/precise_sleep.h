#pragma once

#include <windows.h>
#include <chrono>
#include <thread>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace NitLink {

// Sleep for `duration` with roughly 1 ms accuracy, without changing the whole
// process's timer resolution (timeBeginPeriod). Since Windows 10 2004 a plain
// Sleep / sleep_for in a process that never asked for a finer timer rounds up
// to the default ~15.6 ms tick: measured on a 240 Hz setup, "1 ms" sleeps in
// the render loop took ~15 ms and held it to ~70 iterations/s against its
// 237 Hz present cap.
//
// A high-resolution waitable timer (Windows 10 1803+) fixes only the waits
// that use this helper; every other Sleep in NitLink (capture-card control
// traffic, pollers, retries) keeps its existing timing. If the timer cannot
// be created (older Windows), this falls back to sleep_for.
inline void PreciseSleep(std::chrono::nanoseconds duration)
{
    if (duration <= std::chrono::nanoseconds::zero()) return;

    struct Timer {
        HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr,
                                               CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                               TIMER_ALL_ACCESS);
        ~Timer() { if (handle) CloseHandle(handle); }
    };
    thread_local Timer timer;

    if (timer.handle) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(duration.count() / 100);  // relative, 100 ns units
        if (due.QuadPart == 0) due.QuadPart = -1;
        if (SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0) &&
            WaitForSingleObject(timer.handle, INFINITE) == WAIT_OBJECT_0) {
            return;
        }
    }
    std::this_thread::sleep_for(duration);
}

} // namespace NitLink
