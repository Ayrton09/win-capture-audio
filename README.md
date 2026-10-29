# win-capture-audio

[![Release](https://img.shields.io/github/v/release/Ayrton09/win-capture-audio?label=release)](../../releases/latest)
[![Downloads](https://img.shields.io/github/downloads/Ayrton09/win-capture-audio/total?label=downloads)](../../releases)
[![Build](https://github.com/Ayrton09/win-capture-audio/actions/workflows/build.yml/badge.svg)](../../actions/workflows/build.yml)
[![OBS Studio 28–33](https://img.shields.io/badge/OBS%20Studio-28%E2%80%9333-302e31?logo=obsstudio&logoColor=white)](https://obsproject.com/)
[![License: GPL-2.0](https://img.shields.io/badge/license-GPL--2.0-blue)](LICENSE)

**Capture the audio of individual Windows applications in OBS Studio** — by executable name, mixed into one source, with exclude and hotkey modes. No virtual audio cables, no per-window fiddling.

This is a modernized fork of [bozbez/win-capture-audio](https://github.com/bozbez/win-capture-audio) (unmaintained since 2022), updated to build and run against **OBS Studio 28–33** on current Windows 10/11, with a substantial number of threading, COM-lifetime and audio-timing fixes. See the [CHANGELOG](CHANGELOG.md) for everything that changed.

## Features

- 🎯 **Capture by executable name.** Pick applications by their `.exe`, not by a window, so capture keeps working when the application restarts. Names are case-insensitive and accept wildcards (`League*.exe`).
- 🎛️ **Several applications in one source.** Their audio is mixed and timestamp-aligned into a single OBS source.
- 🚫 **Exclude mode.** Capture *everything except* the listed applications — the usual way to keep music off a recording track while it still plays on stream. Works whether or not the excluded application is running.
- ⌨️ **Hotkey mode.** Capture whichever application is in the foreground when you press a hotkey, and release it with another.
- ⏱️ **Latency setting.** Lower the mixer's alignment window when capturing a single application; keep the safer margin when mixing several.
- 📊 **Live status and refresh.** The properties dialog shows what is actually being captured (`Capturing: chrome.exe`), and the active-session list can be refreshed without reopening it.
- 🔁 **Automatic re-attach.** Capture recovers on its own when an application restarts or the audio device changes.
- 🌍 **15 languages.**

### Compared to OBS's built-in "Application Audio Capture (BETA)"

|                              | win-capture-audio                       | OBS built-in                    |
| ---------------------------- | --------------------------------------- | ------------------------------- |
| Select application by        | executable name, wildcards              | window                          |
| Survives application restart | ✅                                      | ❌ stops when the window closes |
| Applications per source      | as many as you like, mixed and aligned  | exactly one                     |
| Exclude mode                 | ✅                                      | ❌                              |
| Hotkey mode                  | ✅                                      | ❌                              |

Internally both use [ActivateAudioInterfaceAsync](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-activateaudiointerfaceasync) with [AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS](https://learn.microsoft.com/en-us/windows/win32/api/audioclientactivationparams/ns-audioclientactivationparams-audioclient_process_loopback_params).

> **Note:** the exclude mode *replaces* OBS's "Desktop Audio" source rather than filtering it — no plugin can remove an application from Desktop Audio, since that source captures the mixed output of the sound device. Add an exclude-mode source and disable Desktop Audio in Settings → Audio.

## Requirements

- OBS Studio **28 or later** (tested against 33.0)
- Windows **10 2004** or later / Windows 11

## Installation

Grab the latest [release](../../releases/latest):

- **Setup installer** (recommended): removes any older version automatically, including 2.2.x installs inside the OBS directory.
- **Zip**: extract into `%ProgramData%\obs-studio\plugins\`. For a **portable** OBS 33 or newer, extract it into the `plugins\` folder next to OBS's `bin\` folder instead (create it if it does not exist).

The resulting layout:

```
C:\ProgramData\obs-studio\plugins\win-capture-audio\
├── win-capture-audio.dll          (OBS 33 and newer)
├── bin\64bit\win-capture-audio.dll (OBS 28–32)
└── data\locale\*.ini
```

OBS 33 changed where it looks for plugins, and OBS 28–32 only know the old place, so the DLL ships in both. Each OBS version loads exactly one copy; OBS 33 notes in its log that it skipped the second one.

Restart OBS and add the **"Application Audio Output Capture"** source.

> The old layout inside the OBS installation directory (`obs-studio\obs-plugins\64bit\` + `obs-studio\data\obs-plugins\win-capture-audio\`) also still works.

## Usage

**Capture specific applications** — leave the mode on *"Capture audio sessions from a selection of executables"* and add entries to the executable list, either by typing them (wildcards allowed) or by picking a running application from the *active sessions* dropdown and clicking *Add*.

**Capture everything except some applications** — same mode, tick *"Capture all audio EXCEPT sessions from the selected executables"*. Remember to disable Desktop Audio (see the note above), or you will hear everything twice.

**Hotkey mode** — switch the mode to *"Capture foreground window with hotkey"*, then assign the two hotkeys in **Settings → Hotkeys** under this source: *"Capture foreground window"* and *"Deactivate capture"*. (Those are the plugin's hotkeys — not the standard *Show*/*Hide* entries listed above them, which only toggle the source's visibility.) Focus the application you want and press the capture hotkey.

### Apps that play audio from more than one process

Some applications route part of their audio through a separate process with a different executable name, so one list entry does not cover everything:

| Application | What's missing | What to add |
| --- | --- | --- |
| Valorant / League of Legends | voice chat plays from Riot's client, not the game | `Riot*.exe` (or use hotkey mode on the game) |
| Microsoft Teams (new) | runs as its own process, not `Teams.exe` | `ms-teams.exe` |
| Games behind launchers | launcher sounds vs. game audio are separate trees | one entry per executable |

The universal trick: while the missing audio is actually playing, open the source's properties and check the **active sessions** dropdown — whatever process is producing that audio appears there under its real name; add it. The status line also lists entries that match no running session, which is usually a typo.

## Building

Requirements: Visual Studio 2022 or later (MSVC C++ workload) and an installed copy of OBS Studio.

```
git submodule update --init
cmake -S . -B build -A x64
cmake --build build --config RelWithDebInfo
```

There is no libobs SDK on Windows — OBS's installer ships no headers or import libraries. The build instead runs [cmake/bootstrap-obs-sdk.ps1](cmake/bootstrap-obs-sdk.ps1) automatically, which:

1. detects your installed OBS Studio and its exact version,
2. downloads the matching `obs-studio` tag's libobs headers, and
3. generates `obs.lib` directly from your installed `obs.dll`'s export table,

so the plugin is always built against exactly the OBS binary it will load into. To install into `%ProgramData%` after building:

```
cmake --install build --config RelWithDebInfo
```

If you have a self-built OBS instead, point `CMAKE_PREFIX_PATH` at it and the usual `find_package(libobs)` path is used.

Release binaries are built by CI against the **OBS 28.0.0** headers on purpose: libobs is forward-compatible, and declaring the oldest supported API lets one binary load on every OBS release from 28 through 33.

## Troubleshooting

- **"Windows protected your PC" when running the installer** — the binaries are not code-signed yet, so SmartScreen has no reputation for them. Choose "More info" → "Run anyway", and verify the download against the `SHA256SUMS.txt` published with each release if you want to be sure it was not tampered with.
- **Source not showing up** — check the OBS log (Help → Log Files) for a `[win-capture-audio]` line. If absent, the DLL is in the wrong directory — see the layout above.
- **No audio captured** — make sure the target application is actually playing audio and appears in the Windows volume mixer. The capture attaches per audio session; an app that has not opened an audio stream yet has nothing to capture.
- **Hotkey does nothing** — make sure you bound the plugin's own hotkeys (*"Capture foreground window"* / *"Deactivate capture"*) for the source, not the generic *Show*/*Hide* ones.
- **Crackling or dropouts** — lower the OBS audio buffering (Settings → Advanced → Audio) or report it with a log.

Found a bug? [Open an issue](../../issues) and attach your OBS log.

## Credits & license

Originally written by [bozbez](https://github.com/bozbez); maintained in this fork by [Ayrton09](https://github.com/Ayrton09).

GPLv2 — see [LICENSE](LICENSE).
