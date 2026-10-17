# Acknowledgments

This file credits external work that informed NitLink's development but
is not bundled, statically linked, or otherwise redistributed in the
NitLink binary, and the people who contributed code directly. Bundled
and linked third-party software is tracked separately in `LICENSES.md`.

---

## Protocol research reference

NitLink's Elgato 4K S HID tone-mapping control was implemented with
reference to public protocol research from:

- 13bm/elgato4k-linux
- https://github.com/13bm/elgato4k-linux
- Upstream license: GPL-3.0 (GNU General Public License v3.0)

No source code from elgato4k-linux is included in NitLink. The project
was used only as a protocol reference for the Elgato 4K S HID report
format and tone-mapping control behavior. NitLink's Windows HID
implementation in `src/capture/elgato_hid_4ks.{h,cpp}` is independent
and written from scratch against the Win32 SetupAPI and HID Class API.

For the engineering context (why two card-specific control paths exist,
the byte layout of the HID Output Report, and the implemented HDR/SDR
state machine), see [`docs/4ks-hdr-tonemap.md`](docs/4ks-hdr-tonemap.md).

---

## Contributors

Code contributions merged into NitLink:

- Nathan K. (@n810K), NitLink 1.1.0: audio endpoint recovery and
  capture-format rendering in the WASAPI audio router
  (https://github.com/nitlink-dev/nitlink/pull/5). The router now
  survives default playback device changes, rebuilds a lost endpoint
  on its worker thread, and plays through playback devices whose sample
  rate or channel layout differs from the capture card's.

- HolyBear (聖小熊), [@HolyBearTW](https://github.com/HolyBearTW), NitLink 1.2.2 and 1.2.3:
  HDR-to-SDR tone mapping, Traditional Chinese localization, GC553Pro
  capture and automatic HDR source detection, native fractional frame rates,
  startup and placeholder handling, custom No Signal images, settings
  responsiveness, and GC553Pro hardware validation
  ([#9](https://github.com/nitlink-dev/nitlink/pull/9),
  [#11](https://github.com/nitlink-dev/nitlink/pull/11),
  [#12](https://github.com/nitlink-dev/nitlink/pull/12),
  [#14](https://github.com/nitlink-dev/nitlink/pull/14),
  [#16](https://github.com/nitlink-dev/nitlink/pull/16),
  [#19](https://github.com/nitlink-dev/nitlink/pull/19),
  [#20](https://github.com/nitlink-dev/nitlink/pull/20),
  [#21](https://github.com/nitlink-dev/nitlink/pull/21)); NitLink 1.2.4:
  the wide Full-mode settings layout, the Content FPS headline and
  automatic present pacing on the exact display refresh rate
  ([#26](https://github.com/nitlink-dev/nitlink/pull/26),
  [#31](https://github.com/nitlink-dev/nitlink/pull/31)).

- CN_115, [@CN115](https://github.com/CN115), NitLink 1.2.4: the low-latency
  audio path (configurable capture-to-playback buffering, device-clock
  drift estimation and correction, native playback formats with sinc
  conversion, optional exclusive output with focus fallback, automatic
  recovery), merged from the MIT-licensed nitlink-lowlatency fork with the
  original commits (https://github.com/CN115/nitlink-lowlatency).

---

## Localization contributors

Localization contributions are credited by language and do not imply
project authorship or maintainership.

- **Traditional Chinese (zh-TW):** [HolyBear（聖小熊）](https://github.com/HolyBearTW)
  (`@HolyBearTW`) — translation and localization review with real-device
  validation.
- **Simplified Chinese (zh-CN):** [CN_115](https://github.com/CN115) (`@CN115`) — translation
  and automatic system-language selection.
- **German (de-DE):** [Noka123](https://github.com/Noka123) (`@Noka123`) — translation
  and automatic system-language selection.
- **Korean (ko-KR):** [iorph](https://github.com/i0rph) (`@i0rph`) — translation,
  automatic system-language selection, and localization review with real-device
  validation.
