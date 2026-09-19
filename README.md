<p align="center">
  <img src="docs/images/readme-hero.png" alt="NitLink" width="900">
</p>

# NitLink

A capture card viewer for Windows. It makes your console feel like part of your PC: same screen, same workflow, real HDR10, VRR, lowest-latency preview, no extra monitor required.

Built and tested on the Elgato 4K Pro (PCIe), 4K S (USB), 4K X (USB), and Cam Link 4K (USB).

<p align="center">
  <a href="https://github.com/nitlink-dev/nitlink/releases/latest"><img src="https://img.shields.io/github/v/release/nitlink-dev/nitlink?label=release&color=E39A3B" alt="Latest release"></a>
  <a href="https://github.com/nitlink-dev/nitlink/releases"><img src="https://img.shields.io/github/downloads/nitlink-dev/nitlink/total?label=downloads&color=E39A3B" alt="Downloads"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/nitlink-dev/nitlink?color=E39A3B" alt="MIT license"></a>
  <img src="https://img.shields.io/badge/platform-Windows%2010%20%2F%2011-0078D6" alt="Windows 10 / 11">
</p>

<p align="center">
  <img src="docs/images/COMPARSIONS.webp" alt="NitLink HDR comparison" width="900">
</p>

<p align="center">
  <em>HDR comparison preview.</em>
</p>

<p align="center">
  <a href="https://youtu.be/w8Rq5Bonbgg">
    <img src="docs/images/nitlink-thumbnail-1280x720.webp" alt="NitLink 1.1.0 showcase video" width="900">
  </a>
</p>

<p align="center">
  <em>Watch the NitLink 1.1.0 showcase.</em>
</p>

---

## Why

A 42-inch monitor as a main screen. A PS5. No second monitor, no source swaps. The goal: PS5 as just another window on the PC, something to Alt-Tab to, take screenshots from, see in Discord status, all in the same ecosystem.

The existing options didn't work for that. Elgato Studio tonemaps HDR to SDR for the preview. OBS does the same. Every capture utility treats the preview window as "good enough for monitoring while you record", but that's not gameplay. The goal here is to *play*, with the game looking the way it was actually meant to look — and with the lowest latency a capture preview can give you.

NitLink: real HDR10, the lowest preview latency measured, VRR tracking the actual game framerate, integrated with the PC the way every other window is.

---

## Is this for you?

NitLink is for you if:
- You have a single high-end monitor you use as your main display
- You own a console and want it to feel like part of your PC ecosystem
- You like Alt-Tab, integrated screenshots, Discord status: the whole "everything on one screen" workflow
- You care about HDR and don't want it tonemapped to SDR for the preview
- You play games at variable framerates and want VRR to actually work through the capture pipeline
- You want the lowest-latency preview you can get (and you're on a VRR or high-refresh display)

NitLink is NOT for you if:
- You have a free monitor input and just want pure HDMI passthrough (use the card's built-in HDMI-out, it's literally physics-direct)
- You're primarily recording or streaming (use OBS; NitLink runs alongside it just fine)
- Your console already has a dedicated TV and you're happy with that

---

## What it does

- **Real HDR10 passthrough**: auto-detects HDR sources from the Elgato HDR InfoFrame on cards that expose it, negotiates native 10-bit P010 capture (BT.2020 PQ), presents through an R10G10B10A2 + `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` swap chain. Zero color conversion. The card's internal HDR-to-SDR tonemapper is disabled at startup. Color range is handled per the HDR-over-HDMI standard (limited-range luma, full-range chroma) so skin tones and blacks render the way the source intended — no orange shift, no milky blacks. `Alt+R` gives a manual range override (Auto / Full / Limited) for third-party cards or edge cases.
- **Lowest-latency preview, measured.** NitLink presents each captured frame the instant it arrives (low-latency mode, ON by default). On a fixed photon rig, back-to-back against Elgato's own preview software on the same card, NitLink comes out ahead or even on every card tested and never loses. This comes from a low-latency tearing/VRR present — a deliberate tradeoff: lowest latency, but it can tear on a fixed-refresh display, so it's best paired with VRR. `Alt+L` (or the F1 panel) turns low-latency off if you want it, which only adds input lag; the present stays tearing-allowed. See [Performance](#performance).
- **Present pacing (VRR, frame generation, and 30 fps judder).** Capture cards deliver frames at the negotiated HDMI rate regardless of the source's actual framerate, duplicating frames to fill it: a 30 fps game arrives as 60 frames a second. The Present Pacing row in the F1 panel chooses which rate NitLink presents at. **Display refresh** (default) presents every frame the monitor draws, the lowest-latency behavior. **Capture rate** presents once per frame the card delivers. **Source frame rate** uses the GPU frame differ to follow changes in the picture and keeps the measured cadence while the picture is still, so a G-Sync / FreeSync monitor follows the real game framerate, external frame-generation tools such as Lossless Scaling read the real frame rate instead of the panel rate, and 30 fps content stops juddering against a present rate that is not a multiple of it. Both paced modes cost up to one capture interval of latency and turn the present cap off. `present_pacing` in `nitlink.json`.
- **Live source detection in the title bar.** On supported Elgato cards NitLink reads the HDMI source identifier, resolution, fps, and HDR state straight from the card's vendor protocol and shows them in the window title (e.g. `NitLink - PlayStation 5 [HDR]` or `NitLink - 3840x2160 @ 60Hz [HDR]`), updating live as the source changes.
- **True-HDR screenshots.** `Ctrl+S` saves a shareable tonemapped SDR `.png` and, when capturing HDR, a true-HDR `.jxr` (scRGB FP16) sidecar that opens as real HDR in the Windows Photos app.
- **MJP-style Catmull-Rom resampling**: sharp bicubic without ringing artifacts, the same algorithm used by mpv and madVR.
- **NIS upscaling**: NVIDIA Image Scaling integrated as a compute-shader pass for sharpening at non-native window sizes.
- **HDR-aware HUD overlay**: fps, latency, pipeline status. Composited correctly into the HDR backbuffer at 203-nit paper-white so it doesn't blow out against HDR content.
- **WASAPI audio routing** with volume + mute.
- **Borderless fullscreen** and **picture-in-picture**.
- **Discord Rich Presence** showing playing NitLink. Uses Discord's local IPC pipe only; NitLink itself makes no network connections.
- **Multi-device source picker.** Live capture-device list in the F1 settings sidebar. Click a connected device to switch without restarting; the selection persists. Generic devices remain SDR-only unless they have an explicit capture policy, such as the GC553Pro manual HDR/P010 path.
- **Smart signal handling.** Brief HDMI handshake windows (PS5 boot logo, source switch, SDR ↔ HDR transitions) keep showing the last good frame instead of the card's NO SIGNAL placeholder. Real signal loss is detected by format-tagged content fingerprints with a temporal-stability gate.

<p align="center">
  <img src="docs/images/f1-settings.webp" alt="NitLink F1 settings panel" width="900">
</p>

<p align="center">
  <em>F1 settings panel: capture-device picker, HDR, low-latency mode, present pacing, image scaling, audio, hotkeys.</em>
</p>

---

## Performance

**The short version:** NitLink is the lowest-latency capture *viewer* tested. On a fixed photon-to-photon rig, measured back-to-back against Elgato's own preview software on the same card and the same display, NitLink comes out **ahead or even on every card and mode tested, and never loses.**

**Measured results.** Same rig, same session, medians of 100 samples, NitLink versus Elgato's own capture software on the same card and display:

| Card / source mode | NitLink | Elgato | Difference |
|---|---|---|---|
| 4K Pro @ 1080p144 | **38.4 ms** | 44.2 ms | NitLink −5.9 ms |
| 4K X @ 4K120 | **47.7 ms** | 48.4 ms | tie (−0.8 ms, within noise) |
| 4K S @ 4K60 | **67.8 ms** | 75.1 ms | NitLink −7.3 ms |

These results use NitLink's tearing present capped just under the display's refresh (117 Hz on a 120 Hz panel), which is what lets a variable-refresh display absorb the tear. The cap is automatic in 1.1.0: monitor refresh minus 3 Hz, applied when that stays at or above the source frame rate; `present_cap_hz` in `nitlink.json` overrides it. Full methodology, the rig, every caveat, and the raw data: [docs/LATENCY.md](docs/LATENCY.md).

How: NitLink presents each frame the instant it arrives instead of waiting for the next refresh. That's a real latency win and a deliberate **tradeoff** — lowest latency, but the present can tear on a fixed-refresh display. Pair it with VRR and the tear is absorbed. `Alt+L` toggles Low-Latency off, which holds each frame after capture and waits before presenting, so the picture is up to one refresh older; the present stays tearing-allowed either way, so leave it on unless you have a reason not to.

**Why the numbers here are relative, not a single "input lag" figure:** absolute photon latency depends heavily on your display panel and setup, so cross-setup absolutes aren't meaningful. The trustworthy measurement is the *same-rig, back-to-back* delta between two viewers on identical hardware — which is exactly what's reported. NitLink's own present-to-glass slice is well under a millisecond (PresentMon, tearing present).

### What this feels like when actually gaming

The latency above is *capture latency* (HDMI-into-card → photons-off-your-panel). Real gameplay adds the console side: controller polling, game logic, the game's own render/present (roughly 30-60 ms on a 60 fps PS5 title). So total controller-to-screen through NitLink lands in the same ballpark as **playing on a good gaming OLED in Game Mode** — and well ahead of any cloud-gaming option.

- **4K Pro** feels equivalent to a direct HDMI connection for all but frame-perfect competitive play.
- **4K S / 4K X** are great for single-player, RPGs, racing, sports, story, and casual multiplayer; the 4K S is borderline for top-level competitive twitch content.

<p align="center">
  <img src="docs/images/hud-overlay.webp" alt="NitLink HUD overlay on live gameplay" width="900">
</p>

<p align="center">
  <em>HUD overlay (<code>Ctrl+F3</code>) on a live 4K60 PS5 feed: content fps from the GPU frame differ, capture/render latency, and the active capture format.</em>
</p>

### Tested platforms

| Platform | GPU / CPU | Display | Cards |
|---|---|---|---|
| Desktop | RTX 5080, Windows 11 | LG C3 OLED 42" @ 4K 120Hz, G-Sync VRR | 4K Pro, 4K S, 4K X, Cam Link 4K |
| Acer Nitro 5 (AN515-54) | RTX 2060, i7-9750H | TUF VG289Q 4K IPS @60Hz, FreeSync HDR10 | 4K S |

---

## Known limitations

- **Elgato 4K S: 1080p HDR or 4K SDR, not both.** Its USB 3.2 Gen 1 (5 Gbps) interface can't fit 4K@60 P010 (HDR10, ~12 Gbps). The driver only publishes P010 at 1080p/720p. Elgato lists 4K60 SDR capture via MJPEG and native 4K NV12 at up to 30 fps; negotiated viewer output can be converted by Media Foundation. Hardware ceiling, not a NitLink limitation. See [Elgato's format table](https://www.elgato.com/us/en/explorer/products/capture/4k-s-supported-resolutions-and-frame-rates/).
- **Elgato 4K S: HDR costs resolution.** Engaging HDR clamps capture to 1080p, so `hdr_enabled` acts as opt-in even with an HDR source connected. `Alt+H` flips between 1080p HDR and 4K SDR at runtime.
- **AVerMedia GC553Pro HDR is manual.** NitLink does not read a supported HDR InfoFrame interface from this card, so use `Alt+H` to request HDR. P010 modes come exclusively from the card's Media Foundation enumeration; if the requested mode is unavailable, NitLink selects the best native P010 mode and reports it. Manual 1920x1080@60 P010 has been validated on Windows 11; the automatic hardware-selection path is covered by deterministic tests but has not yet been validated on hardware.
- **Windows HDR can be temperamental.** Moving the window across monitors with different HDR profiles, some notification overlays, or apps with custom ICC profiles can cause flickering/desaturation. Closing and reopening NitLink resets the swap chain. A Windows-wide limitation for all HDR apps.
- **VRR below ~40Hz falls back to fixed refresh.** Most VRR displays have a ~40Hz floor; below it VRR disengages.
- **The tearing present can tear on fixed-refresh displays.** That's the tradeoff for the latency win, and it applies whether Low-Latency is on or off. Use a VRR display to absorb it; turning Low-Latency off (`Alt+L`) does not remove tearing, it only adds input lag.
- **Brief visual artifact during PS5 HDR mode changes.** Changing the PS5's HDR setting mid-session renegotiates the HDMI link; a frame or two can show a transient green band before the next reconcile (~100ms). Restarting NitLink avoids it if you know you'll change PS5 HDR mode.
- **Spider-Man 2 on the 4K S in 4K SDR (game-specific).** On the 4K S's NV12 4K@60 SDR path this title produces fewer unique frames than expected (the HUD reports it honestly), while other games on the same setup run at 60fps. Switching the 4K S to 1080p HDR restores 60fps. Most plausibly a PS5 / Insomniac / 4K-S interaction rather than a NitLink defect.
- **No frame generation. No recording or streaming.** Out of scope — run OBS alongside for capture.

<p align="center">
  <img src="docs/images/no-signal.png" alt="NitLink branded no-signal screen" width="900">
</p>

<p align="center">
  <em>Branded no-signal screen, shown after the grace window expires.</em>
</p>

---

## Troubleshooting

- **No audio at all.** Windows has a device-wide Microphone access switch (Settings, Privacy & security, Microphone) that also blocks capture-card audio for desktop apps. NitLink shows a notice when it hits that denial; turn the switch on and restart NitLink.
- **A 120 Hz source captures at 1080p on the 4K Pro.** The card passes 4K120 through to the display but captures 4K only at 60 Hz, so with a 120 Hz source it offers 1080p120. A softer picture at 120 Hz is the card's ceiling, not a NitLink setting. Set the console to 60 Hz for 4K capture, or use a 4K X, which captures 4K120.
- **HDR looks flat or washed out.** NitLink renders real HDR10 only while Windows HDR is on for the display. Turn it on in Windows display settings before pressing Alt+H.
- **Surround sound.** Every Elgato capture device delivers stereo PCM to the PC; Dolby, DTS, and multichannel LPCM are not forwarded by the card, so NitLink can only play two channels. Set the console to Linear PCM, 2.0 channels, so the center channel is not lost in the card's downmix.
- **The picture is squeezed or stretched.** Some cards deliver 4:3 sources inside a 16:9 frame. Press Alt+A to cycle the aspect ratio, or set `aspect_ratio` in nitlink.json.

---

## Download

Get `NitLink-<version>-win64.zip` from the [Releases page](https://github.com/nitlink-dev/nitlink/releases/latest), extract it anywhere, and run `NitLink.exe`. Settings are saved next to the executable.

Official builds are published only on that page. Anything else carrying the NitLink name is a third-party build.

The executable is not code-signed yet, so Windows SmartScreen warns on the first launch. Click **More info**, then **Run anyway**.

---

## Requirements

- Windows 10 (1809+) or Windows 11
- DirectX 11 capable GPU
- Microsoft Edge WebView2 runtime (preinstalled on Windows 11)
- For HDR: an HDR-capable display + HDR enabled in Windows display settings
- For VRR / the tear-free latency win: a VRR-capable display (G-Sync / FreeSync / HDMI 2.1 VRR) with VRR enabled at the OS and display level
- A supported capture card (see [Hardware support](#hardware-support))

### LG OLED notes

On an LG OLED, set the HDMI input icon to "Game Console" (not "PC") for correct HDR Game mode. For VRR, enable G-Sync VRR under `Settings → General → Game Optimizer → VRR & G-Sync`; the Game Dashboard shows "G-SYNC VRR" and the live refresh rate NitLink is driving.

---

## Hardware support

| Device | Status |
|---|---|
| Elgato 4K Pro (PCIe) | ✅ Tested and validated. 4K@60 HDR10 + VRR, HDR auto-detect + source mode via `IKsPropertySet`. |
| Elgato 4K S (USB) | ✅ Tested and validated. 4K@60 SDR (NV12) or 1080p@60 HDR10 (P010). Source name + HDR detect via vendor HID. |
| Elgato 4K X (USB) | ✅ Tested and validated. Source name + resolution + HDR detect via the UVC extension unit; live source-follow. |
| Elgato Cam Link 4K (USB) | ✅ Validated (generic UVC, SDR, no vendor controls). |
| Elgato Game Capture 4K60 Pro MK.2 (PCIe) | ✅ Verified by an owner. HDR auto-detect works and colors match Elgato Studio. |
| AVerMedia Live Gamer ULTRA S GC553Pro (USB) | ✅ Manual 1920x1080@60 P010 HDR validated on Windows 11. Native P010 modes are enumerated through Media Foundation; HDR auto-detection is not implemented. |
| Other Elgato / AVerMedia / Magewell / Razer | ❓ Untested — generic Media Foundation capture should still work. |

NitLink uses the Media Foundation source reader, which works with any DirectShow / WDM capture device. Elgato-specific paths (HDR auto-detect, vendor tonemap control, source detection) silently no-op on cards that don't expose them; generic SDR capture still works.

If you have a different card and want official support, open an issue with: card model, OS version, what you saw, and the `[NitLink/Formats]` lines from the Debug Output window.

---

## Build

CMake. Tested with Visual Studio 2022 / 2026 Insiders.

```
git clone https://github.com/nitlink-dev/nitlink
cd nitlink
```

In Visual Studio: `File → Open → CMake...`, pick `CMakeLists.txt`, select `x64-Release`, `Build → Build All`.

Or CLI (Developer Command Prompt):
```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output at `out/build/x64-Release/NitLink.exe` (VS) or `build/Release/NitLink.exe` (CLI). The build copies `nitlink-menu.html` and `third_party/nis/NIS_Scaler.h` next to the `.exe` (both needed at runtime).

---

## Hotkeys

| Key | Action |
|---|---|
| `F1` | Open / close settings menu |
| `Alt + L` | **Low-Latency** present on/off (default: on; off adds up to one refresh of lag) |
| `Alt + H` | Toggle HDR manually (override auto-detect) |
| `Alt + R` | Cycle source color range: Auto → Full → Limited |
| `Alt + A` | Cycle aspect ratio: Auto → 4:3 → 16:9 → 16:10 → 21:9 → Stretch |
| `F11` or `Alt + Enter` | Toggle fullscreen |
| `Alt + O` | Toggle picture-in-picture |
| `Alt + Up / Down` | Scale PiP up / down, preserving its shape (20 px of width per step, `Shift` for 80 px) |
| `Alt + Left / Right` | Decrease / increase PiP opacity by 5 percentage points (`Shift` for 10 points) |
| `Ctrl + Arrow keys` | Nudge PiP by 20 px (hold `Shift` for 80 px) |
| `Ctrl + Alt + Left / Right` | Decrease / increase PiP width by 20 px (hold `Shift` for 80 px) |
| `Ctrl + Alt + Up / Down` | Decrease / increase PiP height by 20 px (hold `Shift` for 80 px) |
| `Ctrl + S` | Save screenshot (SDR `.png` + true-HDR `.jxr`) to `Pictures/NitLink/` |
| `Ctrl + F3` | Toggle HUD overlay (remembered between launches) |

HUD visibility follows `show_overlay` in `nitlink.json` (default: `false`). Toggling it with `Ctrl + F3` saves the choice immediately.

With PiP active, **drag a corner to scale both dimensions together**, preserving the window's current shape. Drag an edge to adjust just its width or height. The pointer changes to a resize cursor near the edges.

PiP movement, resizing, scaling, and opacity shortcuts can be tapped for one step or held to repeat (after a short delay). Resizing keeps the window within its current monitor's work area, with width limited to 80-16384 px and height to 45-16384 px (or the work area size, if smaller). Set opacity from 10% to 100% with the shortcuts or the **F1 → Video → Picture-in-picture → Opacity** slider. The slider also sets the opacity for the next time PiP opens. Size, position, and opacity are saved to `nitlink.json` on exit.

**Advanced / diagnostic:**

| Key | Action |
|---|---|
| `Ctrl + F4` | HDR color-fidelity test patches (A/B against a reference) |
| `Ctrl + F6` | HDR levels readout (live luma/chroma code range — verify color range per card) |

On the 4K Pro / 4K X, HDR auto-follows the source. On the 4K S, source HDR state is auto-detected but the HDR pipeline is opt-in (it costs resolution); `Alt+H` flips both renderer HDR and capture format at runtime.

---

## Configuration

Settings live in `nitlink.json` next to the executable. Plain text; auto-saves on every toggle. Keys of interest:

- `low_latency`: low-latency present mode (default `true`). `true` = present-on-arrival (lowest input lag). `false` = the frame is held after capture and the swap-chain wait moves before present, so the picture is up to one refresh older. The present is tearing-allowed either way; `false` only adds input lag. Toggle with `Alt+L` or the F1 panel.
- `hdr_enabled`: HDR mode. 4K Pro/X follow the source automatically; on the 4K S it's opt-in (HDR clamps to 1080p over USB).
- `present_pacing`: how often the picture is presented (default `refresh`). `refresh` = every frame the monitor draws, the lowest latency. `captured` = once per frame the card delivers. `unique` = follows changes in the picture and holds the measured cadence on still images, which is what a G-Sync / FreeSync display needs to follow the game, what a frame-generation tool needs to read the real frame rate, and what removes 30 fps judder. Both paced modes add up to one capture interval of latency and turn the present cap off. Cycle from the F1 panel. A `vrr_present_pacing = true` written by an earlier version is migrated to `unique`.

- `present_cap_hz`: present-rate cap in Hz for the low-latency present (default `0` = automatic: monitor refresh minus 3, applied when that is at least the source frame rate). `30` to `1000` = fixed cap, `-1` = off.

- `aspect_ratio`: `auto` (default, the ratio the card reports), `stretch` (fill the window), or a fixed ratio such as `4:3`, `16:9`, `16:10`, `21:9`. Restores 4:3 sources that a card delivers stretched inside a 16:9 frame. Cycle with `Alt+A` or from the F1 panel.
- `panel_side`: `right` (default), `left`, or `full`. Right and left open the F1 panel as a strip beside the picture, which keeps playing underneath. Full covers the window with the wide layout. `panel_width` is the docked width in device-independent pixels (default `420`). Cycle the position from the panel's Video tab.
- `nis_enabled` / `nis_sharpness` / `nis_scale_mode`: NIS upscaler config.
- `color_expansion`: limited→full range expansion (default off; NitLink auto-skips when the source is already full-range).
- `audio_volume` / `audio_muted`: playback level.

---

## Architecture

Pure DirectX 11. Capture frames arrive via Media Foundation on a worker thread into a triple-buffered frame queue, then run through the render pipeline:

```
PS5 HDMI
  ▼
Elgato card (hardware tonemap disabled when supported; source info read via vendor protocol)
  ▼
Media Foundation  (P010 for HDR10, NV12 for SDR; row order + nominal range detected)
  ▼
GPU upload (DYNAMIC texture)
  ▼
GPU frame differ (640x360 SAD) classifies unique vs duplicate  → gates Present in Source frame rate pacing
  ▼
Capture shader  (P010 → BT.2020-PQ passthrough; NV12/BGRA → Catmull-Rom + range-aware decode)
  ▼
Optional NIS upscale (compute shader)
  ▼
HUD / settings overlay composite (203-nit paper-white when HDR)
  ▼
DXGI flip-discard waitable swap chain  (R10G10B10A2 + HDR10 PQ when HDR; BGRA8 when SDR;
  SetMaximumFrameLatency(1), ALLOW_TEARING; present-on-arrival in low-latency mode)
  ▼
Display (VRR active when paired with a VRR panel)
```

The settings menu is HTML/CSS in an embedded WebView2 child window; C++ ↔ JS over `window.chrome.webview.postMessage`.

---

## Project layout

```
src/
├── main.cpp           : WinMain, COM/MF init
├── app/               : Application, config, WebView2 settings bridge, game database
├── audio/             : WASAPI audio routing
├── capture/           : MF device, frame buffer, Elgato HDR/source control, frame differ, DShow backend
├── discord/           : Discord RPC client
├── input/             : Global hotkey manager
├── overlay/           : D2D HUD overlay + branded no-signal screen
├── renderer/          : DX11 swap chain, shaders, window
├── ui/                : Theme tokens
└── upscale/           : NIS upscaler integration

third_party/           : NIS, WebView2, WIL
```

---

## Scope

NitLink does one thing: makes your console feel like part of your PC. Not a recording or streaming tool — run OBS alongside it for that.

---

## License

MIT. See `LICENSE`. Third-party licenses (NIS, WebView2, WIL, MJP's Catmull-Rom) in `LICENSES.md`.

---

## Acknowledgments

- **Matt Pettineo (TheRealMJP)**: Catmull-Rom bicubic reference. MIT licensed.
- **NVIDIA**: NIS SDK.
- **Microsoft**: WIL, WebView2.
- **Brandon (13bm), [elgato4k-linux](https://github.com/13bm/elgato4k-linux)**: Elgato HID/protocol reference. See [`ACKNOWLEDGMENTS.md`](ACKNOWLEDGMENTS.md).
- **Testing & feedback**: u/Lordmau5, u/XSilverlink, and u/TooxChilly for hardware testing, bug reports, and cross-card latency measurements.
- **Hardware**: u/elgato_phil (Elgato) provided the 4K X and the Cam Link 4K used for validation.
- **Traditional Chinese (zh-TW) localization**: [HolyBear（聖小熊）](https://github.com/HolyBearTW) (`@HolyBearTW`) for translation review and real-device validation.

---

## Support development

NitLink is built by one developer. Donations fund hardware testing and distribution. Current goals:

- **Code-signing certificate.** Removes the Windows SmartScreen warning that currently greets every first launch.
- **Broader capture-card validation.** Extend the tested Media Foundation format policies beyond the currently validated Elgato devices and AVerMedia GC553Pro.

Support at [ko-fi.com/klosed89](https://ko-fi.com/klosed89). The core NitLink viewer is free, MIT-licensed, and stays that way. No telemetry, no ads, no bundled junk.

Bug reports and PRs welcome.

---

*NitLink is an independent software application. It is not affiliated with, authorized, sponsored, or endorsed by Corsair Gaming, Inc., Elgato Systems LLC, or their affiliates. All registered trademarks, including "Elgato", "4K Pro", "4K X", "4K S", and "Cam Link", are the property of their respective owners.*
