# win-capture-audio

An OBS plugin similar to OBS's win-capture/game-capture that allows for audio capture from specific applications, rather than the system's audio as a whole.

Compared to OBS's built-in "Application Audio Capture (BETA)" source, this plugin captures by **executable name** (no window required, so it survives the app restarting), supports **multiple applications** in one source, an **exclude mode**, and a **hotkey mode** that captures whatever application is in the foreground.

Internally it uses [ActivateAudioInterfaceAsync](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-activateaudiointerfaceasync) with [AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS](https://learn.microsoft.com/en-us/windows/win32/api/audioclientactivationparams/ns-audioclientactivationparams-audioclient_process_loopback_params).

This is a modernized fork of [bozbez/win-capture-audio](https://github.com/bozbez/win-capture-audio) (unmaintained since 2022), updated to build and run against **OBS Studio 32.x** on current Windows 10/11, with a substantial number of threading, COM-lifetime and audio-timing fixes.

## Requirements

- OBS Studio 28 or later (tested against 32.2.x)
- Windows 10 2004 or later / Windows 11

## Installation

Copy the plugin using the modern per-plugin layout under `%ProgramData%`:

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
git clone --depth 1 https://github.com/microsoft/wil.git deps/wil   # if deps/wil is empty
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

- **Source not showing up:** check the OBS log (Help → Log Files) for a `[win-capture-audio]` line. If absent, the DLL is in the wrong directory — see the layout above.
- **No audio captured:** make sure the target application is actually playing audio and appears in the Windows volume mixer. The capture attaches per audio session; an app that has not opened an audio stream yet has nothing to capture.
- **Crackling or dropouts:** lower the OBS audio buffering (Settings → Advanced → Audio) or report it with a log.

## License

GPLv2 — see [LICENSE](LICENSE).
