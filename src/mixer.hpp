#pragma once

#include <atomic>
#include <thread>
#include <queue>
#include <vector>

#include <windows.h>
#include <mmreg.h>
#include <wil/resource.h>

#include <obs.h>

#include "common.hpp"

namespace MixerEvents {
enum MixerEvents {
	Shutdown = WM_USER,
	Tick,
};
}

class Mixer {
private:
	static const UINT64 ms_in_ts = 10000;

	// cutoff_end is how long we wait for late packets from other capture
	// helpers before shipping audio to OBS, i.e. the latency this source
	// adds. cutoff_start bounds how far back we still emit. Runtime-settable:
	// capturing a single application does not need the safety margin that
	// mixing several does.
	std::atomic<UINT64> cutoff_start{120 * ms_in_ts};
	std::atomic<UINT64> cutoff_end{40 * ms_in_ts};

	static const DWORD tick_interval = 10;

	obs_source_t *source;

	std::thread worker_thread;
	DWORD worker_tid;
	wil::unique_event worker_ready{wil::EventOptions::ManualReset};

	HANDLE timer = NULL;

	WAVEFORMATEX format;

	wil::critical_section input_section;
	std::queue<std::tuple<UINT64, std::vector<float>>> input_queue;

	UINT64 mix_timestamp = 0;
	std::vector<float> mix;

	UINT64 GetCurrentTimestamp();
	std::size_t DurationToFrames(UINT64 duration);
	UINT64 FramesToDuration(std::size_t frames);

	std::size_t TimestampToMixOffset(UINT64 timestamp);
	std::tuple<std::size_t, std::size_t> CalculateCutoff(UINT64 timestamp);

	void ProcessInput(UINT64 input_timestamp, std::vector<float> &input_buffer);
	void ProcessInput();

	void Tick();
	void Run();

public:
	WAVEFORMATEX GetFormat() { return format; }

	void SetLowLatency(bool low)
	{
		cutoff_start = (low ? 60 : 120) * ms_in_ts;
		cutoff_end = (low ? 20 : 40) * ms_in_ts;
	}

	void SubmitPacket(UINT64 timestamp, float *data, UINT32 num_frames);

	Mixer(obs_source_t *source, WAVEFORMATEX format);
	~Mixer();
};