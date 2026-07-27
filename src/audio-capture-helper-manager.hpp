#pragma once

#include <climits>
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
	std::unordered_map<DWORD, AudioCaptureHelper> helpers;

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

	void RegisterMixer(DWORD pid, Mixer *mixer)
	{
		auto lock = helpers_section.lock();

		try {
			// The helper must produce what this mixer expects, so take the
			// format from the mixer rather than from a fresh query.
			auto [it, inserted] = helpers.try_emplace(pid, mixer, mixer->GetFormat(), pid);
			if (!inserted)
				it->second.RegisterMixer(mixer);
		} catch (const wil::ResultException &e) {
			error("failed to create helper... update Windows?");
			error("%s", e.what());
		}
	};

	void UnRegisterMixer(DWORD pid, Mixer *mixer)
	{
		auto lock = helpers_section.lock();

		auto it = helpers.find(pid);
		if (it == helpers.end())
			return;

		auto remove_helper = it->second.UnRegisterMixer(mixer);
		if (remove_helper)
			helpers.erase(it);
	};
};