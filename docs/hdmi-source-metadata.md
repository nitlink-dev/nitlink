# HDMI source identity and signal metadata

The identity describes the device connected to HDMI IN, not the capture card.
`GetEffectiveHdmiSourceLabel()` is shared by the window title and Discord:
manual identity, then the existing automatic detectors, then an empty identity.
Automatic Elgato 4K S detection and 4K X SPD names retain their existing behavior.

The global preferences in `nitlink.json` use its existing UTF-8 key/value format:

```ini
manual_hdmi_source = auto
manual_hdmi_source_custom =
```

Canonical values are `auto`, `ps5`, `ps4`, `switch2`, `switch`, `xbox_series`,
`xbox_one`, `pc`, and `other`. Fixed runtime labels are PS5, PS4, Switch 2,
Switch, Xbox Series X|S, Xbox One, PC, and Other. Auto supplies no override.
Missing or invalid configuration defaults to `auto`. Invalid native UI messages
are rejected. Switching capture cards does not reset these preferences.

Other accepts a trimmed custom name of at most 64 Unicode scalar values.
Controls, newlines, invalid UTF-8, and malformed UTF-16 are rejected. An empty
name displays Other. JSON escaping and DOM text insertion preserve literal text.
The F1 Source popover uses the existing select style, placing the identity picker
immediately after the manual capture format heading, before resolution/FPS/format.
Changing identity saves immediately and refreshes labels without reopening capture.

## Independent signal fields

GC553Pro timing uses the read-only `0x37` RawTiming reply: exactly 45 bytes,
`A1 2A 00 00`, 40 caller output bytes, and a whole-frame LRC sum of zero.
The confirmed little-endian 16-bit fields are outer offsets 10 (active height),
12 (active width), and 18 (nominal refresh in Hz x 100). Unknown or invalid
timing supplies no mode text. Media Foundation's capture output never substitutes
for HDMI source timing. Nominal HDMI refresh is not game content frame rate.

Source HDR uses the existing main `0x65` decoder, reader, settle window and
failure policy, unchanged. A confirmed SDR or HDR10/PQ state supplies SDR or HDR
respectively. An unavailable, unknown, or unsupported EOTF supplies neither label.
The display label uses confirmed source EOTF, including the initial probe, rather
than HDR10 output preference, tone mapping, or negotiated P010/NV12 subtype.

## GC553Pro source VRR enabled/signaling

`0x7D` (AT_Get_HdmiRX_VTEM) must return exactly 17 bytes:

| Outer offsets | Confirmed meaning |
| --- | --- |
| 0..3 | `A1 0E 00 00` response envelope |
| 4..15 | 12-byte body |
| 7 / body[3] | Empirical source VRR enabled/signaling field |
| 16 | LRC; sum of all 17 bytes modulo 256 must be zero |

Valid body[3] values map to `HdmiSourceVrrState`: 0 = Disabled, 1 = Enabled,
all other values = Unknown. No meanings are assigned to the other body fields.

Empirically validated on GC553Pro. Not a confirmed vendor SDK field definition.
Two complete, clean reversible hardware cycles contained 30 samples per phase:
OFF=0, ON_IDLE=1, ON_ACTIVE=1, OFF_RETURN=0, repeated. All 240/240 responses
were valid with no invalid formal samples; resolution stayed 1920x1080 and HDR
stayed HDR10. Nominal timing was about 59.99 Hz for OFF/RETURN and 60.00 Hz for
ON phases. This 0.01 Hz difference remains a potential timing confound in the
evidence and is not used as a detection heuristic.

ON_IDLE also returns 1. Enabled therefore means source VRR enabled/signaling,
not that refresh is varying at that instant, and not monitor/G-Sync/FreeSync
capability. TX getter `0x42` and follow/config getter `0x88` are not detection inputs.
Identity, HDR, resolution and refresh rate cannot imply VRR.

Timing and VTEM reads run at at most 1 Hz on the existing background COM/XU
owner, serialized with the unchanged HDR reads. VRR has an independent 750 ms
settle window and notification channel. Invalid, empty, stale, wrong-envelope,
bad-LRC, or unknown-value observations retain the last valid VRR state and a
pending valid candidate. They never mean Disabled. A new capture-device session
starts Unknown unless its own initial valid probe supplies a state; rollback
restores the previous session's metadata.

Only Enabled adds `[VRR]` to the title and `· VRR` to Discord signal text:

```text
NitLink - Switch 2 - 1920x1080 @ 60Hz [HDR] [VRR]
```

Without a selected game, Discord details is Switch 2 and state is
`1920x1080 @ 60Hz · HDR · VRR`. With a selected game, its name/art remain in
details/assets and source identity joins signal text in state. Metadata refreshes
retain the activity start time. Unknown fields are omitted, including HDR/SDR
when detection is unavailable.

VRR notifications refresh title and Discord only. No new user toggle or VRR
configuration exists. Manual identity, timing and VRR do not change capture
resolution/FPS, HDR detection, P010/NV12 negotiation, HDR output/tone mapping,
reopen/reconcile policy, Present Pacing, display refresh, capture rate, content
frame rate cadence, VSync, ALLOW_TEARING, present cap, or Low Latency Mode.

## Signal resynchronization presentation

After a device has reached presentable Capture, an eligible frame-delivery gap
of at least 250 ms shows `Resynchronizing signal…` / `正在重新同步訊號…`.
This reuses the existing reacquire debounce and last accepted Real frame time;
it adds no timer or minimum display duration. The first newly accepted and
uploaded Real frame restores Capture immediately. Startup and a new device
session keep the ordinary waiting presentation. A confirmed/latched No Signal
retains priority, with its existing timeouts and placeholder rules unchanged.

The presentation is shared by SDR/HDR and all devices. Detection is selective:
the no-valid-frame fallback is enabled only for GC553Pro on Media Foundation,
the verified path. It is not enabled for DirectShow, other Elgato/AVerMedia
models, or generic cards. Their buffering, scheduling and USB stalls have no
proven HDMI-transition meaning. An already known capture format reopen after
stable Capture uses the same generic hint immediately on any backend, without
waiting for 250 ms. The existing reopen decides this event; the hint never
requests reopen, reconciliation, renegotiation or changes in present policy.

Even on GC553Pro, a prolonged driver/USB delivery stall can show this harmless
presentation hint without proving a physical HDMI mode change. Legal black or
duplicate frames keep Capture alive; image content and hash activity are not
resync triggers. Failed timing/HDR/VRR reads and metadata changes alone are not
triggers. HDR/VRR retain their existing last-valid policy, so old `[HDR]`/`[VRR]`
suffixes may remain until new confirmed metadata arrives. Existing startup
black-frame and capture-format promotion hints retain their own semantics.

## Compatible last-frame hold

Separate interruptions with presentable Capture in between remain separate
presentations. No coalescing or cooldown is added.

During SignalResync only, the renderer can now redraw its existing raw capture
texture and planar SRVs underneath the localized resync text and a translucent
scrim. This requires a live capture draw that reached the backbuffer and whose
Present returned S_OK, with the same successfully uploaded texture contents.
An uploaded frame skipped by pacing is not treated as the last displayed frame.
No extra GPU texture or copy/readback is introduced. A cached Present cannot
create a live-frame receipt.

The input dimensions/format, HDR interpretation, row order, range/chroma policy,
output format/HDR/matrix, geometry, post-input path and color-expansion values
must match that successful presentation. The raw input goes through the same
conversion once per redraw; already tone-mapped output is not tone-mapped again.
Held redraws preserve the last conversion's color-expansion value; ordinary
live-frame smoothing and all user targets remain unchanged.

Capture-session invalidation, device switching, capture-resource recreation and
WRITE_DISCARD invalidate the hold receipt, including a failed/partial mapped
write. Only a new completed upload can requalify the contents for a live Present
receipt; a later draw with a stale HasFrame/upload serial cannot undo this guard.
The receipt is independent of SignalResync interruptions. A subsequent complete
upload and successful live Present replace both the old receipt and its
invalidation reason, including after an incompatible format reopen. A second
episode therefore uses the latest safely presented recovery frame; starting
a frame gap does not invalidate it. A legitimate black recovery frame remains
eligible and is shown as black beneath the translucent hint.
Reopen does not destroy the input texture immediately, but invalidates
its presentation before teardown and lazily recreates texture/SRVs at the next
size/subtype change. A color/output change or unavailable resource safely falls
back to the existing opaque status background. Consequently HDR format reopen
is not guaranteed to remain frozen throughout the entire interruption.

The NIS input surface is cleared each iteration, and the flip-discard backbuffer
is undefined after Present; neither is used as a persistent output cache.
Compatible SDR held inputs still follow the existing NIS pipeline, while HDR
uses its existing direct capture path. Translucent HDR status rendering clears
only its offscreen UI surface before drawing, preventing old opaque status
pixels from hiding the retained image.

Startup, never-presented capture, Transition and authoritative No Signal do not
use the hold. The branded/custom No Signal renderer remains unchanged. The
first presentable live recovery immediately selects live Capture, regardless
of cache availability. Detection, eligibility, the 250 ms fallback, metadata,
capture reopen/reconcile/retry, output/tone mapping, pacing, VSync and tearing
policy remain independent of the hold.
