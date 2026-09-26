#pragma once

#include <atomic>
#include <string>
#include <thread>
#include "gc553pro_hdr_source.h"

namespace NitLink {

// Background-thread monitor for HDMI source HDR state transitions.
//
// Once per second, opens a transient DirectShow graph against the Elgato
// capture device, reads the CEA-861 Dynamic Range InfoFrame from the
// IKsPropertySet vendor-specific property (the same path used by
// ReadElgatoHDRSource), and compares the decoded EOTF to the last-known
// state. When it differs, the poller flips an atomic "has update" flag
// that the main thread picks up on its next run-loop iteration and acts
// on (updates Application::m_sourceIsHDR10, pushes the new state to the
// renderer, calls ReconcileCaptureFormat to swap the capture pipeline).
//
// Why a thread and not a main-thread poll? ReadElgatoHDRSource opens a
// DirectShow filter graph and that's a 50 to 100 ms blocking call. Doing it
// on the main thread every second would cause a periodic frame hitch.
// The thread runs detached from frame pacing; the main thread only ever
// reads atomics.
//
// Why 1 Hz? PS5 takes 2 to 5 seconds to fully boot a game's HDR signal,
// so 1 Hz catches every real transition with several samples to spare.
// Faster polling buys nothing and burns CPU on a DirectShow open/close
// dance that returns no new information.
//
// Lifecycle:
//   - Constructed in Application::Initialize before run loop starts.
//   - Start(deviceName) launches the thread. Captures deviceName by
//     value into the worker. Returns immediately.
//   - Application polls HasUpdate() every iteration. When true, calls
//     AcceptUpdate() to read the new state and clear the flag atomically.
//   - Stop() (called by destructor) signals the thread to exit, joins.
//
// Threading guarantees:
//   - Start / Stop must be called from the same thread (main).
//   - HasUpdate / AcceptUpdate are lock-free reads of atomics; safe to
//     call from any thread including main during run loop.
//   - The worker is the sole writer to m_isHDR10 and m_hasUpdate.
//   - The main thread (via AcceptUpdate) is the sole clearer of
//     m_hasUpdate (using std::atomic::exchange: atomic compare-clear).
//
// Failure modes:
//   - ReadElgatoHDRSource returns propertyAccessible=false: card doesn't
//     support the query (e.g. 4K S). Poller keeps trying but never
//     updates state. Application can use HasUpdate's continued false
//     return as a heuristic to skip starting the poller after first
//     failed probe (see Start() comment).
//   - ReadElgatoHDRSource returns propertyAccessible=true but the read
//     itself failed (driver hiccup, signal mid-handshake): poller leaves
//     last-known-good state intact. No false transitions reported.
class HDRSourcePoller {
public:
    HDRSourcePoller();
    ~HDRSourcePoller();

    // Non-copyable, non-movable: owns a thread + atomics by identity.
    HDRSourcePoller(const HDRSourcePoller&) = delete;
    HDRSourcePoller& operator=(const HDRSourcePoller&) = delete;
    HDRSourcePoller(HDRSourcePoller&&) = delete;
    HDRSourcePoller& operator=(HDRSourcePoller&&) = delete;

    // Start the background polling thread. Captures `deviceName` by value
    // (the worker holds its own copy). `initialIsHDR10` seeds the
    // last-known state so the FIRST transition the poller reports is a
    // real change, not a redundant report of the init-time state.
    //
    // No-op if already running. Safe to call from the main thread only.
    //
    // Recommended pattern: skip calling Start at all if the init-time
    // ReadElgatoHDRSource returned propertyAccessible=false. The 4K S
    // never recovers that capability, so the poller would just burn
    // CPU on a DirectShow round-trip that always fails.
    // A successful init-time SDR probe also establishes query support.
    void Start(const std::wstring& deviceName, bool initialIsHDR10,
               bool initialPropertyAccessible = false);

    // GC553Pro uses a distinct decoder and retains Unknown until a valid
    // source state remains uncontradicted for the 750 ms settle window.
    void StartGc553Pro(const std::wstring& deviceName,
                       Gc553ProSourceHdrState initialState);

    // Signal the worker to exit and join. Idempotent; safe to call
    // multiple times. Called automatically by the destructor.
    void Stop();

    // Cheap atomic check: does the poller have a state change to
    // report that the main thread hasn't picked up yet?
    bool HasUpdate() const { return m_hasUpdate.load(std::memory_order_acquire); }

    // Read the latest source state and clear the update flag in one
    // atomic step. Returns true iff there was actually an update
    // pending; in that case *outIsHDR10 is filled with the new state.
    // Returns false (and leaves *outIsHDR10 untouched) if there was
    // no update, so this is safe to call unconditionally per iteration.
    bool AcceptUpdate(bool* outIsHDR10);
    bool AcceptGc553ProUpdate(Gc553ProSourceHdrState* outState);

    // For diagnostics / logging only. Returns true when the worker
    // thread is alive (between Start and Stop).
    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }

private:
    // Worker entry point. Loops on m_stop, sleeping for ~1 second
    // between probes. The sleep is broken into 100ms chunks so Stop()
    // doesn't have to wait up to a full second for the worker to
    // notice the stop flag.
    void PollerThreadMain(std::wstring deviceName, bool hadAccessibleProbe);
    void Gc553ProThreadMain(std::wstring deviceName);

    std::thread       m_thread;
    std::atomic<bool> m_stop{false};        // worker exits when set
    std::atomic<bool> m_running{false};     // worker is alive
    std::atomic<bool> m_isHDR10{false};     // last-known source state
    std::atomic<bool> m_hasUpdate{false};   // change since last AcceptUpdate
    std::atomic<Gc553ProSourceHdrState> m_gcState{Gc553ProSourceHdrState::Unknown};
};

} // namespace NitLink
