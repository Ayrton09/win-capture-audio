#include <obs-module.h>
#include "session-monitor.hpp"
#include "common.hpp"
#include "plugin-macros.generated.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("win-capture-audio", "en-GB")
OBS_MODULE_AUTHOR(PLUGIN_AUTHOR)

// Shown by the OBS 32 Plugin Manager.
MODULE_EXPORT const char *obs_module_name(void)
{
	return "win-capture-audio";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Application Audio Output Capture";
}

extern struct obs_source_info audio_capture_info;

// Process loopback (AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK) first shipped
// in Windows 10 2004. On older builds every capture fails at activation; the
// properties dialog says why instead of just never capturing.
bool windows_supports_process_loopback = true;

static DWORD GetWindowsBuild()
{
	// RtlGetVersion is not subject to the manifest-based lying GetVersionEx
	// does, and OBS's own manifest is not ours to rely on.
	using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);

	auto *ntdll = GetModuleHandleW(L"ntdll.dll");
	auto *fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(
				   reinterpret_cast<void *>(GetProcAddress(ntdll, "RtlGetVersion")))
			 : nullptr;

	OSVERSIONINFOW info = {};
	info.dwOSVersionInfoSize = sizeof(info);
	if (!fn || fn(&info) != 0)
		return 0;

	return info.dwBuildNumber;
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[win-capture-audio] Version %s (%s)", PLUGIN_VERSION, GIT_HASH);

	constexpr DWORD min_build = 19041; // Windows 10 2004
	DWORD build = GetWindowsBuild();
	if (build != 0 && build < min_build) {
		windows_supports_process_loopback = false;
		blog(LOG_WARNING,
		     "[win-capture-audio] Windows build %lu is older than %lu (Windows 10 2004): "
		     "application audio capture is not available",
		     build, min_build);
	}

	SessionMonitor::Create();

	obs_register_source(&audio_capture_info);
	return true;
}

void obs_module_unload()
{
	SessionMonitor::Destroy();
}
