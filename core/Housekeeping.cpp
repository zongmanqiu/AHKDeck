#include "core/Housekeeping.h"

#include "core/Json.h"

#include <windows.h>

namespace ahkdeck
{

namespace
{

// 24 hours in FILETIME ticks (100 ns units).
constexpr std::uint64_t kDay100ns = 864000000000ULL;

std::uint64_t LastWriteTicks(const WIN32_FIND_DATAW &aData)
{
	return (static_cast<std::uint64_t>(aData.ftLastWriteTime.dwHighDateTime) << 32) |
		static_cast<std::uint64_t>(aData.ftLastWriteTime.dwLowDateTime);
}

// Depth-first: children first, then the (now possibly empty) directory itself.
int PruneDirectory(const std::wstring &aDir, std::uint64_t aCutoff, int &aFailed,
	bool aRemoveIfEmpty)
{
	WIN32_FIND_DATAW data = {};
	const std::wstring pattern = aDir + L"\\*";
	HANDLE find = FindFirstFileW(pattern.c_str(), &data);
	if (find == INVALID_HANDLE_VALUE)
		return 0;
	int removed = 0;
	do
	{
		const std::wstring name = data.cFileName;
		if (name == L"." || name == L"..")
			continue;
		const std::wstring path = aDir + L"\\" + name;
		const DWORD attributes = data.dwFileAttributes;
		if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
			continue; // never follow junctions or delete links as if they were files
		if (attributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			removed += PruneDirectory(path, aCutoff, aFailed, true);
			continue;
		}
		if (!IsOlderThan(LastWriteTicks(data), aCutoff))
			continue;
		if (DeleteFileW(path.c_str()))
			++removed;
		else
			++aFailed; // still open (or read-only): keep it, report the count
	} while (FindNextFileW(find, &data));
	FindClose(find);
	if (aRemoveIfEmpty)
		RemoveDirectoryW(aDir.c_str()); // non-empty fails, which is expected
	return removed;
}

} // namespace

int RetentionDaysFromJson(const Json &aRoot)
{
	const Json &housekeeping = aRoot["housekeeping"];
	if (!housekeeping.IsObject())
		return kHousekeepingDefaultDays;
	const int days = housekeeping["max_age_days"].AsInt(kHousekeepingDefaultDays);
	return days < 0 ? 0 : days;
}

std::uint64_t CutoffTicks(std::uint64_t aNowTicks, int aDays)
{
	// Disabled (or a nonsense setting): nothing is older than 1601-01-01.
	if (aDays <= 0)
		return 0;
	const std::uint64_t span = kDay100ns * static_cast<std::uint64_t>(aDays);
	return span >= aNowTicks ? 0 : aNowTicks - span;
}

bool IsOlderThan(std::uint64_t aLastWriteTicks, std::uint64_t aCutoffTicks)
{
	return aLastWriteTicks < aCutoffTicks;
}

int PruneOlderThan(const std::wstring &aDir, int aDays, std::uint64_t aNowTicks,
	std::string &aError)
{
	aError.clear();
	if (aDays <= 0)
		return 0;
	const DWORD attributes = GetFileAttributesW(aDir.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
		return 0;
	int failed = 0;
	const int removed = PruneDirectory(aDir, CutoffTicks(aNowTicks, aDays), failed, false);
	if (failed > 0)
		aError = std::to_string(failed) + " 个文件仍被占用，本次未删除";
	return removed;
}

} // namespace ahkdeck
