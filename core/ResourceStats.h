// core/ResourceStats.h - how much of this machine the deck itself is using.
//
// m107: the status bar names the engine, the script count and the running count,
// but says nothing about the host's own footprint. This module answers one
// question in numbers a user can act on: CPU%, memory, threads, handles, and -
// when Windows exposes it - GPU%.
//
// Every number is the CURRENT PROCESS, and that is also the honest answer for
// "what do my scripts cost": scripts run embedded in this process (closing the
// host closes them), so there is no separate process to point at.
//
// Direction: core stays free of UI/engine/FLTK. Win32 here is measurement only
// (GetProcessTimes / GetProcessMemoryInfo / Toolhelp / PDH), never a window,
// a hook or a message pump.

#pragma once

#include <cstdint>
#include <string>

namespace ahkdeck
{

struct ResourceSample
{
	double cpuPercent = 0.0;      // 0..100, share of the WHOLE machine (Task Manager's rule)
	uint64_t workingSetBytes = 0; // every page the process holds, shared DLLs included
	// Task Manager's "Memory" column: private working set, i.e. the pages this
	// process holds that no other process maps. WorkingSetBytes minus the shared
	// pages; measuring the total would always read higher than Task Manager.
	uint64_t privateWorkingSetBytes = 0;
	uint64_t privateBytes = 0;    // committed bytes only this process owns
	uint32_t threadCount = 0;     // --stats only: too jargon-heavy for the status line
	uint32_t handleCount = 0;     // --stats only: kept for leak diagnosis, not drawn
	double gpuPercent = -1.0;     // < 0 = this machine exposes no GPU counter
};

// Pure: percent of the machine's capacity used between two samples. CPU times
// come from GetProcessTimes (kernel + user) and the wall clock from a monotonic
// tick, both in 100 ns units. aCores <= 0 is treated as 1.
double CpuPercent(uint64_t aPrevCpu100ns, uint64_t aPrevWall100ns,
	uint64_t aCurCpu100ns, uint64_t aCurWall100ns, unsigned aCores);

// 512KB / 52MB / 1.2GB - one decimal only above the gigabyte, so the status
// line stays short.
std::string FormatBytes(uint64_t aBytes);

// "CPU 3% · 内存 52MB · GPU 1%"; the GPU segment is omitted entirely when the
// sample has no counter, rather than printing "GPU -".
// Threads and handles are deliberately NOT here: they are diagnosis numbers
// (leak hunting), and the status line keeps only what a user reads directly.
// `--stats` still prints them, so nothing is lost for a machine reader.
// "内存" is the private working set, so the number matches Task Manager's
// Memory column instead of reading higher by the size of the shared DLLs.
std::string FormatResources(const ResourceSample &aSample);

// Keeps the previous CPU/wall reading and the PDH query between calls, so the
// second and later samples are real deltas. Safe on the UI thread: it only
// reads counters - PDH is collected, never slept on.
class ResourceSampler
{
public:
	ResourceSampler();
	~ResourceSampler();
	ResourceSampler(const ResourceSampler &) = delete;
	ResourceSampler &operator=(const ResourceSampler &) = delete;

	ResourceSample Sample();

private:
	void *mQuery = nullptr;      // PDH_HQUERY
	void *mGpuCounter = nullptr; // PDH_HCOUNTER
	uint64_t mPrevCpu100ns = 0;
	uint64_t mPrevWall100ns = 0;
	unsigned mCores = 1;
};

} // namespace ahkdeck
