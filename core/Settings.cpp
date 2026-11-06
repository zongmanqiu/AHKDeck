#include "core/Settings.h"

#include "core/TextFile.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace ahkdeck
{
namespace
{

bool ReadInt(const std::string &aText, const char *aKey, int &aOut)
{
	std::string needle = aKey;
	needle += "=";
	size_t begin = 0;
	while (begin <= aText.size())
	{
		size_t end = aText.find('\n', begin);
		if (end == std::string::npos)
			end = aText.size();
		std::string line = aText.substr(begin, end - begin);
		if (line.compare(0, needle.size(), needle) == 0)
		{
			const char *value = line.c_str() + needle.size();
			char *stop = nullptr;
			const long parsed = std::strtol(value, &stop, 10);
			if (stop != value)
			{
				aOut = (int)parsed;
				return true;
			}
			return false;
		}
		if (end == aText.size())
			break;
		begin = end + 1;
	}
	return false;
}

} // namespace

std::wstring DefaultSettingsPath()
{
	wchar_t explicitPath[MAX_PATH] = {0};
	DWORD n = GetEnvironmentVariableW(L"AHKDECK_SETTINGS", explicitPath, MAX_PATH);
	if (n > 0 && n < MAX_PATH)
		return explicitPath;

	wchar_t base[MAX_PATH] = {0};
	n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return std::wstring();
	return std::wstring(base) + L"\\AHKDeck\\ui.ini";
}

bool LoadUiSettings(const std::wstring &aPath, WindowGeometry &aOut, std::string &aError)
{
	aOut = WindowGeometry();
	if (GetFileAttributesW(aPath.c_str()) == INVALID_FILE_ATTRIBUTES)
		return true; // first run

	TextDocument document;
	if (!ReadTextDocument(aPath, document, aError))
		return false;

	WindowGeometry geometry;
	const bool hasX = ReadInt(document.utf8, "x", geometry.x);
	const bool hasY = ReadInt(document.utf8, "y", geometry.y);
	const bool hasW = ReadInt(document.utf8, "w", geometry.w);
	const bool hasH = ReadInt(document.utf8, "h", geometry.h);
	// Absent keys keep the constructor defaults (true), so pre-m027 files do
	// not turn the frame off just because the switch did not exist yet.
	{
		int rounded = 1;
		int shadow = 1;
		ReadInt(document.utf8, "rounded", rounded);
		ReadInt(document.utf8, "shadow", shadow);
		geometry.rounded = rounded != 0;
		geometry.shadow = shadow != 0;
	}
	geometry.valid = hasX && hasY && hasW && hasH && geometry.w > 0 && geometry.h > 0;
	aOut = geometry;
	return true;
}

bool SaveUiSettings(const std::wstring &aPath, const WindowGeometry &aGeometry, std::string &aError)
{
	size_t slash = aPath.find_last_of(L"\\/");
	if (slash != std::wstring::npos)
	{
		std::wstring directory = aPath.substr(0, slash);
		CreateDirectoryW(directory.c_str(), nullptr);
	}

	char text[160];
	std::snprintf(text, sizeof(text), "x=%d\ny=%d\nw=%d\nh=%d\nrounded=%d\nshadow=%d\n",
		aGeometry.x, aGeometry.y, aGeometry.w, aGeometry.h,
		aGeometry.rounded ? 1 : 0, aGeometry.shadow ? 1 : 0);
	return WriteTextDocument(aPath, text, TextEncoding::Utf8NoBom, true, nullptr, aError);
}

} // namespace ahkdeck
