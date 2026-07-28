#include <functional>
#include <windows.h>

#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <vector>
#include <stdexcept>

#include <wil/result.h>
#include <wil/result_macros.h>

#include "audio-capture-helper.hpp"
#include "format-conversion.hpp"

AUDIOCLIENT_ACTIVATION_PARAMS AudioCaptureHelper::GetParams()
{
	auto mode = exclude ? PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
			    : PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

	return {
		.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK,
		.ProcessLoopbackParams =
			{
				.TargetProcessId = pid,
				.ProcessLoopbackMode = mode,
			},
	};
}

PROPVARIANT
AudioCaptureHelper::GetPropvariant(AUDIOCLIENT_ACTIVATION_PARAMS *params)
{
	return {
		.vt = VT_BLOB,
		.blob =
			{
				.cbSize = sizeof(*params),
				.pBlobData = (BYTE *)params,
			},
	};
}

// KSAUDIO_SPEAKER_* values from ksmedia.h, spelled out because ksmedia.h cannot
// be included alongside the COM headers this file needs. Matches the layouts
// libobs maps each channel count to (media-io/audio-io.h).
static DWORD GetChannelMask(WORD channels)
{
	switch (channels) {
	case 1:
		return 0x4; // KSAUDIO_SPEAKER_MONO
	case 2:
		return 0x3; // KSAUDIO_SPEAKER_STEREO
	case 3:
		return 0xB; // KSAUDIO_SPEAKER_2POINT1
	case 4:
		return 0x107; // KSAUDIO_SPEAKER_SURROUND
	case 5:
		return 0x10F; // KSAUDIO_SPEAKER_SURROUND | SPEAKER_LOW_FREQUENCY
	case 6:
		return 0x60F; // KSAUDIO_SPEAKER_5POINT1_SURROUND
	case 8:
		return 0x63F; // KSAUDIO_SPEAKER_7POINT1_SURROUND
	}

	return 0;
}

void AudioCaptureHelper::InitClient()
{
	auto params = GetParams();
	auto propvariant = GetPropvariant(&params);

	wil::com_ptr<IActivateAudioInterfaceAsyncOperation> async_op;

	// The completion handler is a COM object and outlives this scope from the
	// system's point of view: keep it heap-allocated and refcounted rather than
	// on the stack.
	auto completion_handler = Make<CompletionHandler>();
	THROW_IF_NULL_ALLOC(completion_handler.Get());

	THROW_IF_FAILED(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
						    __uuidof(IAudioClient), &propvariant,
						    completion_handler.Get(), &async_op));

	completion_handler->event_finished.wait();
	THROW_IF_FAILED(completion_handler->activate_hr);

	client = completion_handler->client;

	// A bare WAVEFORMATEX carries no channel mask, which breaks layouts beyond
	// stereo. Mirror what OBS's own process-loopback source passes: an
	// extensible format with an explicit mask and float subformat.
	WAVEFORMATEXTENSIBLE wfext = {};
	wfext.Format = format;
	wfext.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	wfext.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	wfext.Samples.wValidBitsPerSample = format.wBitsPerSample;
	wfext.dwChannelMask = GetChannelMask(format.nChannels);
	wfext.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

	THROW_IF_FAILED(
		client->Initialize(AUDCLNT_SHAREMODE_SHARED,
				   AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
				   5 * 10000000, 0, &wfext.Format, NULL));

	THROW_IF_FAILED(client->SetEventHandle(events[HelperEvents::PacketReady].get()));
}

void AudioCaptureHelper::InitCapture()
{
	InitClient();
	THROW_IF_FAILED(
		client->GetService(__uuidof(IAudioCaptureClient), capture_client.put_void()));
}

void AudioCaptureHelper::RegisterMixer(Mixer *mixer)
{
	auto lock = mixers_section.lock();
	mixers.insert(mixer);
}

bool AudioCaptureHelper::UnRegisterMixer(Mixer *mixer)
{
	auto lock = mixers_section.lock();
	mixers.erase(mixer);

	return mixers.size() == 0;
}

void AudioCaptureHelper::ForwardToMixers(UINT64 qpc_position, BYTE *data, UINT32 num_frames)
{
	auto lock = mixers_section.lock();

	for (auto *mixer : mixers)
		mixer->SubmitPacket(qpc_position, reinterpret_cast<float *>(data), num_frames);
}

void AudioCaptureHelper::ForwardPacket()
{
	UINT32 num_frames = 0;
	THROW_IF_FAILED(capture_client->GetNextPacketSize(&num_frames));

	while (num_frames > 0) {
		BYTE *new_data;
		DWORD flags;
		UINT64 qpc_position;

		THROW_IF_FAILED(capture_client->GetBuffer(&new_data, &num_frames, &flags, NULL,
							  &qpc_position));

		if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
			// Forward explicit silence instead of a gap: the mixer keeps a
			// continuous timeline and OBS never sees the stream stall.
			silence_buffer.assign((std::size_t)num_frames * format.nChannels, 0.0f);
			ForwardToMixers(qpc_position, reinterpret_cast<BYTE *>(silence_buffer.data()),
					num_frames);
		} else {
			ForwardToMixers(qpc_position, new_data, num_frames);
		}

		if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
			warn("data discontinuity flag set");

		if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)
			warn("timestamp error flag set");

		THROW_IF_FAILED(capture_client->ReleaseBuffer(num_frames));
		THROW_IF_FAILED(capture_client->GetNextPacketSize(&num_frames));
	}
}

void AudioCaptureHelper::Capture()
{
	InitCapture();
	THROW_IF_FAILED(client->Start());

	bool shutdown = false;
	while (!shutdown) {
		auto event_id = WaitForMultipleObjects(static_cast<DWORD>(events.size()),
						       events[0].addressof(), FALSE, INFINITE);

		switch (event_id) {
		case HelperEvents::PacketReady:
			ForwardPacket();
			break;

		case HelperEvents::Shutdown:
			shutdown = true;
			break;

		default:
			error("wait failed with result: %d", event_id);
			shutdown = true;
			break;
		}
	}

	client->Stop();
}

void AudioCaptureHelper::CaptureSafe()
{
	// This thread does all the COM work; initialize COM here, not on the thread
	// that constructed us.
	auto couninit = wil::CoInitializeEx();

	while (true) {
		try {
			Capture();
			return;
		} catch (const wil::ResultException &e) {
			error("capture failed for pid %lu: %s", pid, e.what());
		}

		// The stream dies when the device changes or the target exits
		// (AUDCLNT_E_DEVICE_INVALIDATED and friends). Retry until we are
		// shut down; if the session is truly gone the SessionMonitor will
		// destroy us shortly anyway.
		client = nullptr;
		capture_client = nullptr;

		if (WaitForSingleObject(events[HelperEvents::Shutdown].get(), 2000) == WAIT_OBJECT_0)
			return;

		info("retrying capture for pid %lu", pid);
	}
}

AudioCaptureHelper::AudioCaptureHelper(Mixer *mixer, WAVEFORMATEX format, DWORD pid, bool exclude)
	: pid{pid}, exclude{exclude}, mixers{mixer}, format{format}
{
	for (auto &event : events)
		event.create();

	capture_thread = std::thread(&AudioCaptureHelper::CaptureSafe, this);
}

AudioCaptureHelper::~AudioCaptureHelper()
{
	auto lock = mixers_section.lock();
	mixers.clear();
	lock.reset();

	events[HelperEvents::Shutdown].SetEvent();
	capture_thread.join();
}
