#include <windows.h>
#include <processthreadsapi.h>
#include <psapi.h>

#include <wil/result_macros.h>
#include <winuser.h>

#include "session-monitor.hpp"

static SessionMonitor *instance = nullptr;

void SessionMonitor::Create()
{
	instance = new SessionMonitor();
}

void SessionMonitor::Destroy()
{
	delete instance;
	instance = nullptr;
}

SessionMonitor *SessionMonitor::Instance()
{
	return instance;
}

DeviceWatcher::DeviceWatcher(std::wstring device_id, wil::com_ptr<IMMDevice> device,
			     DWORD worker_tid)
	: device_id{device_id},
	  device{device},
	  worker_tid{worker_tid},
	  session_notification_client{worker_tid}
{
	THROW_IF_FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL,
					 manager2.put_void()));

	THROW_IF_FAILED(manager2->RegisterSessionNotification(&session_notification_client));

	THROW_IF_FAILED(manager2->GetSessionEnumerator(enumerator.put()));

	int num_sessions = 0;
	THROW_IF_FAILED(enumerator->GetCount(&num_sessions));

	for (int i = 0; i < num_sessions; ++i) {
		wil::com_ptr<IAudioSessionControl> session;
		THROW_IF_FAILED(enumerator->GetSession(i, session.put()));

		AudioSessionState state;
		THROW_IF_FAILED(session->GetState(&state));

		if (state != AudioSessionStateExpired) {
			// One AddRef, owned by the posted message; AddSession takes
			// it over. The second unconditional AddRef the old code did
			// leaked every enumerated session.
			session->AddRef();
			if (!PostThreadMessageA(worker_tid, SessionEvents::SessionAdded,
						reinterpret_cast<WPARAM>(session.get()), NULL))
				session->Release();
		}
	}
}

DeviceWatcher::~DeviceWatcher()
{
	manager2->UnregisterSessionNotification(&session_notification_client);
}

static std::string WideToUtf8(const wchar_t *wide)
{
	if (!wide || !*wide)
		return {};

	auto num_chars = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
	if (num_chars <= 0)
		return {};

	std::string out(static_cast<std::size_t>(num_chars) - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), num_chars, NULL, NULL);
	return out;
}

// WASAPI session identifiers look like
//   {device-guid}|\Device\HarddiskVolume3\...\app.exe%b{instance-guid}
// so the executable name can be recovered from the identifier alone. That works
// even for elevated/protected processes (anti-cheat games) that OpenProcess
// cannot touch, which previously all showed up as "unknown".
static std::string ExecutableFromSessionId(const std::wstring &session_id)
{
	auto head = session_id.substr(0, session_id.rfind(L"%b"));

	auto slash = head.find_last_of(L"\\/");
	if (slash == std::wstring::npos || slash + 1 >= head.size())
		return {};

	return WideToUtf8(head.substr(slash + 1).c_str());
}

std::string SessionWatcher::ResolveExecutable()
{
	// Preferred: the real image name of the process.
	wil::unique_process_handle session_process{
		OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid)};

	if (session_process) {
		wchar_t name_buf[MAX_PATH] = {L'\0'};
		if (GetProcessImageFileNameW(session_process.get(), name_buf, MAX_PATH)) {
			auto path = WideToUtf8(name_buf);
			auto name = path.substr(path.find_last_of('\\') + 1);
			if (!name.empty())
				return name;
		}
	}

	// Fallback 1: parse it out of the session identifier.
	auto name = ExecutableFromSessionId(session_id);
	if (!name.empty())
		return name;

	// Fallback 2: whatever display name the application registered.
	wil::unique_cotaskmem_string display;
	if (SUCCEEDED(session_control->GetDisplayName(display.put()))) {
		auto display_name = WideToUtf8(display.get());
		if (!display_name.empty())
			return display_name;
	}

	warn("could not resolve an executable name for pid %lu", pid);
	return std::string("unknown");
}

SessionWatcher::SessionWatcher(DWORD worker_tid,
			       const wil::com_ptr<IAudioSessionControl> &session_control)
	: session_control{session_control}
{
	wil::unique_cotaskmem_string session_id_raw;
	THROW_IF_FAILED(GetSessionControl2()->GetSessionIdentifier(session_id_raw.put()));

	session_id = session_id_raw.get();

	THROW_IF_FAILED(GetSessionControl2()->GetProcessId(&pid));

	notification_client.emplace(worker_tid, SessionKey(pid, session_id));

	THROW_IF_FAILED(
		session_control->RegisterAudioSessionNotification(&notification_client.value()));

	executable = ResolveExecutable();
	debug("registered new session: [%d] %s", pid, executable.c_str());
}

SessionWatcher::~SessionWatcher()
{
	session_control->UnregisterAudioSessionNotification(&notification_client.value());
	debug("session expired: [%d] %s", pid, executable.c_str());
}

void SessionMonitor::Init()
{
	enumerator = wil::CoCreateInstance<MMDeviceEnumerator, IMMDeviceEnumerator>();
	THROW_IF_FAILED(
		enumerator->RegisterEndpointNotificationCallback(&device_notification_client));

	wil::com_ptr<IMMDeviceCollection> collection;
	THROW_IF_FAILED(
		enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.put()));

	UINT num_devices = 0;
	THROW_IF_FAILED(collection->GetCount(&num_devices));

	for (UINT i = 0; i < num_devices; ++i) {
		wil::com_ptr<IMMDevice> device;
		THROW_IF_FAILED(collection->Item(i, device.put()));

		wil::unique_cotaskmem_string device_id;
		THROW_IF_FAILED(device->GetId(device_id.put()));

		AddDevice(std::wstring(device_id.get()), device);
	}
}

void SessionMonitor::UnInit()
{
	// Non-throwing on purpose: this runs on failure paths and at shutdown, where
	// the enumerator may not exist or the callback may never have registered.
	if (enumerator)
		enumerator->UnregisterEndpointNotificationCallback(&device_notification_client);
}

void SessionMonitor::AddDevice(MSG msg)
{
	std::unique_ptr<std::wstring> id(reinterpret_cast<std::wstring *>(msg.wParam));

	wil::com_ptr<IMMDevice> device;
	THROW_IF_FAILED(enumerator->GetDevice(id->c_str(), device.put()));

	wil::com_ptr<IMMEndpoint> endpoint = device.query<IMMEndpoint>();
	EDataFlow data_flow;
	THROW_IF_FAILED(endpoint->GetDataFlow(&data_flow));

	if (data_flow == eRender)
		AddDevice(*id, device);
}

void SessionMonitor::AddDevice(std::wstring id, wil::com_ptr<IMMDevice> device)
{
	device_watchers.try_emplace(id, id, device, worker_tid);
	debug("registered new device: %ls", id.c_str());
}

void SessionMonitor::RemoveDevice(MSG msg)
{
	std::unique_ptr<std::wstring> id(reinterpret_cast<std::wstring *>(msg.wParam));
	RemoveDevice(*id);
}

void SessionMonitor::RemoveDevice(std::wstring id)
{
	if (!device_watchers.contains(id))
		return;

	device_watchers.erase(id);
	debug("removed device: %ls", id.c_str());
}

void SessionMonitor::AddSession(MSG msg)
{
	auto session_control_ptr = reinterpret_cast<IAudioSessionControl *>(msg.wParam);

	wil::com_ptr<IAudioSessionControl> session_control;
	*session_control.put() = session_control_ptr;

	auto session_control2 = session_control.query<IAudioSessionControl2>();
	if (session_control2->IsSystemSoundsSession() == S_OK)
		return;

	std::wstring session_id;
	DWORD pid;

	std::string executable;

	try {
		wil::unique_cotaskmem_string session_id_raw;
		THROW_IF_FAILED(session_control2->GetSessionIdentifier(session_id_raw.put()));

		session_id = std::wstring(session_id_raw.get());
		THROW_IF_FAILED(session_control2->GetProcessId(&pid));

		if (session_watchers.contains({pid, session_id}))
			return;

		auto [it, inserted] = session_watchers.try_emplace({pid, session_id}, worker_tid,
								   session_control);

		if (!inserted)
			return;

		executable = it->second.GetExecutable();
	} catch (wil::ResultException e) {
		error("unable to add session: %s", e.what());
		return;
	}

	{
		auto lock = sessions_lock.lock();
		sessions_list.emplace(SessionKey(pid, session_id), executable);
	}

	{
		auto lock = callbacks_lock.lock();
		for (auto [client_tid, msgs] : callbacks)
			PostThreadMessageA(client_tid, std::get<0>(msgs), 0, 0);
	}
}

void SessionMonitor::RemoveSession(MSG msg)
{
	std::unique_ptr<SessionKey> session_key(reinterpret_cast<SessionKey *>(msg.wParam));

	if (!session_watchers.contains(*session_key))
		return;

	auto &session = session_watchers.at(*session_key);

	auto executable = session.GetExecutable();
	auto num_removed = session_watchers.erase(*session_key);

	if (num_removed == 0)
		return;

	{
		auto lock = sessions_lock.lock();
		auto itr = sessions_list.find(*session_key);
		if (itr != sessions_list.end())
			sessions_list.erase(itr);
	}

	{
		auto lock = callbacks_lock.lock();
		for (auto [client_tid, msgs] : callbacks)
			PostThreadMessageA(client_tid, std::get<1>(msgs), 0, 0);
	}
}

void SessionMonitor::Run()
{
	// All COM work happens on this thread.
	auto couninit = wil::CoInitializeEx();

	// Written here rather than in the constructor so it is guaranteed visible
	// before any COM callback can post to us.
	worker_tid = GetCurrentThreadId();
	device_notification_client.SetWorkerThreadId(worker_tid);

	// Force message queue creation
	MSG msg;
	PeekMessageA(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);

	worker_ready.SetEvent();

	try {
		Init();
	} catch (const wil::ResultException &e) {
		// Without a device enumerator there is nothing to monitor; leave no
		// half-registered COM callbacks behind.
		error("session monitor init failed: %s", e.what());
		UnInit();
		return;
	}

	bool shutdown = false;
	while (!shutdown) {
		auto ret = GetMessage(&msg, reinterpret_cast<HWND>(-1), 0, 0);
		if (ret == 0 || ret == -1) {
			debug("shutting down");
			break;
		}

		// One flaky device or session must not kill the monitor for the
		// rest of the OBS session.
		try {
			switch (msg.message) {
			case SessionEvents::Shutdown:
				debug("shutting down");
				shutdown = true;
				break;

			case SessionEvents::DeviceAdded:
				AddDevice(msg);
				break;

			case SessionEvents::DeviceRemoved:
				RemoveDevice(msg);
				break;

			case SessionEvents::SessionAdded:
				AddSession(msg);
				break;

			case SessionEvents::SessionExpired:
				RemoveSession(msg);
				break;
			}
		} catch (const wil::ResultException &e) {
			error("failed to process event %u: %s", msg.message, e.what());
		} catch (const std::exception &e) {
			error("failed to process event %u: %s", msg.message, e.what());
		}
	}

	UnInit();
}

void SessionMonitor::SafeRun()
{
	try {
		Run();
	} catch (const wil::ResultException &e) {
		error("%s", e.what());
	}
}

SessionMonitor::SessionMonitor()
{
	worker_thread = std::thread(&SessionMonitor::SafeRun, this);

	// Run() publishes worker_tid; wait for it so Instance() users can safely
	// register callbacks the moment Create() returns.
	worker_ready.wait();
	worker_tid = GetThreadId(worker_thread.native_handle());
}

SessionMonitor::~SessionMonitor()
{
	worker_ready.wait();
	PostThreadMessageW(worker_tid, SessionEvents::Shutdown, NULL, NULL);
	worker_thread.join();
}

void SessionMonitor::RegisterEvent(DWORD client_tid, UINT session_added, UINT session_expired)
{
	auto lock = callbacks_lock.lock();
	callbacks[client_tid] = {session_added, session_expired};
}

void SessionMonitor::UnRegisterEvent(DWORD client_tid)
{
	auto lock = callbacks_lock.lock();
	auto itr = callbacks.find(client_tid);
	if (itr != callbacks.end())
		callbacks.erase(itr);
}

std::unordered_map<SessionKey, std::string> SessionMonitor::GetSessions()
{
	auto lock = sessions_lock.lock();
	return sessions_list;
}
