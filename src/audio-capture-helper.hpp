#pragma once

#include <stdio.h>
#include <array>
#include <functional>
#include <thread>
#include <set>
#include <vector>

#include <windows.h>
#include <mmreg.h>

#include <audiopolicy.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>

#include <wrl/implements.h>
#include <wil/com.h>

#include "mixer.hpp"
#include "common.hpp"

using namespace Microsoft::WRL;

struct CompletionHandler : public RuntimeClass<RuntimeClassFlags<ClassicCom>, FtmBase,
					       IActivateAudioInterfaceCompletionHandler> {
	wil::com_ptr<IAudioClient> client;

	HRESULT activate_hr = E_FAIL;
	wil::unique_event event_finished;

	CompletionHandler() { event_finished.create(); }

	STDMETHOD(ActivateCompleted)
	(IActivateAudioInterfaceAsyncOperation *operation)
	{
		auto set_finished = event_finished.SetEvent_scope_exit();

		RETURN_IF_FAILED(operation->GetActivateResult(&activate_hr, client.put_unknown()));

		if (FAILED(activate_hr))
			error("activate failed (0x%lx)", activate_hr);

		return S_OK;
	}
};

namespace HelperEvents {
enum HelperEvents {
	PacketReady,
	Shutdown,
	Count,
};
};

class AudioCaptureHelper {
private:
	DWORD pid;

	// false: capture this process tree (INCLUDE_TARGET_PROCESS_TREE).
	// true:  capture everything *except* this process tree
	//        (EXCLUDE_TARGET_PROCESS_TREE) - one client covers the whole
	//        system, which is what "capture all audio except X" needs.
	bool exclude;

	wil::critical_section mixers_section;
	std::set<Mixer *> mixers;

	wil::com_ptr<IAudioClient> client;
	wil::com_ptr<IAudioCaptureClient> capture_client;

	WAVEFORMATEX format;
	std::vector<float> silence_buffer;

	std::array<wil::unique_event, HelperEvents::Count> events;
	std::thread capture_thread;

	AUDIOCLIENT_ACTIVATION_PARAMS GetParams();
	PROPVARIANT GetPropvariant(AUDIOCLIENT_ACTIVATION_PARAMS *params);

	void InitClient();
	void InitCapture();

	void ForwardToMixers(UINT64 qpc_position, BYTE *data, UINT32 num_frames);
	void ForwardPacket();

	void Capture();
	void CaptureSafe();

public:
	DWORD GetPid() { return pid; }
	bool IsExclude() { return exclude; }
	WAVEFORMATEX GetFormat() { return format; }

	AudioCaptureHelper(Mixer *mixer, WAVEFORMATEX format, DWORD pid, bool exclude);
	~AudioCaptureHelper();

	void RegisterMixer(Mixer *mixer);
	bool UnRegisterMixer(Mixer *mixer);
};
