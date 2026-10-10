# FPS telemetry and automatic present-cap validation

Investigation date: 2026-10-10. Hardware measurements used baseline `9bb810d`
and the two-stage candidate. Raw traces and local diagnostic tools remain in
ignored build directories; they are not distributed with NitLink.

## FPS definitions and the HUD regression

The old HDR and SDR HUD selected nonzero Present FPS in source pacing,
otherwise nonzero Content FPS, otherwise Capture FPS. A static and a moving
picture could therefore use different measurements under one label. A real
zero Present rate could also be replaced with an unrelated nonzero rate.

| Metric | Counter and sampling | Interpretation |
| --- | --- | --- |
| Capture FPS | `FrameBuffer::GetFramesWritten()`; accepted capture callbacks increment the write counter. | Frames delivered into the application buffer, including duplicate pictures. Placeholder-filtered samples are excluded. It is not the selected format or HDMI refresh rate. |
| Content FPS (est.) | `m_uniqueFrameCount`, incremented for GPU FrameDiffer change verdicts on newly consumed capture frames. | Changes estimated from consumed pictures, not the game's renderer FPS. Static content can correctly be zero. |
| Present FPS | `m_presentCount`, incremented once after a run-loop `EndFrame()` returns `S_OK`. | Successful DXGI submissions, including repeated pictures, cadence holds, keepalives and status/settings backgrounds. It is not physical scan-out or game FPS. |

All three counters use the same real elapsed sampling interval, at least one
second. Integer display can show 119 for a source running near 120. Decreasing
counters produce zero deltas instead of unsigned underflow; subsequent windows
recover normally. Invalid intervals produce zero and excessive rates saturate
before conversion to `uint32_t`. A missing buffer does not invent deliveries.
Unchanged counters decay to zero.

The HUD primary value and sparkline always show Present rate. Capture FPS and
Content FPS (est.) remain visible below it, in English and Traditional Chinese.
Zero is valid; an unavailable differ displays `--` for Content. Existing No
Signal presentation is retained. The sparkline accommodates rates above 70 FPS.
There is no new display toggle or relocation of these metrics into F1.

`FrameBuffer::GetUniqueFramesWritten()` is a separate CPU hash measurement and
does not supply Content FPS. Buffer dropped counts unread frames overwritten
by new deliveries; it is different from MF arrival rate and ETW Dropped.

| Present pacing | Render decision | HUD primary value |
| --- | --- | --- |
| Display refresh | Every render iteration, subject to VSync and the present cap. | Successful Present FPS |
| Capture rate | Newly delivered capture frames, with existing status/keepalive paths. | Successful Present FPS |
| Source frame rate | Changed content plus existing still-picture cadence holds and keepalives. | Successful Present FPS |

The old source-paced HUD already preferred nonzero Present FPS. Its reported
single-digit or 63-64 FPS during motion cannot be attributed entirely to the
Content/Capture fallback. FrameDiffer and SourceCadence were not retuned.
An earlier telemetry-only hardware window with VSync on had Capture119,
Present119 and Content0-17 without additional buffer/readback drops. It supports
independent measurements, not the absence of drops in every other window.

## Negotiated MF rates

After the existing negotiation, `GetCurrentMediaType()` supplies the actual
dimensions, subtype and `MF_MT_FRAME_RATE`. Failure to read the actual media
type aborts opening. A missing, zero or invalid rate is published as unknown
(`0/1`), rather than retaining the requested rate.

Before a successful format publication, and after the session is cleared,
public dimensions and rate are unknown. The internal 4K60 negotiation defaults
are not exposed as a negotiated result, and no stale pacing hint is reused.

Native numerator/denominator values are preserved, including GC553Pro's
`10000000/83333`, approximately 120.00048 FPS. F1 displays the negotiated ratio
to two decimal places, or `-- FPS` when unknown. Capture FPS remains independently
measured. The request, negotiated media type and delivery rate are distinct.

`GetPresentPacingRateHint()` preserves the selected operational rate only for
cap policy when MF omits rate metadata. That hint is never reported as
negotiated or measured FPS. The existing P010/NV12 selection, exact-ratio
override checks and HDR/SDR negotiation order are retained.

The optional `USE_DSHOW.txt` backend populates the same ratio fields from its
existing negotiated `AvgTimePerFrame`, instead of retaining the default 60/1
while updating only integer FPS. Invalid or unrepresentable periods are unknown;
DirectShow's format selection, validation and capture graph are unchanged.

An initial independent probe incorrectly requested `120/1` instead of the
exact native ratio. It fell back and obtained no samples. That probe is
excluded from compatibility and performance claims.

References: Microsoft [GetCurrentMediaType](https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsourcereader-getcurrentmediatype),
[MF_MT_FRAME_RATE](https://learn.microsoft.com/en-us/windows/win32/medfound/mf-mt-frame-rate-attribute)
and [Present](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present).

## Automatic cap regression and fixes

The old Auto policy kept `monitorHz - 3` only when it was at least the source
rate hint; otherwise it called `SetPresentCap(0)`. On a 120 Hz display, switching
capture from 60 to 120 FPS changed the cap from 117 Hz to uncapped. Sources
faster than the display had the same problem. This fallback already existed
upstream and was documented; it was not introduced by the telemetry fix.
Explicit Off remains an intentional, supported choice.

The final policy keeps the original headroom when it accommodates the source,
and uses the queried monitor refresh otherwise. It does not hard-code a display
rate. A 180 Hz monitor with capture120 still selects 177 Hz; a 60 Hz monitor
with capture120 selects 60 Hz.

The other two corrections are confined to the existing wait mechanism:

- `BeginFrame(true)` now uses `WaitForFrameReady()`, so Low Latency off honors
  the cap. LL on still waits before reading the freshest capture frame; off
  still waits after selecting the frame. Source/capture pacing retains its
  arrival wait without an additional refresh wait.
- The cap advances a release deadline rather than starting a full interval at
  the previous Present return. Work stays inside the interval. Missing an entire
  interval rebases the deadline instead of issuing catch-up submissions. A cap
  change clears the deadline; reapplying the same rate leaves it intact.

The existing adaptive sleep/yield wait is retained. No fixed Sleep, frame
generation, VSync flags, buffer count or maximum-frame-latency change was added.
VSync, explicit Off, manual 30-1000 Hz caps and `VRR_CAP.txt` priority are
retained. The marker parser still defaults to 117 Hz for empty/invalid contents
and accepts its original 30-1000 Hz range. VSync and source/capture pacing bypass
the cap. An unusable or below-50 Hz monitor query retains the legacy uncapped
behavior and now reports that reason rather than guessing Hz.

The waitable object reports queue readiness; it does not guarantee a display
refresh limit for immediate tearing submissions. Short successful waits do not
establish a broken object. See Microsoft's [waitable-object documentation](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject)
and SyncInterval=0 behavior in [Present](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present).

The existing two-second diagnostic distinguishes cap reasons, configured and
effective rate, actual MF metadata, S_OK/status/failure, wait counts/errors,
buffer/readback deltas, GPU time and F1 visibility. There are no per-frame logs.
The first window and detected counter decreases invalidate drop deltas.
`compMode=1` is DXGI's overlay enum and alone does not prove Independent Flip.

## Hardware validation

Setup: Nintendo Switch 2 -> AVerMedia GC553Pro -> NitLink, 1080p SDR with the
120 Hz output option enabled. Switch and Windows VRR were off. Test host:
Windows 11 Pro, version 10.0.26300, NVIDIA RTX 3060 Ti.

Windows `EnumDisplaySettingsW(ENUM_CURRENT_SETTINGS)` reported 120 Hz for the
window's monitor. `QueryDisplayConfig` reported `119998/1000 = 119.998 Hz` for
both path and signal refresh. The application retains its original integer
refresh query for policy. The monitor was not configured at 180 Hz.

Read-only NVAPI DRS initially showed G-SYNC permitted for fullscreen/windowed
applications. After the operator disabled it, the query confirmed mode/request
0 and fixed-refresh override 4. The comparisons below used that off state.
Driver settings were queried, not modified; see NVIDIA's [setting definitions](https://github.com/NVIDIA/nvapi/blob/main/NvApiDriverSettings.h).

The old candidate still produced Capture119, Content59 and Present2044-2282
with G-SYNC off. Average wait was 0.000532-0.000569 ms and Present API time
0.043-0.051 ms. Buffer/readback totals did not increase in that window. The
runaway was not dependent on G-SYNC being enabled. Earlier G-SYNC-permitted
observations also exceeded 1800 FPS, but are not mixed into the A/B comparison.

Each capture change waited for MF reopen and positive deliveries. Final MF
readback was **1920x1080 @ 60/1 NV12** for 60, and
**1920x1080 @ 10000000/83333 NV12** for 120. Capture values below come from sample
counters, not negotiated-rate attributes.

| Approximately 12-second window | Effective cap | Capture | Content (est.) | Present | New buffer dropped / readback skips |
| --- | --- | ---: | ---: | ---: | ---: |
| Before fix: 120, refresh, LL on, VSync off | 0, old Auto fallback | 119 | 59 | **2044-2282** | 0 / 0 |
| Intermediate 60 candidate retaining old clock, F1 open | 117, Auto | 59-60 | 59-60 | 94-102 | 3 / 0 |
| Final 60, Auto, F1 closed | 117 | 59-60 | 59-60 | **116-117** | 0 / 0 |
| Final 60, Auto, F1 open | 117 | 59-60 | 59-60 | **116-117** | 0 / 0 |
| Final 120, Auto, F1 transition window | 120 | 119-120 | 17-60 | **119-120** | 0 / 0 |
| Final 120, Auto, F1 closed and foreground | 120 | 119-120 | 59-118 | **117-120** | 3 / 0 |
| Final 120, Auto, LL off | 120 | 119-120 | 0-69 | **119-120** | **167 / 0** |
| Final 120, explicit Off, F1 closed | 0, intentionally uncapped | 118-120 | 4-20 | **1522-1598** | 0 / 0 |
| Final 120, explicit Off, F1 open | 0, intentionally uncapped | 119-120 | 2-4 | **1462-1563** | 0 / 0 |
| Final 120, manual95 | 95 | 119-120 | 11-78 | **94-95** | **300 / 0** |
| Final 120, VSync on | 0, synchronized bypass | 119-120 | 13-42 | **119-120** | 0 / 0 |
| Final 120, captured pacing | 0, capture-paced bypass | 119-120 | 1-53 | **119-120** | 0 / 0 |
| Final 120, unique pacing | 0, source-paced bypass | 119-120 | 1-5 | **59-60** | 0 / **2** |

Unless indicated otherwise, final rows use refresh pacing, LL on and VSync off.
Drop values sum all valid diagnostic windows, not lifetime totals. F1 state
comes from `settingsVisible`, not a file name. Both visible/hidden settings
remained capped in the 120 transition window.

Negative and transitional observations are retained:

- A cap-only intermediate120 candidate still presented 104-109 FPS with cap120,
  Capture119-120 and 23-29 additional overwrites per two seconds. Its roughly
  8.15 ms wait plus work produced 9.2-9.5 ms loops, supporting the clock fix.
- A later restore window had Capture111-120, Present114-120 and 13 new overwrites
  with cap120 unchanged. Another F1-visible window added 78 overwrites. The
  patch does not establish zero capture jitter at all times.
- An initial final-candidate window had Capture0 and `DXGI_STATUS_OCCLUDED`.
  It is excluded from capture performance comparisons, and confirms that the
  status was not counted as S_OK.
- LL off's 167 overwrites and unique pacing's two readback skips remain
  unexplained in full. Reading before waiting may contribute to LL-off
  overwrites, but this is not a complete demonstrated cause.
- manual95's roughly 300 overwrites in 12 seconds are consistent with the
  explicit 25 FPS difference from capture120. Unique pacing's near-60 Present
  rate despite low Content retains the existing cadence hold.

Valid final capture windows had `presentOk == presents`, no non-S_OK statuses
or failures, and no wait timeout/failure/APC early return. The explicit-Off
closed-settings window recorded **18,692 S_OK submissions**, far exceeding its
capture samples, with waitable waits of 0.00061-0.00117 ms. These high FPS values
represent repeated submissions, not a multiplied HUD counter or physical
updates. Tests verify 1576 submissions/1 second and 3152/2 seconds both report
1576 FPS.

| Process measurement window | CPU, one-core % | PID 3D engine % | PID copy engine % | Mean Present API ms | Latest GPU frame ms range |
| --- | ---: | ---: | ---: | ---: | ---: |
| Before fix, 120 Auto, G-SYNC off | 153.3 | 20.54 | 7.46 | 0.043-0.051 | Not collected |
| Final 120 Auto, LL on, closed/foreground | **42.8** | 9.57 | 0.70 | 0.245 | 0.078-1.955 |
| Final 60 Auto, F1 closed | 36.4 | 11.56 | 0.76 | 0.303 | 0.524-0.667 |
| Final 60 Auto, F1 open | 41.0 | 8.22 | 0.81 | 0.318 | 0.112-3.344 |
| Final 120 Auto, LL off | 45.5 | 9.94 | 0.62 | 0.228 | 0.105-0.659 |
| Final 120 Off, F1 closed | **143.3** | 17.46 | 5.68 | 0.052 | 0.243-0.335 |
| Final 120 manual95 | 38.3 | 6.28 | 0.46 | 0.171 | 0.136-0.653 |
| Final 120 VSync on | 28.0 | 7.61 | 1.39 | 0.197 | 7.310-8.107 |
| Final 120 captured | 33.8 | 16.61 | 1.17 | 0.316 | 0.559-1.503 |
| Final 120 unique | 23.5 | 22.63 | 0.44 | 0.073 | 0.642-2.441 |

CPU is process CPU-time delta divided by wall time; 42.8% of one core is about
3.6% across the host's 12 logical processors. WebView children are excluded.
GPU engine percentages are five one-second PDH samples and must not be added.
Timestamp spans are not GPU busy percentages, especially under VSync. Game
scenes, foreground/composition and windows were not identical. These values do
not establish an exact improvement percentage or input-to-photon improvement.
In source/capture pacing, the existing `uploadMs` denominator depends on
refresh-wait counts, so zero there does not mean free uploads.

## ETW display-path evidence

The final foreground/F1-closed PresentMon 2.6.0 trace contained **1437 DXGI
events**, approximately **120.0001 events/second** over 11.966657 seconds between
the first and last event. All were **Hardware: Independent Flip**, with
**Dropped = 0** and mean `msBetweenDisplayChange = 8.33365 ms`. Concurrent
diagnostics confirmed native120 NV12, Capture119-120, cap120 and S_OK results.
This supports controlled submission and display delivery in that window; it
does not prove distinct game content per event or photon/input latency.

An earlier final-candidate composition/F1 window contained 1381 events, all
Composed: Flip, with **1359 ETW Dropped**. A transitional earlier trace contained
both Composed Flip and Independent Flip with 3165 Dropped. These remain
separate, not interpreted as MF losses or a physical 2 FPS display. DWM,
foreground and occlusion can change delivery. The patch does not force
Independent Flip or alter driver/VRR policy.

## Automated validation

| Revision tested | Release build | Full CTest |
| --- | --- | --- |
| Upstream baseline `9bb810d` | PASS | 22/23 |
| First-stage telemetry candidate | PASS, clean build | 23/24 |
| Two-stage candidate | PASS, clean build | 25/26 |
| Final review on original baseline | PASS, x64 clean build | 25/26 |
| Final review integrated with upstream `d522bdc`, isolated source copy | PASS, x64 clean build | 26/27 |
| Final worktree including optional DirectShow rate readback | PASS, x64 clean build | 26/27 |

Every recorded incomplete suite has the same failure:
`webview_runtime_security`, assertion `restarted host reloads the packaged menu`.
Its assertion is not changed, skipped or disabled. Six added English/Traditional
Chinese negotiated-rate UI checks pass before that existing failure. This is a
reproduced local baseline failure, not a claim that every upstream CI fails.

The final review also built the two WebView targets from unmodified upstream
`d522bdc` in an isolated source copy. Its targeted run reproduced the same
restart assertion; `webview_settings_layout` passed. That upstream change adds
one CTest to the final suite without modifying FPS or renderer behavior.
The first integrated run took 81.17 seconds. After the optional DirectShow
readback correction, the final worktree passed a new x64 clean Release build
and ran all 27 tests in 68.20 seconds: 26 passed, with the same known failure.
The telemetry-only isolated source also passed an x64 clean Release build and
both FPS/MF regression tests. Only validation prose was updated after the
final clean build; no executable source changed.

- `frame_rate_stats`: still -> first motion -> sustained motion -> still for
  all three pacing modes; zero capture/content/Present; keepalive; missing
  differ; resets; elapsed intervals; saturation; buffer overwrites; both locales;
  and high successful submission rates. The old HUD selection reproduces the
  inconsistent-metric failure.
- `present_cap_policy`: **1152 combinations** covering capture60/120,
  monitor60/120/144/180, Auto/Off/manual, VSync, LL, three pacing modes, known or
  unknown MF rates and marker presence. Additional cases include 60 -> 120 ->
  60, source > monitor, fractional rates, retained hints and invalid monitor
  metadata. The old fallback fails `1080p60 -> 1080p120 must not turn Auto into
  uncapped Present`.
- `present_wait_paths`: an 8x8 offscreen WARP target with a signaled queue-ready
  event exercises real renderer waits. Both LL locations enforce cap; Off,
  VSync, marker priority, no second source wait, consume-once counters, deadline
  reset and missed-slot behavior are checked. No capture device or OS change
  is involved.
- `p010_native_negotiation`: actual120, actual60, fractional/native ratios and
  missing/invalid rates, preserving the separate operational hint; unpublished
  and cleared sessions expose unknown dimensions/rates.
  DirectShow interval readback has native-ratio and invalid-period regressions.
  Existing HDR, P010 selector, VSync, No Signal, placeholder and format-policy checks
  also pass.

Reproduce from an **x64 Visual Studio developer command prompt**, with CMake
and the repository's normal dependencies installed:

```bat
cmake -S . -B build/fps-validation -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DNITLINK_TEST_WEBVIEW_RUNTIME=ON
cmake --build build/fps-validation --clean-first --parallel 4
ctest --test-dir build/fps-validation -C Release --output-on-failure
```

## Remaining hardware limits and reproduction

Statistics tests do not validate GPU classification quality. No synchronized
source/picture/differ fixture establishes that the motion-onset single-digit
FPS or every 63-64 FPS observation was purely HUD behavior. LL-off overwrites,
occasional capture jitter and readback skips need separate controlled evidence.
Physical HDR/SDR and HDMI No Signal transitions, other capture cards/monitor
modes, G-SYNC-on operation and a real marker file were not fully exercised in
this hardware round. Input-to-photon latency was not measured. Automated
compatibility passes do not imply all hardware paths pass.

1. Run the candidate from its own build directory with runtime resources and
   a configuration copy. Preserve the installed executable/configuration.
   Close competing capture applications before taking the device.
2. Keep console/display settings fixed and query the window's actual Windows
   refresh rate. Check NVIDIA G-SYNC independently of Windows VRR.
3. Select native1080p120/Auto in F1, preserving its exact ratio. Wait for reopen
   to finish; save final negotiated dimensions/rate/subtype and positive
   samples, rather than trusting selection text.
4. In one scene, record at least ten seconds each of still -> continuous motion
   -> still. Save all FPS values, differ/tile verdicts, cadence, cap reason,
   Present results, wait errors and buffer/readback deltas. Do not compare
   lifetime counters across a buffer rebuild.
5. Repeat F1 closed/open, Auto/Off/manual, both LL settings, VSync and all three
   pacing modes. Expect Content zero on unchanged pictures and independent
   Present counts during cadence holds or status presentation.
6. Use PresentMon for actual PresentMode/display events and an external
   high-speed camera for source/preview and input response. Keep ETW Dropped
   separate from capture-buffer overwrites.
7. Independently exercise HDMI loss/restoration and supported SDR/HDR switching.
   Verify existing keepalive, NV12/P010 negotiation and recovery. Do not
   substitute fixed HUD numbers or altered VRR/VSync settings for these checks.

## Change boundaries

Telemetry: `frame_rate_stats.h`, application sampling/submission counters,
overlay/localization, actual MF/DirectShow rate publication, negotiated-rate menu display
and corresponding regression tests. Pacing: `present_cap_policy.h`,
`present_cap_clock.h`, application policy/wait selection, renderer waits and
low-frequency diagnostics, two regression executables, CMake and README/config
comments. FrameDiffer shaders/thresholds/readback, SourceCadence, HDR device
pollers/protocols, native-format selection order and VSync flags are retained.
