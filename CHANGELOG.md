# Changelog

## 2.3.0 — 2026-07-27

Modernization release: builds and runs against OBS Studio 32.x on current Windows 10/11.
Verified end-to-end against OBS 32.2.1 on Windows 11 25H2 (source created, sessions listed,
audio captured, clean log). Based on upstream 2.2.3 (bozbez, 2022).

### Build system (rewritten)

- **New CMake build that works without an OBS source build.** OBS's Windows installer ships
  no headers, import libraries or CMake package, so the old `find_package(libobs)` could never
  succeed on a normal machine. The build now runs `cmake/bootstrap-obs-sdk.ps1` automatically:
  it detects the installed OBS, downloads the *matching* obs-studio tag's libobs headers,
  hand-generates the CMake-produced `obsconfig.h`, and creates `obs.lib` from the installed
  `obs.dll`'s export table (`dumpbin`/`lib`), guaranteeing the import library can never drift
  from the binary the plugin loads into. A self-built libobs via `CMAKE_PREFIX_PATH` is still
  supported and takes precedence (`OBS::libobs` imported target either way).
- Linked against the modern `OBS::libobs` target name (the pre-OBS-28 `libobs` name is gone);
  added the previously missing `user32`/`ole32` link libraries that old toolchains pulled in
  implicitly.
- `cmake --install` installs to the modern per-plugin layout
  `%ProgramData%\obs-studio\plugins\win-capture-audio\{bin\64bit,data}` — the layout OBS 31/32
  actually scans (`%APPDATA%\obs-studio\plugins` is **not** searched on Windows). The legacy
  in-tree layout is still staged under `build/release/` for the Inno Setup installer.
- Generated files (`plugin-macros.generated.hpp`, `installer.generated.iss`) now go to the
  build tree instead of polluting the source tree; the git-hash step no longer errors in a
  non-git checkout; removed the cmd.exe-specific `if ==1` POST_BUILD hack that broke Ninja.
- C++20 with `/permissive- /utf-8`, `NOMINMAX`, `WIN32_LEAN_AND_MEAN`, `UNICODE`; builds
  warning-clean on MSVC 14.5x (VS 2026). 32-bit builds removed (OBS dropped them in 28).

### Compatibility with OBS 28–32

- Registered `obs_module_name` / `obs_module_description` / `OBS_MODULE_AUTHOR` so the OBS 32
  Plugin Manager displays the plugin properly.
- `OBS_SOURCE_DO_NOT_DUPLICATE` added to `output_flags` (prevents double audio when the source
  is duplicated into multiple scenes; matches every OBS built-in audio source).
- Switched to `obs_properties_add_button2` (`add_button` is deprecated in 32) and the
  OBS 28+ `OBS_ICON_TYPE_PROCESS_AUDIO_OUTPUT` icon.
- Missing transitive includes (`<optional>`, `<algorithm>`, `<vector>`, `<mmreg.h>`,
  `<climits>`) added — older MSVC STLs provided them by accident.

### Audio correctness

- **Fixed the master clock.** `Mixer::GetCurrentTimestamp` computed
  `QPC_ticks * (10000000 / QPC_frequency)` in integer arithmetic — exact only when QPC runs at
  10 MHz, and **zero** (i.e. *no audio, ever*) on machines where the frequency is higher.
  Replaced with libobs's correctly scaled `os_gettime_ns()`, which is the same clock WASAPI
  stamps capture buffers with.
- **Multi-channel formats fixed.** The WASAPI client was initialized with a bare
  `WAVEFORMATEX` (no channel mask), breaking layouts beyond stereo; it now passes a
  `WAVEFORMATEXTENSIBLE` with an explicit `KSAUDIO_SPEAKER_*` mask and float subformat,
  mirroring OBS's own process-loopback source. Additionally the OBS speaker layout was being
  converted to a channel *count* by casting the enum value, which is wrong for 4.1 and 7.1;
  it now uses `get_audio_channels()`.
- **Audio format is queried live** from `obs_get_audio_info()` (with its return value checked)
  instead of being snapshotted once by a global constructor at DLL load and kept forever.
- **Silent packets are forwarded as explicit silence** instead of being dropped, so the mixer
  timeline and OBS's view of the stream stay continuous through quiet periods.
- **Late packets are salvaged**: the portion of a late mixer input that still overlaps the mix
  window is mixed instead of the whole packet being discarded (with several captured
  processes, the old behavior made all but one of them intermittent).
- Mix buffer growth is now bounded (5 s cap with clean restart) so a stalled tick timer or a
  wild timestamp cannot grow memory without limit.

### Crash / correctness fixes

- `AudioCapture`'s constructor called `Update()` — which posts a thread message — *before*
  the worker thread or its thread id existed; reordered so the worker is up first.
- `SessionMonitor`'s worker could read its own thread id and the device-notification client's
  target tid before the constructor wrote them; both are now published from the worker thread
  itself before any COM registration, and the constructor waits for worker start-up.
- `DeDuplicateCaptureList` erased from a `std::set` while range-iterating it (undefined
  behavior), could spin forever on parent-pid cycles left by pid reuse, and double-captured
  grandchildren of captured roots. Rewritten with an explicit covered-set algorithm that
  terminates and dedups correctly.
- The WASAPI activation `CompletionHandler` was a stack-allocated COM object whose refcount
  the OS bypassed; it is now properly heap-allocated via `Microsoft::WRL::Make`.
- The capture thread died silently on the first failed HRESULT (device change, target exit).
  It now logs, tears down the client and retries every 2 s until shut down, so capture
  self-heals when the device or process comes back.
- One flaky device/session event could kill the session-monitor thread for the rest of the
  OBS session; per-event exceptions are now contained and logged.
- COM is initialized on the threads that actually perform COM work (capture thread and
  session-monitor worker) instead of on the constructing thread.
- Every `GetMessage` loop now handles the `-1` error return (previously treated as a valid
  message with an uninitialized `MSG`) and no longer overwrites the shutdown flag.
- Hotkey mode: a destroyed foreground window produced an indeterminate pid to capture; now
  detected and capture stopped. The hotkey pair is unregistered on source destruction.
- `audio_capture_create` catches all exceptions (previously only `wil::ResultException`,
  by value), so a `std::bad_alloc`/`system_error` during creation can no longer crash OBS.
- `CreateToolhelp32Snapshot` and `CreateTimerQueueTimer` failures are detected and logged;
  `GetProcessImageFileNameW`/`WideCharToMultiByte` failures fall back to `"unknown"` instead
  of producing empty/corrupt session names.
- Defensive null checks around `SessionMonitor::Instance()` for shutdown-ordering safety.

### Resource leaks

- `DeviceWatcher` enumeration leaked one COM reference per discovered session (double
  `AddRef`, single consume).
- `SessionMonitor::RemoveSession` leaked its heap `SessionKey` on both early-return paths
  (one of which is the *common* path); device-id strings leaked if their handler threw.
  All message payloads are now owned by `unique_ptr` on receipt.
- All COM callbacks now release/delete their posted payloads when `PostThreadMessage` fails,
  so a full message queue can no longer leak references.

### Hardening / robustness

- `DeviceWatcher`/`SessionWatcher` are non-copyable (they register their own address with
  WASAPI notification interfaces).
- `get_obs_format` validated: `SubFormat` is only read when the `WAVEFORMATEXTENSIBLE`
  extension block is actually present (`cbSize` check) instead of reading past the struct.
- Exceptions caught by const reference throughout; unused variables and narrowing warnings
  eliminated; builds clean at `/W4` except third-party headers.

### Performance

- `Mixer::Tick` consumes the mix head in place instead of allocating a fresh vector 100
  times per second.
- The silence path reuses a member buffer rather than allocating per packet.

### Signing

- Added a code-signing pipeline (`cmake/sign-plugin.ps1`): SHA-256 + RFC-3161 timestamp,
  taking a certificate from the user store or a PFX. Currently exercised with a self-signed
  development certificate; see `SIGNING.md` for obtaining a CA-issued certificate for
  distribution.

### Documentation

- README rewritten: current installation layout, build instructions, comparison with OBS's
  built-in Application Audio Capture, troubleshooting against the actual OBS 32 behavior.
