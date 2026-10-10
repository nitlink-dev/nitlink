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

## First-stage automated validation

The baseline Release build passed with 22/23 CTest cases. The first-stage
telemetry candidate passed its clean Release build with 23/24 cases. Both
had the same existing `webview_runtime_security` failure:
`restarted host reloads the packaged menu`. That assertion is not changed,
skipped or disabled. The six added English/Traditional Chinese integer,
fractional and unknown negotiated-rate checks pass before that failure.

`frame_rate_stats` covers still -> motion -> still in all pacing modes,
zero rates, keepalives, missing differ, resets, real elapsed windows, saturation,
and deliveries overwritten before consumption. `p010_native_negotiation`
covers actual120/60, native and fractional ratios, missing/invalid MF rates,
and unpublished/cleared metadata while retaining the separate cap-policy hint.
These tests do not validate GPU
classification quality or physical HDR/No Signal transitions. Those hardware
paths and input-to-photon latency still need dedicated validation.
