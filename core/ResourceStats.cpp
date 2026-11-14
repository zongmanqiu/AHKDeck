#include "core/ResourceStats.h"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ahkdeck
{

namespace
{

uint64_t FileTime100ns(const FILETIME &aTime)
{
	ULARGE_INTEGER value;
	value.LowPart = aTime.dwLowDateTime;
	value.HighPart = aTime.dwHighDateTime;
	return value.QuadPart;
}

// Kernel + user time in 100 ns units, plus the process creation time. The
// latter is the guard against pid reuse: a dead child's pid can be handed to
// an unrelated process, and blindly subtracting the old CPU total would then
// print a bogus spike (or a huge negative delta).
bool ProcessTimes(HANDLE aProcess, uint64_t &aCpu, uint64_t &aCreated)
{
	FILETIME created;
	FILETIME exited;
	FILETIME kernel;
	FILETIME user;
	if (!GetProcessTimes(aProcess, &created, &exited, &kernel, &user))
		return false;
	aCpu = FileTime100ns(kernel) + FileTime100ns(user);
	aCreated = FileTime100ns(created);
	return true;
}

// One Toolhelp snapshot covers the whole machine, so counting threads for the
// host and every child costs one walk rather than one walk per process.
std::map<uint32_t, uint32_t> ThreadsByPid(const std::set<uint32_t> &aPids)
{
	std::map<uint32_t, uint32_t> result;
	for (uint32_t pid : aPids)
		result[pid] = 0;
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return result;
	THREADENTRY32 entry;
	std::memset(&entry, 0, sizeof(entry));
	entry.dwSize = sizeof(entry);
	if (Thread32First(snapshot, &entry))
	{
		do
		{
			// The owner field is only valid once dwSize covers it (Win32 rule).
			if (entry.dwSize <
				FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(entry.th32OwnerProcessID))
				continue;
			auto it = result.find((uint32_t)entry.th32OwnerProcessID);
			if (it != result.end())
				++it->second;
		} while (Thread32Next(snapshot, &entry));
	}
	CloseHandle(snapshot);
	return result;
}

// A single PDH collect exposes every GPU engine instance on the machine;
// instance names look like "pid_1234_luid_0x..._engtype_3D". Parse the pid
// prefix and group by the processes we were asked about. Returns false only
// when the counter itself cannot be read (<0 GPU semantics); an absent pid in
// the map means a legitimate 0% GPU for that process.
bool GpuByPid(PDH_HCOUNTER aCounter, const std::vector<uint32_t> &aPids,
	std::map<uint32_t, double> &aOut)
{
	DWORD size = 0;
	DWORD count = 0;
	PDH_STATUS status = PdhGetFormattedCounterArrayW(aCounter, PDH_FMT_DOUBLE,
		&size, &count, nullptr);
	if (status != PDH_MORE_DATA || size == 0)
		return false;

	std::vector<BYTE> buffer(size);
	PPDH_FMT_COUNTERVALUE_ITEM_W items =
		reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buffer.data());
	status = PdhGetFormattedCounterArrayW(aCounter, PDH_FMT_DOUBLE, &size, &count, items);
	if (status != ERROR_SUCCESS)
		return false;

	std::set<uint32_t> wanted(aPids.begin(), aPids.end());
	wanted.insert((uint32_t)GetCurrentProcessId());
	for (uint32_t pid : wanted)
		aOut[pid] = 0.0;
	for (DWORD i = 0; i < count; ++i)
	{
		const wchar_t *name = items[i].szName;
		if (!name || std::wcsncmp(name, L"pid_", 4) != 0)
			continue;
		wchar_t *end = nullptr;
		const unsigned long pid = std::wcstoul(name + 4, &end, 10);
		if (end == name + 4 || !end || *end != L'_' || pid == 0)
			continue;
		auto it = aOut.find((uint32_t)pid);
		if (it == aOut.end())
			continue;
		if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
			items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
			continue;
		it->second += items[i].FmtValue.doubleValue;
	}
	// A process can touch several engines; 180% would be noise in a list cell.
	for (auto &entry : aOut)
		if (entry.second > 100.0)
			entry.second = 100.0;
	return true;
}

// Task Manager's "Memory" column is the PRIVATE working set: the pages this
// process holds that no other process maps. GetProcessMemoryInfo only reports
// the total, so the shared pages have to be subtracted - QueryWorkingSet lists
// every page, QueryWorkingSetEx says whether each one is shared.
//
// Returns 0 when the walk fails; the caller then falls back to the total rather
// than printing "0MB". For another process the handle needs
// PROCESS_QUERY_INFORMATION | PROCESS_VM_READ (OpenProcess in the caller).
uint64_t PrivateWorkingSetBytes(HANDLE aProcess, SIZE_T aTotalWorkingSet)
{
	SYSTEM_INFO info;
	std::memset(&info, 0, sizeof(info));
	GetSystemInfo(&info);
	const SIZE_T pageSize = info.dwPageSize ? info.dwPageSize : 4096;

	// One pointer per page, plus slack for a set that grew between the two calls.
	const SIZE_T pageCount = aTotalWorkingSet / pageSize + 64;
	std::vector<PVOID> pages(pageCount, nullptr);
	const SIZE_T bufferBytes = pageCount * sizeof(PVOID);
	if (bufferBytes > 0xFFFFFFFFu)
		return 0;
	if (!QueryWorkingSet(aProcess, pages.data(), (DWORD)bufferBytes))
		return 0;

	std::vector<PSAPI_WORKING_SET_EX_INFORMATION> entries;
	entries.reserve(pageCount);
	for (SIZE_T i = 0; i < pageCount; ++i)
	{
		// The API writes the addresses it has and leaves the rest as they were;
		// page zero is never a working-set page, so a null slot ends the list.
		if (!pages[i])
			break;
		PSAPI_WORKING_SET_EX_INFORMATION entry;
		std::memset(&entry, 0, sizeof(entry));
		entry.VirtualAddress = pages[i];
		entries.push_back(entry);
	}
	if (entries.empty())
		return 0;
	const SIZE_T entryBytes = entries.size() * sizeof(PSAPI_WORKING_SET_EX_INFORMATION);
	if (entryBytes > 0xFFFFFFFFu)
		return 0;
	if (!QueryWorkingSetEx(aProcess, entries.data(), (DWORD)entryBytes))
		return 0;

	uint64_t privatePages = 0;
	for (const PSAPI_WORKING_SET_EX_INFORMATION &entry : entries)
	{
		if (entry.VirtualAttributes.Valid && !entry.VirtualAttributes.Shared)
			++privatePages;
	}
	return privatePages * (uint64_t)pageSize;
}

// One process, one sample. aHistory is that process's previous CPU reading;
// aPrevWall/aWall are shared by the whole snapshot so every CPU percentage is
// measured over exactly the same interval.
ResourceSample SampleOne(HANDLE aProcess, ResourceSampler::CpuHistory &aHistory,
	uint64_t aPrevWall, uint64_t aWall, unsigned aCores,
	double aGpuPercent, uint32_t aThreads)
{
	ResourceSample sample;
	PROCESS_MEMORY_COUNTERS_EX counters;
	std::memset(&counters, 0, sizeof(counters));
	if (GetProcessMemoryInfo(aProcess,
		reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters)))
	{
		sample.workingSetBytes = counters.WorkingSetSize;
		sample.privateBytes = counters.PrivateUsage;
		sample.privateWorkingSetBytes = PrivateWorkingSetBytes(aProcess, counters.WorkingSetSize);
	}
	DWORD handles = 0;
	if (GetProcessHandleCount(aProcess, &handles))
		sample.handleCount = handles;
	sample.threadCount = aThreads;
	sample.gpuPercent = aGpuPercent;

	uint64_t cpu = 0;
	uint64_t created = 0;
	if (ProcessTimes(aProcess, cpu, created))
	{
		// No delta on the first sighting, or when the pid was recycled: the
		// cumulative counter belongs to a different process in that case.
		if (aPrevWall != 0 && aHistory.cpu100ns != 0 && aHistory.created100ns == created)
			sample.cpuPercent = CpuPercent(aHistory.cpu100ns, aPrevWall, cpu, aWall, aCores);
		aHistory.cpu100ns = cpu;
		aHistory.created100ns = created;
	}
	return sample;
}

void AddInto(ResourceSample &aTotal, const ResourceSample &aPart)
{
	aTotal.cpuPercent += aPart.cpuPercent;
	aTotal.workingSetBytes += aPart.workingSetBytes;
	aTotal.privateWorkingSetBytes += aPart.privateWorkingSetBytes;
	aTotal.privateBytes += aPart.privateBytes;
	aTotal.threadCount += aPart.threadCount;
	aTotal.handleCount += aPart.handleCount;
	if (aTotal.gpuPercent < 0.0 || aPart.gpuPercent < 0.0)
		aTotal.gpuPercent = -1.0;
	else
		aTotal.gpuPercent += aPart.gpuPercent;
}

} // namespace

double CpuPercent(uint64_t aPrevCpu100ns, uint64_t aPrevWall100ns,
	uint64_t aCurCpu100ns, uint64_t aCurWall100ns, unsigned aCores)
{
	if (aCores == 0)
		aCores = 1;
	if (aCurWall100ns <= aPrevWall100ns || aCurCpu100ns <= aPrevCpu100ns)
		return 0.0;
	const double used = (double)(aCurCpu100ns - aPrevCpu100ns);
	const double capacity = (double)(aCurWall100ns - aPrevWall100ns) * (double)aCores;
	const double percent = used * 100.0 / capacity;
	return percent > 100.0 ? 100.0 : percent;
}

std::string FormatBytes(uint64_t aBytes)
{
	const double kb = 1024.0;
	const double mb = kb * 1024.0;
	const double gb = mb * 1024.0;
	char buffer[32];
	if ((double)aBytes >= gb)
		std::snprintf(buffer, sizeof(buffer), "%.1fGB", (double)aBytes / gb);
	else if ((double)aBytes >= mb)
		std::snprintf(buffer, sizeof(buffer), "%.0fMB", (double)aBytes / mb);
	else if ((double)aBytes >= kb)
		std::snprintf(buffer, sizeof(buffer), "%.0fKB", (double)aBytes / kb);
	else
		std::snprintf(buffer, sizeof(buffer), "%lluB", (unsigned long long)aBytes);
	return std::string(buffer);
}

std::string FormatResources(const ResourceSample &aSample)
{
	// Task Manager's "Memory" number is the private working set; fall back to the
	// total only if the page walk was refused, so the line never reads "0MB".
	const uint64_t memory = aSample.privateWorkingSetBytes
		? aSample.privateWorkingSetBytes
		: aSample.workingSetBytes;
	char buffer[96];
	std::snprintf(buffer, sizeof(buffer), "CPU %.0f%% · 内存 %s",
		aSample.cpuPercent, FormatBytes(memory).c_str());
	std::string text(buffer);
	if (aSample.gpuPercent >= 0.0)
	{
		std::snprintf(buffer, sizeof(buffer), " · GPU %.0f%%", aSample.gpuPercent);
		text += buffer;
	}
	return text;
}

ResourceSampler::ResourceSampler()
{
	SYSTEM_INFO info;
	std::memset(&info, 0, sizeof(info));
	GetSystemInfo(&info);
	mCores = info.dwNumberOfProcessors ? info.dwNumberOfProcessors : 1;

	PDH_HQUERY query = nullptr;
	if (PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS)
	{
		mQuery = query;
		PDH_HCOUNTER counter = nullptr;
		// English counter path on purpose: a localized Windows spells the same
		// counter differently, and this deck has to read it on every language.
		if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage",
			0, &counter) == ERROR_SUCCESS)
			mGpuCounter = counter;
	}
}

ResourceSampler::~ResourceSampler()
{
	if (mQuery)
		PdhCloseQuery(static_cast<PDH_HQUERY>(mQuery));
}

ResourceSample ResourceSampler::Sample()
{
	return SampleWithChildren(std::vector<uint32_t>{}).host;
}

ResourceSnapshot ResourceSampler::SampleWithChildren(const std::vector<uint32_t> &aChildPids)
{
	ResourceSnapshot snapshot;
	// GetTickCount64 is monotonic and costs nothing; a wall clock that jumps
	// with NTP would make one sample read as a spike. One reading for the whole
	// fleet, so every CPU delta covers the same interval.
	const uint64_t wall = GetTickCount64() * 10000ULL;
	const uint64_t prevWall = mPrevWall100ns;

	// GPU: one collect, grouped by pid. No Sleep here: this runs on the UI thread.
	std::map<uint32_t, double> gpu;
	bool gpuOk = false;
	if (mQuery && mGpuCounter)
	{
		PdhCollectQueryData(static_cast<PDH_HQUERY>(mQuery));
		gpuOk = GpuByPid(static_cast<PDH_HCOUNTER>(mGpuCounter), aChildPids, gpu);
	}
	auto gpuOf = [&](uint32_t pid) -> double {
		if (!gpuOk)
			return -1.0;
		auto it = gpu.find(pid);
		return it == gpu.end() ? 0.0 : it->second;
	};

	// Threads: one snapshot for the host plus every requested child.
	std::set<uint32_t> wanted(aChildPids.begin(), aChildPids.end());
	const uint32_t hostPid = (uint32_t)GetCurrentProcessId();
	wanted.insert(hostPid);
	const std::map<uint32_t, uint32_t> threads = ThreadsByPid(wanted);
	auto threadsOf = [&](uint32_t pid) -> uint32_t {
		auto it = threads.find(pid);
		return it == threads.end() ? 0u : it->second;
	};

	snapshot.host = SampleOne(GetCurrentProcess(), mHostCpu, prevWall, wall, mCores,
		gpuOf(hostPid), threadsOf(hostPid));

	for (uint32_t pid : aChildPids)
	{
		ChildResource child;
		child.pid = pid;
		// PROCESS_VM_READ is required by QueryWorkingSet/QueryWorkingSetEx for a
		// different process. Our children are same-user, same-integrity, so this
		// normally succeeds; a race with a session ending just leaves a blank cell.
		HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
		if (process)
		{
			auto it = mChildCpu.find(pid);
			if (it == mChildCpu.end())
				it = mChildCpu.emplace(pid, CpuHistory{}).first;
			child.sample = SampleOne(process, it->second, prevWall, wall, mCores,
				gpuOf(pid), threadsOf(pid));
			child.valid = true;
			CloseHandle(process);
		}
		snapshot.children.push_back(child);
	}

	// Forget sessions that ended, so a stale history can never be reused when
	// Windows hands the same pid to a new process. The creation-time check in
	// SampleOne is the second line of defence.
	for (auto it = mChildCpu.begin(); it != mChildCpu.end(); )
	{
		const bool live = std::find(aChildPids.begin(), aChildPids.end(), it->first)
			!= aChildPids.end();
		if (live)
			++it;
		else
			it = mChildCpu.erase(it);
	}
	mPrevWall100ns = wall;

	snapshot.total = snapshot.host;
	for (const ChildResource &child : snapshot.children)
		if (child.valid)
			AddInto(snapshot.total, child.sample);
	// Percentages are shares of the machine; summing processes can cross 100.
	// Plain comparisons, not std::min: windows.h defines min/max as macros.
	if (snapshot.total.cpuPercent > 100.0)
		snapshot.total.cpuPercent = 100.0;
	if (snapshot.total.gpuPercent > 100.0)
		snapshot.total.gpuPercent = 100.0;
	return snapshot;
}

} // namespace ahkdeck
