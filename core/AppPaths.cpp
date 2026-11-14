#include "core/AppPaths.h"

#include <windows.h>

namespace ahkdeck
{

namespace
{

std::string Utf8Of(const std::wstring &aWide)
{
	if (aWide.empty())
		return std::string();
	const int size = WideCharToMultiByte(CP_UTF8, 0, aWide.c_str(), (int)aWide.size(),
		nullptr, 0, nullptr, nullptr);
	if (size <= 0)
		return std::string();
	std::string out((size_t)size, '\0');
	WideCharToMultiByte(CP_UTF8, 0, aWide.c_str(), (int)aWide.size(), &out[0], size,
		nullptr, nullptr);
	return out;
}

std::wstring WithoutTrailingSeparator(const std::wstring &aPath)
{
	std::wstring out = aPath;
	while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/'))
		out.pop_back();
	return out;
}

} // namespace

std::wstring AppDirectory()
{
	wchar_t buffer[MAX_PATH] = {0};
	const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
	if (length == 0 || length >= MAX_PATH)
		return std::wstring();
	std::wstring path(buffer, length);
	const size_t slash = path.find_last_of(L"\\/");
	if (slash == std::wstring::npos)
		return std::wstring();
	path.resize(slash);
	return path;
}

std::wstring TempDirUnder(const std::wstring &aAppDir, const wchar_t *aSub,
	bool aCreate)
{
	if (aAppDir.empty())
		return std::wstring();
	std::wstring path = WithoutTrailingSeparator(aAppDir) + L"\\temp";
	if (aSub && *aSub)
		path += std::wstring(L"\\") + aSub;
	if (aCreate)
	{
		// CreateDirectory does not build parents, and temp\ may not exist yet on
		// the very first run: create each level in turn. An existing directory
		// makes the call fail with ERROR_ALREADY_EXISTS, which is success here.
		for (size_t i = 1; i < path.size(); ++i)
		{
			if (path[i] == L'\\')
				CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
		}
		CreateDirectoryW(path.c_str(), nullptr);
	}
	return path;
}

std::wstring AppTempRoot(bool aCreate)
{
	static const std::wstring app = AppDirectory();
	return TempDirUnder(app, nullptr, aCreate);
}

std::wstring AppTempDir(const wchar_t *aSub, bool aCreate)
{
	static const std::wstring app = AppDirectory();
	return TempDirUnder(app, aSub, aCreate);
}

std::string AppTempRelative(const wchar_t *aSub, const std::string &aLeafUtf8)
{
	// Forward slashes: this string is SHOWN (console, journal, tool results) and
	// it is also a valid Win32 relative path, so it never has to be rewritten.
	std::string out = "temp/";
	if (aSub && *aSub)
	{
		out += Utf8Of(std::wstring(aSub));
		out += "/";
	}
	out += aLeafUtf8;
	return out;
}

void EnsureAppTempLayout()
{
	const std::wstring app = AppDirectory();
	if (app.empty())
		return;
	TempDirUnder(app, nullptr, true);
	TempDirUnder(app, kTempLogs, true);
	TempDirUnder(app, kTempBackups, true);
	TempDirUnder(app, kTempWork, true);
	TempDirUnder(app, kTempDeleted, true);
}

} // namespace ahkdeck
