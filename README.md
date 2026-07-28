# win-capture-audio

This is a modernized fork of [bozbez/win-capture-audio](https://github.com/bozbez/win-capture-audio) (unmaintained since 2022), updated to build and run against **OBS Studio 28–32** on current Windows 10/11, with a substantial number of threading, COM-lifetime and audio-timing fixes.

## Features

- **Capture by executable name.** Pick applications by their `.exe`, not by a window, so capture keeps working when the application is restarted. Names are case-insensitive and accept wildcards (`League*.exe`).
- **Several applications in one source.** Their audio is mixed and timestamp-aligned into a single OBS source.
- **Exclude mode.** Capture *everything except* the listed applications — the usual way to keep music off a recording track while it still plays on stream.
- **Hotkey mode.** Capture whichever application is in the foreground when you press a hotkey, and release it with another.
- **Latency setting.** Lower the mixer's alignment window when capturing a single application; keep the safer margin when mixing several.
- **Live status and refresh.** The properties dialog shows what is actually being captured, and the active-session list can be refreshed without reopening it.
- **Automatic re-attach.** Capture recovers on its own when an application restarts or the audio device changes.
- **15 languages.**

### Compared to OBS's built-in "Application Audio Capture (BETA)"

OBS ships its own process-loopback source. It requires selecting a **window** and stops when that window disappears, and each source captures exactly one application. This plugin selects by executable, survives restarts, handles multiple applications and the exclude/hotkey modes above.

Internally both use [ActivateAudioInterfaceAsync](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-activateaudiointerfaceasync) with [AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS](https://learn.microsoft.com/en-us/windows/win32/api/audioclientactivationparams/ns-audioclientactivationparams-audioclient_process_loopback_params).

> **Note:** the exclude mode replaces OBS's "Desktop Audio" source rather than filtering it — no plugin can remove an application from Desktop Audio, since that source captures the mixed output of the sound device. Add an exclude-mode source and disable Desktop Audio in Settings → Audio.

## Requirements

- OBS Studio 28 or later (tested against 32.2.x)
- Windows 10 2004 or later / Windows 11

## Installation

Grab the latest [release](../../releases): run the **setup installer** (it also removes any
older version automatically, including 2.2.x installs inside the OBS directory), or use the
**zip** for portable installs.

Manual install — copy the plugin using the modern per-plugin layout under `%ProgramData%`:

```
C:\ProgramData\obs-studio\plugins\win-capture-audio\
├── bin\64bit\win-capture-audio.dll
└── data\locale\*.ini
```

Restart OBS and add the **"Application Audio Output Capture"** source.

> The old layout inside the OBS installation directory (`obs-studio\obs-plugins\64bit\` + `obs-studio\data\obs-plugins\win-capture-audio\`) also still works.

## Building

Requirements: Visual Studio 2022 or later (MSVC C++ workload), and an installed copy of OBS Studio.

```
git submodule update --init
cmake -S . -B build -A x64
cmake --build build --config RelWithDebInfo
```

There is no libobs SDK on Windows: OBS's installer ships no headers or import libraries. The build instead runs [cmake/bootstrap-obs-sdk.ps1](cmake/bootstrap-obs-sdk.ps1) automatically, which:

1. detects your installed OBS Studio and its exact version,
2. downloads the matching `obs-studio` tag's libobs headers, and
3. generates `obs.lib` directly from your installed `obs.dll`'s export table,

so the plugin is always built against exactly the OBS binary it will load into. To install into `%ProgramData%` after building:

```
cmake --install build --config RelWithDebInfo
```

If you have a self-built OBS instead, point `CMAKE_PREFIX_PATH` at it and the usual `find_package(libobs)` path is used.

## Troubleshooting

- **"Windows protected your PC" when running the installer:** the binaries are not
  code-signed yet, so SmartScreen has no reputation for them. Choose "More info" →
  "Run anyway", and verify the download against the `SHA256SUMS.txt` published with
  each release if you want to be sure it was not tampered with.

- **Source not showing up:** check the OBS log (Help → Log Files) for a `[win-capture-audio]` line. If absent, the DLL is in the wrong directory — see the layout above.
- **No audio captured:** make sure the target application is actually playing audio and appears in the Windows volume mixer. The capture attaches per audio session; an app that has not opened an audio stream yet has nothing to capture.
- **Crackling or dropouts:** lower the OBS audio buffering (Settings → Advanced → Audio) or report it with a log.

## License

GPLv2 — see [LICENSE](LICENSE).
