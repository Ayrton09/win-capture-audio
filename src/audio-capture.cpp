#include <cstdint>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <format>
#include <set>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <stringapiset.h>
#include <processthreadsapi.h>
#include <mmreg.h>
#include <audiopolicy.h>
#include <audioclientactivationparams.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <obs.h>
#include <obs-module.h>
#include <obs-data.h>
#include <obs-properties.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <winuser.h>

#include "wil/result.h"
#include "wil/result_macros.h"

#include "audio-capture.hpp"
#include "audio-capture-helper-manager.hpp"

AudioCaptureHelperManager helper_manager;

// Case folding must be Unicode-aware: executable names are UTF-8 and Windows
// filenames case-fold beyond ASCII, so a byte-wise tolower() would fail to
// match names containing accented or non-Latin letters.
static std::wstring Utf8ToLowerWide(const char *utf8)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
	if (n <= 1)
		return {};

	std::wstring wide(static_cast<std::size_t>(n) - 1, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), n);
	CharLowerBuffW(wide.data(), static_cast<DWORD>(wide.size()));
	return wide;
}

// Wildcard match over pre-folded wide strings: '*' spans any run, '?' any
// single character.
static bool WildcardMatch(const wchar_t *pattern, const wchar_t *str)
{
	const wchar_t *star = nullptr;
	const wchar_t *star_str = nullptr;

	while (*str) {
		if (*pattern == *str || *pattern == L'?') {
			++pattern;
			++str;
		} else if (*pattern == L'*') {
			star = pattern++;
			star_str = str;
		} else if (star) {
			pattern = star + 1;
			str = ++star_str;
		} else {
			return false;
		}
	}

	while (*pattern == L'*')
		++pattern;

	return *pattern == L'\0';
}

static bool MatchesAnyExecutable(const std::set<std::string> &patterns, const std::string &executable)
{
	auto folded = Utf8ToLowerWide(executable.c_str());

	for (const auto &pattern : patterns) {
		if (WildcardMatch(Utf8ToLowerWide(pattern.c_str()).c_str(), folded.c_str()))
			return true;
	}

	return false;
}

// Full parent map of every process in the system. The map must cover
// non-session processes too: a session pid's capture-relevant ancestor can sit
// behind any number of session-less intermediates (game -> launcher -> child),
// and a parents-of-sessions-only map cannot see across them.
static std::unordered_map<DWORD, DWORD> GetProcessParents()
{
	std::unordered_map<DWORD, DWORD> parent_map;

	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		warn("CreateToolhelp32Snapshot failed (%lu)", GetLastError());
		return parent_map;
	}

	wil::unique_handle handle{snapshot};

	PROCESSENTRY32W info;
	info.dwSize = sizeof(PROCESSENTRY32W);

	for (bool ret = Process32FirstW(handle.get(), &info); ret;
	     ret = Process32NextW(handle.get(), &info))
		parent_map[info.th32ProcessID] = info.th32ParentProcessID;

	return parent_map;
}

// Calls visit(ancestor) for each proper ancestor of pid, nearest first, until
// visit returns true (found) or the chain ends. Bounded and cycle-guarded:
// a stale snapshot can contain parent cycles from pid reuse.
static bool AnyAncestor(const std::unordered_map<DWORD, DWORD> &parents, DWORD pid,
			const std::function<bool(DWORD)> &visit)
{
	std::set<DWORD> seen;
	DWORD current = pid;

	for (int depth = 0; depth < 64; ++depth) {
		auto it = parents.find(current);
		if (it == parents.end())
			return false;

		DWORD parent = it->second;
		if (parent == 0 || parent == current || !seen.insert(parent).second)
			return false;

		if (visit(parent))
			return true;

		current = parent;
	}

	return false;
}

std::set<DWORD>
AudioCapture::DeDuplicateCaptureList(const std::set<DWORD> &pids,
				     const std::set<DWORD> &exclude_pids)
{
	auto parents = GetProcessParents();

	// WASAPI process loopback captures the whole process tree, so any
	// candidate that is an ancestor of an excluded pid - at any depth, not
	// just the direct parent - would pull the excluded audio back in.
	std::set<DWORD> candidates = pids;
	for (auto excluded : exclude_pids) {
		AnyAncestor(parents, excluded, [&](DWORD ancestor) {
			candidates.erase(ancestor);
			return false;
		});
	}

	// A candidate whose ancestor chain reaches another candidate is covered
	// by that ancestor's capture; only chain-tops become capture roots.
	std::set<DWORD> roots;
	for (auto pid : candidates) {
		bool covered = AnyAncestor(parents, pid, [&](DWORD ancestor) {
			return candidates.contains(ancestor);
		});

		if (!covered)
			roots.insert(pid);
	}

	// Snapshot-stale parent cycles could mark every candidate as covered by
	// another; capture everything rather than silently capturing nothing.
	if (roots.empty() && !candidates.empty())
		return candidates;

	return roots;
}

void AudioCapture::StartCapture(const std::set<DWORD> &new_pids, bool exclude)
{
	// Switching between include and exclude means every existing helper is the
	// wrong kind; drop them all before registering the new ones.
	if (exclude != capture_exclude)
		StopCapture();

	for (auto pid : pids) {
		if (new_pids.contains(pid))
			continue;

		helper_manager.UnRegisterMixer(pid, capture_exclude, &mixer.value());
	}

	for (auto new_pid : new_pids) {
		if (pids.contains(new_pid))
			continue;

		helper_manager.RegisterMixer(new_pid, exclude, &mixer.value());
	}

	auto lock = pids_section.lock();
	pids = new_pids;
	capture_exclude = exclude;
}

void AudioCapture::StopCapture()
{
	for (auto pid : pids)
		helper_manager.UnRegisterMixer(pid, capture_exclude, &mixer.value());

	auto lock = pids_section.lock();
	pids.clear();
	capture_exclude = false;
}

std::set<DWORD> AudioCapture::GetCapturedPids()
{
	auto lock = pids_section.lock();
	return pids;
}

bool AudioCapture::IsExcludeCapture()
{
	auto lock = pids_section.lock();
	return capture_exclude;
}

void AudioCapture::WorkerUpdate()
{
	auto config_lock = config_section.lock();
	auto config = this->config;
	config_lock.reset();

	if (config.mode == MODE_HOTKEY) {
		if (config.hotkey_window == NULL) {
			StopCapture();
			return;
		}

		DWORD pid = 0;
		GetWindowThreadProcessId(config.hotkey_window, &pid);
		if (pid == 0) {
			// The window was destroyed since the hotkey was pressed.
			StopCapture();
			return;
		}

		StartCapture({pid}, false);
		return;
	}

	auto *monitor = SessionMonitor::Instance();
	if (!monitor) {
		StopCapture();
		return;
	}

	auto sessions = monitor->GetSessions();

	std::set<DWORD> capture_pids;
	std::set<DWORD> exclude_pids;

	for (auto &[key, executable] : sessions) {
		// In exclude mode, never treat OBS's own sessions (audio monitoring)
		// as capturable: the enumeration fallback would otherwise feed OBS's
		// output back into the capture. The native single-tree path already
		// gets this right when it excludes our own process.
		if (config.exclude && key.pid == GetCurrentProcessId())
			continue;

		if ((!MatchesAnyExecutable(config.executables, executable)) ^ config.exclude) {
			exclude_pids.insert(key.pid);
			continue;
		}

		capture_pids.insert(key.pid);
	}

	if (config.exclude) {
		if (exclude_pids.size() > 0) {
			// Windows can capture "everything except this process tree"
			// natively, with a single client. That is both cheaper than one
			// helper per application and more complete: it also covers
			// system sounds and anything that never shows up as a tracked
			// audio session.
			//
			// The API takes a single target, so this only applies when the
			// excluded executables resolve to one process tree; otherwise
			// fall back to enumerating everything that should be captured.
			auto excluded_roots = AudioCapture::DeDuplicateCaptureList(exclude_pids);

			if (excluded_roots.size() == 1) {
				StartCapture(excluded_roots, true);
				return;
			}
		} else {
			// None of the excluded executables is running. Enumerating
			// sessions here (like the multi-tree case below) would make the
			// capture depend on whether the excluded application is alive:
			// system sounds and sessionless applications would drop out
			// until it starts playing. Keep the native whole-system capture
			// by excluding our own process tree instead - it always exists,
			// can never match a target, and keeps OBS's own audio
			// monitoring out of the capture.
			StartCapture({GetCurrentProcessId()}, true);
			return;
		}
	}

	if (capture_pids.size() == 0) {
		StopCapture();
		return;
	}

	StartCapture(AudioCapture::DeDuplicateCaptureList(
			     capture_pids, config.exclude ? exclude_pids : std::set<DWORD>()),
		     false);
}

bool AudioCapture::Tick(const MSG &msg)
{
	bool shutdown = false;

	switch (msg.message) {
	case CaptureEvents::Shutdown:
		debug("shutting down");
		shutdown = true;

		break;

	case CaptureEvents::Update:
	case CaptureEvents::SessionAdded:
	case CaptureEvents::SessionExpired:
		WorkerUpdate();
		break;

	default:
		warn("unexpected event id, ignoring");
		break;
	}

	return shutdown;
}

void AudioCapture::Run()
{
	// Force message queue creation
	MSG msg;
	PeekMessageA(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);

	worker_ready.SetEvent();

	// Before current thread is running, any message won't be sent successfully.
	// So here send Update again to make sure Tick() can be called once.
	PostThreadMessageA(GetCurrentThreadId(), CaptureEvents::Update, NULL, NULL);

	bool shutdown = false;
	while (!shutdown) {
		auto ret = GetMessage(&msg, reinterpret_cast<HWND>(-1), 0, 0);
		if (ret == 0 || ret == -1) {
			debug("shutting down");
			break;
		}

		// An escaping exception would std::terminate all of OBS; one failed
		// update must not kill the worker for the rest of the session.
		try {
			shutdown = Tick(msg);
		} catch (const wil::ResultException &e) {
			error("failed to process event %u: %s", msg.message, e.what());
		} catch (const std::exception &e) {
			error("failed to process event %u: %s", msg.message, e.what());
		}
	}

	StopCapture();
}

void AudioCapture::Update(obs_data_t *settings)
{
	AudioCaptureConfig new_config = {
		.mode = (mode)obs_data_get_int(settings, SETTING_MODE),
		.exclude = obs_data_get_bool(settings, SETTING_EXCLUDE),
	};

	if (new_config.mode == MODE_SESSION)
		new_config.executables = GetExecutables(settings);

	if (mixer)
		mixer->SetLowLatency(obs_data_get_int(settings, SETTING_LATENCY) == 1);

	auto lock = config_section.lock();
	// The captured window is runtime state set by the hotkey, not a setting:
	// carry it across settings updates, or merely opening the properties
	// dialog silently stops an active hotkey capture.
	new_config.hotkey_window = config.hotkey_window;
	config = std::move(new_config);
	lock.reset();

	PostThreadMessageA(worker_tid, CaptureEvents::Update, NULL, NULL);
}

static void audio_capture_update(void *data, obs_data_t *settings)
{
	auto *ctx = static_cast<AudioCapture *>(data);
	ctx->Update(settings);
}

bool AudioCapture::IsUwpWindow(HWND window)
{
	wchar_t name[256] = {L'\0'};

	if (!GetClassNameW(window, name, sizeof(name) / sizeof(wchar_t)))
		return false;

	return wcscmp(name, L"ApplicationFrameWindow") == 0;
}

HWND AudioCapture::GetUwpActualWindow(HWND parent_window)
{
	DWORD parent_pid;
	HWND child_window;

	GetWindowThreadProcessId(parent_window, &parent_pid);
	child_window = FindWindowEx(parent_window, NULL, NULL, NULL);

	while (child_window != NULL) {
		DWORD child_pid;
		GetWindowThreadProcessId(child_window, &child_pid);

		if (child_pid != parent_pid)
			return child_window;

		child_window = FindWindowEx(parent_window, child_window, NULL, NULL);
	}

	return NULL;
}

void AudioCapture::HotkeyStart()
{
	auto lock = config_section.lock();
	auto config_copy = this->config;
	lock.reset();

	if (config_copy.mode != MODE_HOTKEY)
		return;

	auto window = GetForegroundWindow();
	if (AudioCapture::IsUwpWindow(window))
		window = AudioCapture::GetUwpActualWindow(window);

	lock = config_section.lock();
	config.hotkey_window = window;
	lock.reset();

	PostThreadMessageA(worker_tid, CaptureEvents::Update, NULL, NULL);
}

void AudioCapture::HotkeyStop()
{
	auto lock = config_section.lock();
	if (config.mode != MODE_HOTKEY)
		return;

	config.hotkey_window = NULL;
	lock.reset();

	PostThreadMessageA(worker_tid, CaptureEvents::Update, NULL, NULL);
}

static bool hotkey_start(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	if (!pressed)
		return false;

	auto *ctx = static_cast<AudioCapture *>(data);
	ctx->HotkeyStart();

	return true;
}

static bool hotkey_stop(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	if (!pressed)
		return false;

	auto *ctx = static_cast<AudioCapture *>(data);
	ctx->HotkeyStop();

	return true;
}

AudioCapture::AudioCapture(obs_data_t *settings, obs_source_t *source) : source{source}
{
	mixer.emplace(source, helper_manager.GetFormat());

	// The worker must exist before Update() posts to it; the old order posted
	// CaptureEvents::Update to an uninitialized thread id.
	worker_thread = std::thread(&AudioCapture::Run, this);
	worker_tid = GetThreadId(worker_thread.native_handle());
	worker_ready.wait();

	if (auto *monitor = SessionMonitor::Instance())
		monitor->RegisterEvent(worker_tid, CaptureEvents::SessionAdded,
				       CaptureEvents::SessionExpired);

	Update(settings);

	hotkey_pair = obs_hotkey_pair_register_source(source, HOTKEY_START, TEXT_HOTKEY_START,
						      HOTKEY_STOP, TEXT_HOTKEY_STOP, hotkey_start,
						      hotkey_stop, this, this);
}

static void *audio_capture_create(obs_data_t *settings, obs_source_t *source)
{
	try {
		return new AudioCapture(settings, source);
	} catch (const wil::ResultException &e) {
		error("failed to create context: %s", e.what());
	} catch (const std::exception &e) {
		error("failed to create context: %s", e.what());
	}

	return nullptr;
}

AudioCapture::~AudioCapture()
{
	obs_hotkey_pair_unregister(hotkey_pair);

	if (auto *monitor = SessionMonitor::Instance())
		monitor->UnRegisterEvent(worker_tid);

	if (!worker_thread.joinable())
		return;

	worker_ready.wait();
	PostThreadMessageA(worker_tid, CaptureEvents::Shutdown, NULL, NULL);
	worker_thread.join();
}

static void audio_capture_destroy(void *data)
{
	auto *ctx = static_cast<AudioCapture *>(data);
	delete ctx;
}

static bool mode_callback(obs_properties_t *ps, obs_property_t *p, obs_data_t *settings)
{
	auto mode = obs_data_get_int(settings, SETTING_MODE);

	p = obs_properties_get(ps, SETTING_EXECUTABLE_LIST);
	obs_property_set_visible(p, mode == MODE_SESSION);

	p = obs_properties_get(ps, SETTING_ACTIVE_SESSION_GROUP);
	obs_property_set_visible(p, mode == MODE_SESSION);

	p = obs_properties_get(ps, SETTING_EXCLUDE);
	obs_property_set_visible(p, mode == MODE_SESSION);

	return true;
}

std::tuple<std::string, std::string>
AudioCapture::MakeSessionOptionStrings(std::set<DWORD> pids, const std::string &executable,
				       bool added = false)
{
	auto pids_string = std::string();

	if (pids.size() > 0) {
		auto it = std::begin(pids);
		pids_string.append(std::format("{}", *it));

		++it;
		for (auto end = std::end(pids); it != end; ++it)
			pids_string.append(std::format(", {}", *it));
	} else {
		pids_string.append("*");
	}

	if (!added)
		return {std::format("[{}] {}", pids_string, executable), executable};

	static int id = 0;
	return {std::format("[{}] {} (added)", pids_string, executable), std::format("{}", id++)};
}

static bool executable_list_callback(void *data, obs_properties_t *ps, obs_property_t *p,
				     obs_data_t *settings)
{
	auto *ctx = static_cast<AudioCapture *>(data);
	if (!ctx)
		return true;

	auto *active_session_list = obs_properties_get(ps, SETTING_ACTIVE_SESSION_LIST);
	auto *active_session_add = obs_properties_get(ps, SETTING_ACTIVE_SESSION_ADD);

	obs_property_list_clear(active_session_list);
	ctx->FillActiveSessionList(active_session_list, active_session_add);
	ctx->UpdateStatus(ps);

	return true;
}

static bool session_refresh_callback(obs_properties_t *ps, obs_property_t *p, void *data)
{
	auto *ctx = static_cast<AudioCapture *>(data);
	if (!ctx)
		return true;

	auto *active_session_list = obs_properties_get(ps, SETTING_ACTIVE_SESSION_LIST);
	auto *active_session_add = obs_properties_get(ps, SETTING_ACTIVE_SESSION_ADD);

	obs_property_list_clear(active_session_list);
	ctx->FillActiveSessionList(active_session_list, active_session_add);
	ctx->UpdateStatus(ps);

	return true;
}

static bool session_add_callback(obs_properties_t *ps, obs_property_t *p, void *data)
{
	auto *ctx = static_cast<AudioCapture *>(data);
	if (!ctx)
		return false;

	auto *source = ctx->GetSource();
	auto *settings = obs_source_get_settings(source);

	// The combo's stored setting lags the widget: OBS only writes it when the
	// selection is actively changed, so a fresh dialog shows the first entry
	// while the setting is still empty (or holds a stale, no-longer-offered
	// value). Add what the user actually sees: the stored value when it is a
	// currently-addable executable, the first addable one otherwise.
	auto addable = ctx->GetAddableExecutables(settings);
	std::string executable = obs_data_get_string(settings, SETTING_ACTIVE_SESSION_LIST);

	if (std::find(addable.begin(), addable.end(), executable) == addable.end())
		executable = addable.empty() ? std::string() : addable.front();

	if (!executable.empty()) {
		auto *executable_list_array = obs_data_get_array(settings, SETTING_EXECUTABLE_LIST);

		if (obs_data_array_count(executable_list_array) == 0) {
			obs_data_array_release(executable_list_array);

			executable_list_array = obs_data_array_create();
			obs_data_set_array(settings, SETTING_EXECUTABLE_LIST, executable_list_array);
		}

		auto *executable_obj = obs_data_create();

		obs_data_set_bool(executable_obj, "hidden", false);
		obs_data_set_bool(executable_obj, "selected", false);
		obs_data_set_string(executable_obj, "value", executable.c_str());

		obs_data_array_push_back(executable_list_array, executable_obj);

		obs_data_release(executable_obj);
		obs_data_array_release(executable_list_array);

		// Mutating the settings object only feeds the UI; the live capture
		// reads a cached config that only the source's update callback
		// refreshes, so apply the change explicitly.
		obs_source_update(source, nullptr);
	}

	auto *active_session_list = obs_properties_get(ps, SETTING_ACTIVE_SESSION_LIST);
	auto *active_session_add = obs_properties_get(ps, SETTING_ACTIVE_SESSION_ADD);

	obs_property_list_clear(active_session_list);
	ctx->FillActiveSessionList(active_session_list, active_session_add);
	ctx->UpdateStatus(ps);

	obs_data_release(settings);

	return true;
}

std::set<std::string> AudioCapture::GetExecutables(obs_data_t *settings)
{
	auto *executable_list_array = obs_data_get_array(settings, SETTING_EXECUTABLE_LIST);
	auto count = obs_data_array_count(executable_list_array);

	std::set<std::string> executables;

	for (std::size_t i = 0; i < count; ++i) {
		auto *item = obs_data_array_item(executable_list_array, i);
		auto *executable = obs_data_get_string(item, "value");

		executables.insert(std::string(executable));
		// obs_data_array_item hands out a reference.
		obs_data_release(item);
	}

	obs_data_array_release(executable_list_array);
	return executables;
}

// Executables the "Add" combo currently offers as enabled options, in the
// same order FillActiveSessionList lists them.
std::vector<std::string> AudioCapture::GetAddableExecutables(obs_data_t *settings)
{
	auto *monitor = SessionMonitor::Instance();
	auto sessions = monitor ? monitor->GetSessions()
				: std::unordered_map<SessionKey, std::string>{};
	auto patterns = GetExecutables(settings);

	std::set<std::string> unique;
	for (auto &[key, executable] : sessions) {
		if (!MatchesAnyExecutable(patterns, executable))
			unique.insert(executable);
	}

	std::vector<std::string> addable(unique.begin(), unique.end());
	std::sort(addable.begin(), addable.end(), [](const std::string &a, const std::string &b) {
		return astrcmpi(a.c_str(), b.c_str()) < 0;
	});

	return addable;
}

void AudioCapture::FillActiveSessionList(obs_property_t *session_list, obs_property_t *session_add)
{
	auto *settings = obs_source_get_settings(GetSource());

	auto *monitor = SessionMonitor::Instance();
	auto sessions = monitor ? monitor->GetSessions()
				: std::unordered_map<SessionKey, std::string>{};
	auto executables = GetExecutables(settings);

	std::unordered_map<std::string, std::set<DWORD>> session_options;
	for (auto &[key, executable] : sessions)
		session_options[executable].insert(key.pid);

	std::vector<std::tuple<std::string, std::set<DWORD>>> enabled_session_options;
	std::vector<std::tuple<std::string, std::set<DWORD>>> disabled_session_options;

	for (auto &[executable, pids] : session_options) {
		if (MatchesAnyExecutable(executables, executable)) {
			disabled_session_options.push_back({executable, pids});
			continue;
		}

		enabled_session_options.push_back({executable, pids});
	}

	auto cmp = [](auto a, auto b) {
		return astrcmpi(std::get<0>(a).c_str(), std::get<0>(b).c_str()) < 0;
	};

	std::sort(enabled_session_options.begin(), enabled_session_options.end(), cmp);
	std::sort(disabled_session_options.begin(), disabled_session_options.end(), cmp);

	for (auto &[executable, pids] : enabled_session_options) {
		auto [name, val] = AudioCapture::MakeSessionOptionStrings(pids, executable);
		obs_property_list_add_string(session_list, name.c_str(), val.c_str());
	}

	for (auto &[executable, pids] : disabled_session_options) {
		auto [name, val] = AudioCapture::MakeSessionOptionStrings(pids, executable, true);
		auto idx = obs_property_list_add_string(session_list, name.c_str(), val.c_str());
		obs_property_list_item_disable(session_list, idx, true);
	}

	obs_property_set_enabled(session_add, enabled_session_options.size() != 0);
	obs_property_set_enabled(session_list, enabled_session_options.size() != 0);

	obs_data_release(settings);
}

void AudioCapture::UpdateStatus(obs_properties_t *ps)
{
	auto *status = obs_properties_get(ps, SETTING_STATUS);
	if (!status)
		return;

	auto *monitor = SessionMonitor::Instance();
	auto sessions = monitor ? monitor->GetSessions()
				: std::unordered_map<SessionKey, std::string>{};

	auto captured = GetCapturedPids();
	if (captured.empty()) {
		std::string text = TEXT_STATUS_NONE;
		AppendUnmatchedPatterns(text, sessions);
		obs_property_set_description(status, text.c_str());
		return;
	}

	std::set<std::string> names;
	for (auto &[key, executable] : sessions) {
		if (captured.contains(key.pid))
			names.insert(executable);
	}

	if (names.empty()) {
		// No audio session carries the captured pid. Typical in hotkey
		// mode: the captured root is a browser/game launcher main process
		// whose audio sessions belong to child processes (the tree is
		// still captured). Resolve the root's own image name so the
		// status shows an executable instead of a bare pid count.
		for (auto pid : captured) {
			wil::unique_process_handle process{
				OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid)};
			if (!process)
				continue;

			wchar_t path[MAX_PATH] = {L'\0'};
			if (!GetProcessImageFileNameW(process.get(), path, MAX_PATH))
				continue;

			char utf8[MAX_PATH * 4] = {'\0'};
			os_wcs_to_utf8(path, 0, utf8, sizeof(utf8));

			std::string name{utf8};
			auto slash = name.find_last_of('\\');
			if (slash != std::string::npos)
				name = name.substr(slash + 1);

			if (!name.empty())
				names.insert(name);
		}
	}

	std::string text = IsExcludeCapture() ? TEXT_STATUS_EXCLUDING : TEXT_STATUS_CAPTURING;
	if (names.empty()) {
		// Protected process (OpenProcess denied): fall back to the count.
		text += std::format(" {} pid(s)", captured.size());
	} else {
		bool first = true;
		for (auto &name : names) {
			text += first ? " " : ", ";
			text += name;
			first = false;
		}
	}

	AppendUnmatchedPatterns(text, sessions);
	obs_property_set_description(status, text.c_str());
}

// A typo'd executable name fails silently forever - the list accepts it and
// nothing ever captures. Surface list entries that match no running session so
// the user can tell a wrong name from an app that just is not playing yet.
void AudioCapture::AppendUnmatchedPatterns(std::string &text,
					   const std::unordered_map<SessionKey, std::string> &sessions)
{
	auto *settings = obs_source_get_settings(GetSource());

	if (obs_data_get_int(settings, SETTING_MODE) != MODE_SESSION) {
		obs_data_release(settings);
		return;
	}

	auto patterns = GetExecutables(settings);
	obs_data_release(settings);

	std::string unmatched;
	for (const auto &pattern : patterns) {
		auto folded_pattern = Utf8ToLowerWide(pattern.c_str());

		bool hit = false;
		for (auto &[key, executable] : sessions) {
			if (WildcardMatch(folded_pattern.c_str(),
					  Utf8ToLowerWide(executable.c_str()).c_str())) {
				hit = true;
				break;
			}
		}

		if (!hit) {
			unmatched += unmatched.empty() ? "" : ", ";
			unmatched += pattern;
		}
	}

	if (!unmatched.empty())
		text += std::format("\n{} {}", TEXT_STATUS_NO_MATCH, unmatched);
}

static obs_properties_t *audio_capture_properties(void *data)
{
	auto *ctx = static_cast<AudioCapture *>(data);

	obs_properties_t *ps = obs_properties_create();

	// Live status line ("Capturing: chrome.exe, spotify.exe")
	obs_properties_add_text(ps, SETTING_STATUS, TEXT_STATUS_NONE, OBS_TEXT_INFO);

	// Mode setting (specific session or hotkey)
	auto *mode = obs_properties_add_list(ps, SETTING_MODE, TEXT_MODE, OBS_COMBO_TYPE_LIST,
					     OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(mode, TEXT_MODE_SESSION, MODE_SESSION);
	obs_property_list_add_int(mode, TEXT_MODE_HOTKEY, MODE_HOTKEY);

	obs_property_set_modified_callback(mode, mode_callback);

	// Executable list setting
	auto *executable_list =
		obs_properties_add_editable_list(ps, SETTING_EXECUTABLE_LIST, TEXT_EXECUTABLE_LIST,
						 OBS_EDITABLE_LIST_TYPE_STRINGS, NULL, NULL);

	obs_property_set_modified_callback2(executable_list, executable_list_callback, ctx);

	// Exclude setting
	obs_properties_add_bool(ps, SETTING_EXCLUDE, TEXT_EXCLUDE);

	// Latency setting
	auto *latency = obs_properties_add_list(ps, SETTING_LATENCY, TEXT_LATENCY,
						OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(latency, TEXT_LATENCY_NORMAL, 0);
	obs_property_list_add_int(latency, TEXT_LATENCY_LOW, 1);

	// Active session group
	obs_properties_t *active_session_group = obs_properties_create();

	// Active session list
	auto *active_session_list = obs_properties_add_list(
		active_session_group, SETTING_ACTIVE_SESSION_LIST, TEXT_ACTIVE_SESSION_LIST,
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	// Add session button
	//
	// obs_properties_add_button is deprecated as of OBS 32: it leaves the callback's
	// void* to libobs, which passes the source's data pointer. add_button2 makes that
	// explicit instead of relying on the implicit behaviour.
	auto *active_session_add =
		obs_properties_add_button2(active_session_group, SETTING_ACTIVE_SESSION_ADD,
					   TEXT_ACTIVE_SESSION_ADD, session_add_callback, ctx);

	// Refresh button: sessions that appear while the dialog is open used to
	// require closing and reopening it.
	obs_properties_add_button2(active_session_group, SETTING_ACTIVE_SESSION_REFRESH,
				   TEXT_ACTIVE_SESSION_REFRESH, session_refresh_callback, ctx);

	// libobs may call get_properties with no instance (data == NULL), e.g.
	// obs_get_source_properties() from scripts or frontends querying the
	// source *type*; only instance-backed parts may touch ctx.
	if (ctx)
		ctx->FillActiveSessionList(active_session_list, active_session_add);

	// Active session group
	obs_properties_add_group(ps, SETTING_ACTIVE_SESSION_GROUP, TEXT_ACTIVE_SESSION_GROUP,
				 OBS_GROUP_NORMAL, active_session_group);

	if (ctx)
		ctx->UpdateStatus(ps);

	return ps;
}

static void audio_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, SETTING_MODE, MODE_SESSION);

	auto *executable_list = obs_data_array_create();
	obs_data_set_default_array(settings, SETTING_EXECUTABLE_LIST, executable_list);
	obs_data_array_release(executable_list);

	obs_data_set_default_bool(settings, SETTING_EXCLUDE, false);
	obs_data_set_default_int(settings, SETTING_LATENCY, 0);
}

static const char *audio_capture_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return TEXT_NAME;
}

struct obs_source_info audio_capture_info = {
	.id = "audio_capture",

	.type = OBS_SOURCE_TYPE_INPUT,
	// DO_NOT_SELF_MONITOR: exclude mode captures desktop-wide audio, which
	// includes the monitoring device; without the flag, monitoring this
	// source feeds it back into itself (same reason OBS's own desktop audio
	// sources set it).
	.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_DO_NOT_SELF_MONITOR,

	.get_name = audio_capture_get_name,

	.create = audio_capture_create,
	.destroy = audio_capture_destroy,

	.get_defaults = audio_capture_defaults,
	.get_properties = audio_capture_properties,

	.update = audio_capture_update,

	.icon_type = OBS_ICON_TYPE_PROCESS_AUDIO_OUTPUT,
};
