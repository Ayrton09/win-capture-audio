#include "mixer.hpp"
#include "format-conversion.hpp"
#include "wil/result_macros.h"
#include <algorithm>
#include <basetsd.h>
#include <memory>
#include <processthreadsapi.h>
#include <threadpoollegacyapiset.h>
#include <winbase.h>
#include <winuser.h>

UINT64 Mixer::GetCurrentTimestamp()
{
	// The old implementation multiplied QPC ticks by (10000000 / frequency) in
	// integer arithmetic: exact only when QPC runs at 10 MHz, and *zero* (i.e. no
	// audio at all) on machines where the frequency is higher. os_gettime_ns is
	// the same QPC clock, scaled correctly by libobs.
	return os_gettime_ns() / 100;
}

std::size_t Mixer::DurationToFrames(UINT64 duration)
{
	UINT64 duration_ns = duration * 100;
	return (duration_ns * format.nSamplesPerSec) / 1000000000;
}

UINT64 Mixer::FramesToDuration(std::size_t frames)
{
	UINT64 duration_ns = (frames * 1000000000) / format.nSamplesPerSec;
	return duration_ns / 100;
}

void Mixer::ProcessInput(UINT64 input_timestamp, std::vector<float> &input_buffer)
{
	if (mix.size() == 0) {
		mix_timestamp = input_timestamp;
		mix = std::move(input_buffer);

		return;
	}

	if (input_timestamp < mix_timestamp) {
		// Salvage whatever part of a late packet still overlaps the mix
		// window instead of dropping the whole thing; with several capture
		// helpers running, dropping made all but one process intermittent.
		auto skip = format.nChannels * DurationToFrames(mix_timestamp - input_timestamp);
		if (skip >= input_buffer.size()) {
			warn("late mix input packet - dropped");
			return;
		}

		input_buffer.erase(input_buffer.begin(), input_buffer.begin() + skip);
		input_timestamp = mix_timestamp;
	}

	auto offset = format.nChannels * DurationToFrames(input_timestamp - mix_timestamp);

	// A stalled tick timer or a wild input timestamp must not grow the mix
	// without bound; past this point something is broken, so restart cleanly.
	// (Not static: each Mixer's cap must follow its own format.)
	const std::size_t max_mix_samples = format.nChannels * DurationToFrames(5000 * ms_in_ts);
	if (offset + input_buffer.size() > max_mix_samples) {
		warn("mix buffer overflow - resetting");
		mix_timestamp = input_timestamp;
		mix = std::move(input_buffer);
		return;
	}

	if (offset + input_buffer.size() > mix.size())
		mix.resize(offset + input_buffer.size());

	for (std::size_t i = 0; i < input_buffer.size(); ++i)
		mix[offset + i] += input_buffer[i];
}

void Mixer::ProcessInput()
{
	auto lock = input_section.lock();

	while (input_queue.size() > 0) {
		auto &[input_timestamp, input_buffer] = input_queue.front();
		ProcessInput(input_timestamp, input_buffer);
		input_queue.pop();
	}
}

std::size_t Mixer::TimestampToMixOffset(UINT64 timestamp)
{
	if (timestamp < mix_timestamp)
		return 0;

	return std::min<std::size_t>(mix.size() / format.nChannels,
				     DurationToFrames(timestamp - mix_timestamp));
}

std::tuple<std::size_t, std::size_t> Mixer::CalculateCutoff(UINT64 timestamp)
{
	const UINT64 start_window = cutoff_start.load(std::memory_order_relaxed);
	const UINT64 end_window = cutoff_end.load(std::memory_order_relaxed);

	if (timestamp < end_window)
		return {0, 0};

	if (timestamp < start_window)
		return {0, TimestampToMixOffset(timestamp - end_window)};

	auto start = TimestampToMixOffset(timestamp - start_window);
	auto end = TimestampToMixOffset(timestamp - end_window);

	return {start, end};
}

void Mixer::Tick()
{
	ProcessInput();

	if (mix.size() == 0)
		return;

	UINT64 current_timestamp = GetCurrentTimestamp();
	auto [start, end] = CalculateCutoff(current_timestamp);

	if (start * format.nChannels >= mix.size()) {
		mix.clear();
		return;
	} else if (end - start == 0)
		return;

	obs_source_audio obs_packet = {
		.frames = static_cast<UINT32>(end - start),
		.speakers = get_obs_speaker_layout(&format),
		.format = get_obs_format(&format),
		.samples_per_sec = format.nSamplesPerSec,
		.timestamp = (mix_timestamp + FramesToDuration(start)) * 100,
	};

	obs_packet.data[0] = reinterpret_cast<BYTE *>(mix.data() + start * format.nChannels);
	obs_source_output_audio(source, &obs_packet);

	// Drop the consumed head in place; the old code allocated a fresh vector
	// on every tick (100x per second).
	mix.erase(mix.begin(), mix.begin() + end * format.nChannels);

	mix_timestamp += FramesToDuration(end);
}

void Mixer::Run()
{
	// Force message queue creation
	MSG msg;
	PeekMessageA(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);

	worker_ready.SetEvent();

	bool shutdown = false;
	while (!shutdown) {
		auto ret = GetMessage(&msg, reinterpret_cast<HWND>(-1), 0, 0);
		if (ret == 0 || ret == -1) {
			debug("shutting down");
			break;
		}

		switch (msg.message) {
		case MixerEvents::Shutdown:
			debug("shutting down");
			shutdown = true;
			break;

		case MixerEvents::Tick:
			Tick();
			break;
		}
	}
}

void Mixer::SubmitPacket(UINT64 timestamp, float *data, UINT32 num_frames)
{
	auto lock = input_section.lock();

	auto &[_, buffer] = input_queue.emplace(timestamp, 0);
	buffer.assign(data, data + num_frames * format.nChannels);

	lock.reset();
}

static void CALLBACK post_tick(PVOID param, BOOLEAN)
{
	auto worker_tid = static_cast<DWORD>(reinterpret_cast<uintptr_t>(param));
	PostThreadMessageW(worker_tid, MixerEvents::Tick, NULL, NULL);
}

Mixer::Mixer(obs_source_t *source, WAVEFORMATEX format) : source{source}, format{format}
{
	worker_thread = std::thread(&Mixer::Run, this);
	worker_tid = GetThreadId(worker_thread.native_handle());

	SetThreadPriority(worker_thread.native_handle(), THREAD_PRIORITY_HIGHEST);
	if (!CreateTimerQueueTimer(&timer, NULL, post_tick,
				   reinterpret_cast<void *>(static_cast<uintptr_t>(worker_tid)),
				   tick_interval, tick_interval, WT_EXECUTEINTIMERTHREAD))
		error("failed to create tick timer (%lu)", GetLastError());
}

Mixer::~Mixer()
{
	if (timer && !DeleteTimerQueueTimer(NULL, timer, INVALID_HANDLE_VALUE))
		warn("DeleteTimerQueueTimer failed (%lu)", GetLastError());

	worker_ready.wait();
	PostThreadMessageW(worker_tid, MixerEvents::Shutdown, NULL, NULL);
	worker_thread.join();
}
