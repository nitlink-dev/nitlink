# NitLink 1.2.6

[![NitLink with frame generation: available now on Steam](https://raw.githubusercontent.com/nitlink-dev/nitlink/main/docs/images/steam-banner-available.png)](https://store.steampowered.com/app/5346980/)

**NitLink with built-in frame generation is now available on [Steam](https://store.steampowered.com/app/5346980/).** It shows 30 fps console games at 60 fps, with the extra frames generated on the PC GPU. Frame generation adds delay and does not change the console's own frame rate; see the Steam page for GPU requirements. The viewer on GitHub stays free and open source.

This release adds an option to hide the PC cursor over the game and a Turkish settings menu.

## Changes

- **Hide PC cursor during gameplay (#41).** A new F1 toggle, off by default. It hides the PC pointer over live video in the active NitLink window, windowed or fullscreen. The pointer stays visible in the F1 settings, in Picture-in-Picture and over other apps. A cursor that is part of the captured picture is not affected.
- **Turkish settings menu (#48).** Pick Türkçe in F1; a Turkish Windows selects it automatically. Toasts and HUD text remain in English.

## Contributors

- [karsiyakali1912](https://github.com/karsiyakali1912): Turkish.
- [SwankyT1ger](https://github.com/SwankyT1ger): cursor request (#41).

Full contributor and research credits are in ACKNOWLEDGMENTS.md.

## Install

Extract NitLink-1.2.6-win64.zip into a new writable folder and run NitLink.exe. To keep existing settings, copy nitlink.json from the previous NitLink folder into the new one.
