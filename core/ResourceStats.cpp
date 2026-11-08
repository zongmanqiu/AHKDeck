#include "core/ResourceStats.h"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
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

// Kernel + user time of this process, in 100 ns units (the unit CpuPercent
// expects from both sides).
uint64_t ProcessCpu100ns()
{
	FILETIME created;
	FILETIME exited;
	FILETIME kernel;
	FILETIME user;
	if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
		return 0;
	return FileTime100ns(kernel) + FileTime100ns(user);
}

// GetProcessMemoryInfo exposes no thread count, and the documented way to count
// threads of the current process is a Toolhelp snapshot. It is ~ms work, taken
// once per couple of seconds, so the cost is not worth a private peak API.
uint32_t CountOwnThreads()
{
	const DWORD pid = GetCurrentProcessId();
	uint32_t count = 0;
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return 0;
	THREADENTRY32 entry;
	std::memset(&entry, 0, sizeof(entry));
	entry.dwSize = sizeof(entry);
	if (Thread32First(snapshot, &entry))
	{
		do
		{
			// The owner field is only valid once dwSize covers it (Win32 rule).
			if (entry.dwSize >=
				FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(entry.th32OwnerProcessID) &&
				entry.th32OwnerProcessID == pid)
				++count;
		} while (Thread32Next(snapshot, &entry));
	}
	CloseHandle(snapshot);
	return count;
}

// Sums this process's GPU engine utilization. Instance names look like
// "pid_1234_luid_0x..._phys_0_eng_0_engtype_3D", so the pid prefix is the
// filter. -1 means "the counter could not be read at all", not "0% GPU".
double GpuPercentOfOwnProcess(PDH_HCOUNTER aCounter)
{
	DWORD size = 0;
	DWORD count = 0;
	PDH_STATUS status = PdhGetFormattedCounterArrayW(aCounter, PDH_FMT_DOUBLE,
		&size, &count, nullptr);
	if (status != PDH_MORE_DATA || size == 0)
		return -1.0;

	std::vector<BYTE> buffer(size);
	PPDH_FMT_COUNTERVALUE_ITEM_W items =
		reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buffer.data());
	status = PdhGetFormattedCounterArrayW(aCounter, PDH_FMT_DOUBLE, &size, &count, items);
	if (status != ERROR_SUCCESS)
		return -1.0;

	const std::wstring prefix = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
	double total = 0.0;
	for (DWORD i = 0; i < count; ++i)
	{
		if (!items[i].szName)
			continue;
		if (std::wcsncmp(items[i].szName, prefix.c_str(), prefix.size()) != 0)
			continue;
		if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
			items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
			continue;
		total += items[i].FmtValue.doubleValue;
	}
	// A process can touch several engines; the sum can exceed 100, and a
	// status line saying "GPU 180%" would be noise, so it is clamped.
	return total > 100.0 ? 100.0 : total;
}

// Task Manager's "Memory" column is the PRIVATE working set: the pages this
// process holds that no other process maps. GetProcessMemoryInfo only reports
// the total, so the shared pages have to be subtracted - QueryWorkingSet lists
// every page, QueryWorkingSetEx says whether each one is shared.
//
// Returns 0 when the walk fails; the caller then falls back to the total rather
// than printing "0MB".
uint64_t OwnPrivateWorkingSetBytes(PSIZE_T aTotalWorkingSet)
{
	const HANDLE process = GetCurrentProcess();

	SYSTEM_INFO info;
	std::memset(&info, 0, sizeof(info));
	GetSystemInfo(&info);
	const SIZE_T pageSize = info.dwPageSize ? info.dwPageSize : 4096;

	// One pointer per page, plus slack for a set that grew between the two calls.
	const SIZE_T pageCount = (SIZE_T)(*aTotalWorkingSet / pageSize) + 64;
	std::vector<PVOID> pages(pageCount, nullptr);
	const SIZE_T bufferBytes = pageCount * sizeof(PVOID);
	if (bufferBytes > 0xFFFFFFFFu)
		return 0;
	if (!QueryWorkingSet(process, pages.data(), (DWORD)bufferBytes))
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
	if (!QueryWorkingSetEx(process, entries.data(), (DWORD)entryBytes))
		return 0;

	uint64_t privatePages = 0;
	for (const PSAPI_WORKING_SET_EX_INFORMATION &entry : entries)
	{
		if (entry.VirtualAttributes.Valid && !entry.VirtualAttributes.Shared)
			++privatePages;
	}
	return privatePages * (uint64_t)pageSize;
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
	ResourceSample sample;

	PROCESS_MEMORY_COUNTERS_EX counters;
	std::memset(&counters, 0, sizeof(counters));
	if (GetProcessMemoryInfo(GetCurrentProcess(),
		reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters)))
	{
		sample.workingSetBytes = counters.WorkingSetSize;
		sample.privateBytes = counters.PrivateUsage;
		sample.privateWorkingSetBytes = OwnPrivateWorkingSetBytes(&counters.WorkingSetSize);
	}
	DWORD handles = 0;
	if (GetProcessHandleCount(GetCurrentProcess(), &handles))
		sample.handleCount = handles;
	sample.threadCount = CountOwnThreads();

	// GetTickCount64 is monotonic and costs nothing; a wall clock that jumps
	// with NTP would make one sample read as a spike.
	const uint64_t wall = GetTickCount64() * 10000ULL;
	const uint64_t cpu = ProcessCpu100ns();
	if (mPrevWall100ns != 0)
		sample.cpuPercent = CpuPercent(mPrevCpu100ns, mPrevWall100ns, cpu, wall, mCores);
	mPrevWall100ns = wall;
	mPrevCpu100ns = cpu;

	if (mQuery && mGpuCounter)
	{
		// One collect per sample; the query keeps the previous collect, so the
		// second sample onwards is a real reading. No Sleep here: this runs on
		// the UI thread.
		PdhCollectQueryData(static_cast<PDH_HQUERY>(mQuery));
		sample.gpuPercent = GpuPercentOfOwnProcess(static_cast<PDH_HCOUNTER>(mGpuCounter));
	}
	return sample;
}

} // namespace ahkdeck
