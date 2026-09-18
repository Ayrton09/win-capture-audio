# Changelog

## Unreleased

### Fixed

- **Reference leak in the executable list.** Every entry of the list was
  fetched from OBS and never released, leaking one small settings object per
  entry on each source update and each time the properties dialog refreshed.

### Installer

- The previous version's uninstaller is now looked up in the machine-wide
  registry hives only. Setup runs elevated and executes that path; a per-user
  entry, which any unprivileged process can write, is no longer honoured.

### Build

- CI actions are pinned to exact commits instead of moving tags.
- Removed an unused CI job and its helper script.

## 2.3.3 — 2026-08-25

### New

- **Glitch resistance under load.** The capture threads and the mixer thread
  now register with Windows' multimedia scheduler (MMCSS, "Pro Audio" class),
  the same mechanism OBS uses for its own audio threads. Under CPU contention
  (game + encoder) they keep their deadlines instead of getting starved, which
  is where crackling and popping came from.
- **The status line flags list entries that match nothing.** A typo'd
  executable name used to fail silently forever; the properties dialog now
  shows "No running session matches:" with the offending entries, so a wrong
  name is distinguishable from an app that just is not playing yet.
- **README: guide for apps that play audio from several processes** (Valorant
  and League voice chat, the new Microsoft Teams, launcher/game splits), plus
  the universal recipe for finding the right executable with the
  active-sessions list.

### Installer

- Upgrades no longer inherit the install directory recorded by a 2.2.x-era
  setup (`UsePreviousAppDir=no`): that path was inside the OBS root, where the
  modern layout cannot be loaded from.

## 2.3.2 — 2026-08-24

### Fixed

- **`obs_get_source_properties()` no longer crashes OBS.** libobs may call a
  source's `get_properties` with no instance (scripts and frontends querying
  the source *type*); the callback dereferenced the NULL context
  unconditionally. Instance-backed parts of the dialog are now skipped when
  there is no instance.
- **The "Add executable" button now actually works.** Two independent bugs:
  the added entry was never applied to the running capture (the settings were
  mutated without `obs_source_update`, so the change only showed in the UI
  until any other control was touched), and the value added was the combo's
  *stored* setting — empty or stale when the user never touched the dropdown —
  rather than what the widget displays. The button now applies the update
  immediately and adds what the user actually sees.
- **One broken audio endpoint no longer kills session monitoring.** A single
  failing device during startup enumeration (half-removed devices, misbehaving
  virtual cables) aborted the whole session monitor for the rest of the OBS
  session; sessions never listed and session-mode capture silently never
  started. Failing devices are now skipped, at startup and at hotplug alike.
- **Exclude mode no longer feeds OBS's own audio back into the capture.** The
  session-enumeration fallback treated OBS's own sessions (audio monitoring)
  as capturable, and the source lacked `OBS_SOURCE_DO_NOT_SELF_MONITOR`, so
  monitoring an exclude-mode source looped its audio into itself.
- **Mixer timeline no longer drifts.** Frame↔duration conversions double-floored
  on every tick, accumulating ~12–18 ms/hour of timestamp drift: a one-sample
  silence gap every few seconds and an audible resync after hours of
  continuous capture. The timeline is now derived from a fixed anchor with
  single-floor 128-bit conversions, which also removes a `UINT64` overflow
  after ~4 days of accumulated frames.
- **Tree deduplication now sees whole ancestor chains.** Only the direct
  parent of each audio session was considered, so a session-less intermediate
  process (game → launcher → child) caused doubled audio in include mode and
  leaks in exclude mode. Ancestors are now resolved against a full process
  snapshot, bounded and cycle-guarded.
- **Executable matching is Unicode-aware.** Case folding was byte-wise ASCII
  over UTF-8 strings, so names containing accented or non-Latin letters never
  matched case-insensitively the way Windows filenames do.
- **Wedged WASAPI activations can no longer hang OBS shutdown.** The wait for
  `ActivateAudioInterfaceAsync` was infinite and un-interruptible, and helper
  teardown ran under the global helper-map lock — one stuck audio service call
  could stall every other source and the plugin's unload. The wait is now
  bounded and shutdown-aware, and helpers are destroyed outside the lock.
- **Format-mismatched sources no longer read out of bounds.** Attaching a
  second source with a different audio format to an existing capture helper
  fed it raw frames sized for the other format; the mismatch is now refused
  loudly instead of warned about and allowed.
- **Remaining exception-safety gaps closed.** Non-COM exceptions on the
  capture worker thread now log instead of terminating OBS, and a device
  watcher that fails half-way through construction unregisters its WASAPI
  callback instead of leaving it dangling.

### Installer

- Old zip installs inside the OBS installation directory are now removed on
  install: OBS loads that legacy location first, so a stale DLL there silently
  shadowed the new version.

## 2.3.1 — 2026-08-22

### Fixed

- **Exclude mode no longer depends on the excluded application being alive.**
  When none of the excluded executables is running, the plugin previously fell
  back to enumerating audio sessions one by one, silently dropping system
  sounds and applications without a tracked session until the excluded
  application started playing — at which point the capture switched to the
  native whole-system path and those sources reappeared. Now the native
  `EXCLUDE_TARGET_PROCESS_TREE` capture stays active in that case too,
  targeting OBS's own process tree (which also keeps OBS's audio monitoring
  out of the capture). What you hear no longer changes with the excluded
  application's lifecycle.
- **A non-COM exception in a capture thread could crash all of OBS.** The
  capture thread's retry wrapper only caught `wil::ResultException`; anything
  else (e.g. `std::bad_alloc` while buffering) escaped the thread and
  terminated the process. The net is now as wide as the session monitor's.
- **The status line now names the captured executable in hotkey mode.** The
  captured root process (a browser or game-launcher main process) often has no
  audio session of its own — the sessions belong to child processes — so the
  status fell back to "1 pid(s)". The root's image name is now resolved
  directly ("Capturing: chrome.exe"); the pid count remains only for protected
  processes that cannot be opened.
- **Opening the properties dialog no longer stops an active hotkey capture.**
  Any settings update rebuilt the runtime config with an empty captured-window
  handle, so merely inspecting the source's properties silently deactivated
  the capture the hotkey had started. The captured window now survives
  settings updates; only the deactivate hotkey (or the window going away)
  stops it.
- **Capture retries now back off.** A helper whose target is permanently gone
  (hotkey mode, where no session event cleans it up) retried every 2 seconds
  forever, spamming the log. Retries now double up to 60 seconds — and reset
  as soon as a capture runs, so re-attach after a device change stays prompt.

### Build / CI

- The portable release zip is now versioned
  (`win-capture-audio-<version>.zip`), matching the installer's naming.

## 2.3.0 — 2026-07-28

Modernization release: builds and runs against OBS Studio 32.x on current Windows 10/11.
Verified end-to-end against OBS 32.2.1 on Windows 11 25H2 (source created, sessions listed,
audio captured, clean log). Based on upstream 2.2.3 (bozbez, 2022).

### New features

- **Native exclude capture.** Exclude mode now uses Windows'
  `PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE` when the excluded
  executables resolve to a single process tree: one capture client covers the
  whole system minus that application, instead of enumerating audio sessions and
  spawning a capture thread per application. It also picks up audio the old path
  missed entirely (system sounds, applications that had not opened a stream yet).
  This makes the common streaming setup work: capture an application in one
  source and everything-except-it in another, so it can be kept off a recording
  track without leaking back in through desktop audio.
- **Wildcard matching** in the executable list: entries like `League*.exe` or
  `obs*` now work, and matching is case-insensitive (Windows filenames are, and
  the old exact lookup silently was not).
- **Latency setting**: "Low (single application)" halves the mixer's alignment
  window (~40 ms → ~20 ms of added latency) for the common one-app case;
  "Normal" keeps the safer margin for mixing several applications.
- **Live status line** in the source properties showing exactly which
  executables are being captured.
- **Refresh button** for the active-session list — no more closing and
  reopening the properties dialog to see an application you just started.
- **Better names for protected processes**: sessions whose process cannot be
  opened (elevated games, anti-cheat) now get their executable name parsed out
  of the WASAPI session identifier, falling back to the session display name,
  instead of all showing up as "unknown".

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

### Documentation

- README rewritten: current installation layout, build instructions, comparison with OBS's
  built-in Application Audio Capture, troubleshooting against the actual OBS 32 behavior.
