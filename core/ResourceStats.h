// core/ResourceStats.h - the resource cost of the deck AND the scripts it runs.
//
// m107 introduced this as "the host's own footprint". That was honest while
// scripts were threads inside the host process. Since m192 each running script
// is its own official-kernel process, so measuring only GetCurrentProcess()
// under-reported the fleet by every child. m196 fixes that: SampleWithChildren()
// takes the pids of the live sessions and returns the host, one sample per
// child, and a total in the SAME units. The status bar draws `total` and the
// per-script list columns draw `children`, so the two cannot disagree.
//
// Direction: core stays free of UI/engine/FLTK. Win32 here is measurement only
// (GetProcessTimes / GetProcessMemoryInfo / Toolhelp / PDH), never a window,
// a hook or a message pump. Pids are plain uint32_t so this header needs no
// windows.h.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

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

// One running script's reading. valid=false means OpenProcess failed (the
// session ended between the engine snapshot and the sample); the UI shows a
// blank cell rather than a misleading 0, and total simply skips it.
struct ChildResource
{
	uint32_t pid = 0;
	bool valid = false;
	ResourceSample sample;
};

struct ResourceSnapshot
{
	ResourceSample host;
	std::vector<ChildResource> children; // same order as the pids passed in
	ResourceSample total;                // host + every valid child, CPU/GPU clamped
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
//
// Sample() is the host-only reading used by --stats. SampleWithChildren() is
// the full picture: one PDH collect covers every process, one Toolhelp
// snapshot counts every thread, and per-pid CPU histories are kept in a map
// keyed by pid. A session that disappears is dropped on the next call, and a
// changed process creation time (pid reused by a different process) resets
// that history so a dead pid can never produce a bogus CPU spike.
class ResourceSampler
{
public:
	// Exposed only because the free helpers in ResourceStats.cpp take it by
	// reference; it is an implementation detail of the sampler, not an API.
	struct CpuHistory
	{
		uint64_t created100ns = 0;
		uint64_t cpu100ns = 0;
	};

	ResourceSampler();
	~ResourceSampler();
	ResourceSampler(const ResourceSampler &) = delete;
	ResourceSampler &operator=(const ResourceSampler &) = delete;

	ResourceSample Sample();
	ResourceSnapshot SampleWithChildren(const std::vector<uint32_t> &aChildPids = {});

private:
	void *mQuery = nullptr;      // PDH_HQUERY
	void *mGpuCounter = nullptr; // PDH_HCOUNTER
	CpuHistory mHostCpu;
	std::map<uint32_t, CpuHistory> mChildCpu;
	uint64_t mPrevWall100ns = 0; // one wall clock shared by every process
	unsigned mCores = 1;
};

} // namespace ahkdeck
