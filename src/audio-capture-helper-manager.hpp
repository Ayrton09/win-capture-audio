#pragma once

#include <climits>
#include <exception>
#include <unordered_map>
#include <tuple>
#include <set>

#include <windows.h>
#include <mmreg.h>

#include <wil/resource.h>

#include "common.hpp"
#include "audio-capture-helper.hpp"

class AudioCaptureHelperManager {
private:
	wil::critical_section helpers_section;

	// Keyed by pid *and* mode: the same process can legitimately be the target
	// of an include capture (one source captures Spotify) and of an exclude
	// capture (another source captures everything but Spotify) at once.
	std::unordered_map<uint64_t, AudioCaptureHelper> helpers;

	static uint64_t MakeKey(DWORD pid, bool exclude)
	{
		return static_cast<uint64_t>(pid) | (exclude ? (1ull << 32) : 0ull);
	}

public:
	AudioCaptureHelperManager() = default;
	~AudioCaptureHelperManager() = default;

	// Queried live rather than snapshotted at DLL load: this object is a global,
	// and its constructor used to run at static-init time and keep whatever
	// obs_get_audio_info returned then, forever. It also stored the raw
	// speaker_layout enum value as the channel count, which is wrong for 4.1
	// (enum 5 = 5 channels only by coincidence) and 7.1 (enum 8).
	WAVEFORMATEX GetFormat()
	{
		WORD channels = 2;
		DWORD samples_per_sec = 48000;

		obs_audio_info info;
		if (obs_get_audio_info(&info)) {
			WORD n = (WORD)get_audio_channels(info.speakers);
			if (n > 0)
				channels = n;
			samples_per_sec = info.samples_per_sec;
		} else {
			warn("obs_get_audio_info failed, assuming 48kHz stereo");
		}

		WAVEFORMATEX format;
		format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
		format.nChannels = channels;
		format.nSamplesPerSec = samples_per_sec;

		format.nBlockAlign = format.nChannels * sizeof(float);
		format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
		format.wBitsPerSample = CHAR_BIT * sizeof(float);
		format.cbSize = 0;

		return format;
	}

	void RegisterMixer(DWORD pid, bool exclude, Mixer *mixer)
	{
		auto lock = helpers_section.lock();

		try {
			// The helper must produce what this mixer expects, so take the
			// format from the mixer rather than from a fresh query.
			auto format = mixer->GetFormat();
			auto [it, inserted] =
				helpers.try_emplace(MakeKey(pid, exclude), mixer, format, pid, exclude);
			if (!inserted) {
				// Sources created before and after an OBS audio-settings
				// change can carry different formats. The helper feeds the
				// mixer raw frames sized by the helper's own format, so
				// attaching a mismatched mixer would make SubmitPacket
				// read/copy out of bounds - refuse instead.
				auto existing = it->second.GetFormat();
				if (existing.nChannels != format.nChannels ||
				    existing.nSamplesPerSec != format.nSamplesPerSec) {
					error("format mismatch on pid %lu helper "
					      "(%u ch @ %lu Hz vs %u ch @ %lu Hz) - "
					      "not attaching; recreate the source or restart OBS",
					      pid, existing.nChannels, existing.nSamplesPerSec,
					      format.nChannels, format.nSamplesPerSec);
					return;
				}

				it->second.RegisterMixer(mixer);
			}
		} catch (const wil::ResultException &e) {
			error("failed to create helper... update Windows?");
			error("%s", e.what());
		} catch (const std::exception &e) {
			// Anything escaping here would unwind through the source's
			// worker thread and std::terminate all of OBS.
			error("failed to create helper for pid %lu: %s", pid, e.what());
		}
	};

	// S_OK when the helper is healthy or does not exist (not registered yet,
	// or already torn down); the failing HRESULT otherwise.
	HRESULT GetHelperError(DWORD pid, bool exclude)
	{
		auto lock = helpers_section.lock();

		auto it = helpers.find(MakeKey(pid, exclude));
		return it == helpers.end() ? S_OK : it->second.GetLastError();
	}

	void UnRegisterMixer(DWORD pid, bool exclude, Mixer *mixer)
	{
		auto lock = helpers_section.lock();

		auto it = helpers.find(MakeKey(pid, exclude));
		if (it == helpers.end())
			return;

		auto remove_helper = it->second.UnRegisterMixer(mixer);
		if (!remove_helper)
			return;

		// ~AudioCaptureHelper joins its capture thread, which can be stuck
		// inside a WASAPI call; release the map lock first so a slow helper
		// teardown cannot stall every other source's register/unregister.
		auto node = helpers.extract(it);
		lock.reset();
	};
};