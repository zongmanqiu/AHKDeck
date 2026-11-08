// m017: window geometry persistence. cwd = .temp/settings_gate (ctest).

#include "core/Settings.h"

#include "core/TextFile.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace
{

int gFailures = 0;

void Check(bool aCondition, const char *aWhat)
{
	if (aCondition)
		std::printf("ok   %s\n", aWhat);
	else
	{
		std::printf("FAIL %s\n", aWhat);
		++gFailures;
	}
}

} // namespace

int main()
{
	const std::wstring dir = L"settings_gate";
	CreateDirectoryW(dir.c_str(), nullptr);
	const std::wstring path = dir + L"\\ui.ini";
	std::string error;

	// Missing file is the first run.
	{
		ahkdeck::WindowGeometry geometry;
		Check(ahkdeck::LoadUiSettings(dir + L"\\missing.ini", geometry, error), "missing settings is not an error");
		Check(!geometry.valid, "missing settings are not valid");
	}

	// Round-trip with a negative x (secondary monitor to the left).
	{
		ahkdeck::WindowGeometry saved;
		saved.x = -1200;
		saved.y = 40;
		saved.w = 928;
		saved.h = 611;
		Check(ahkdeck::SaveUiSettings(path, saved, error), "save settings");
		Check(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES, "settings file exists");

		ahkdeck::WindowGeometry loaded;
		Check(ahkdeck::LoadUiSettings(path, loaded, error), "load settings");
		Check(loaded.valid, "loaded settings are valid");
		Check(loaded.x == -1200 && loaded.y == 40 && loaded.w == 928 && loaded.h == 611,
			"geometry round-trips exactly");
		Check(loaded.rounded && loaded.shadow, "frame switches default on");
	}

	// m027: the two frame switches round-trip, and an old file without them
	// keeps the default (on) rather than silently going square.
	{
		const std::wstring framePath = dir + L"\\frame.ini";
		Check(ahkdeck::WriteTextDocument(framePath,
			"x=0\ny=0\nw=1180\nh=720\nrounded=0\nshadow=0\n",
			ahkdeck::TextEncoding::Utf8NoBom, true, nullptr, error), "write frame switches");
		ahkdeck::WindowGeometry off;
		Check(ahkdeck::LoadUiSettings(framePath, off, error), "load frame switches");
		Check(off.valid && !off.rounded && !off.shadow, "rounded=0 / shadow=0 round-trip");

		Check(ahkdeck::WriteTextDocument(framePath, "x=0\ny=0\nw=1180\nh=720\n",
			ahkdeck::TextEncoding::Utf8NoBom, true, nullptr, error), "write old-style settings");
		ahkdeck::WindowGeometry old;
		Check(ahkdeck::LoadUiSettings(framePath, old, error), "load old-style settings");
		Check(old.valid && old.rounded && old.shadow, "missing switches default to on");
		DeleteFileW(framePath.c_str());
	}

	// A partial file is not valid but is not an error either.
	{
		const std::wstring partial = dir + L"\\partial.ini";
		Check(ahkdeck::WriteTextDocument(partial, "x=1\ny=2\nw=900\n", ahkdeck::TextEncoding::Utf8NoBom,
			true, nullptr, error), "write partial settings");
		ahkdeck::WindowGeometry geometry;
		Check(ahkdeck::LoadUiSettings(partial, geometry, error), "partial file loads");
		Check(!geometry.valid, "partial file is invalid");
		DeleteFileW(partial.c_str());
	}

	// Override path wins over LOCALAPPDATA.
	{
		SetEnvironmentVariableW(L"AHKDECK_SETTINGS", L"Z:\\explicit\\ui.ini");
		Check(ahkdeck::DefaultSettingsPath() == L"Z:\\explicit\\ui.ini", "AHKDECK_SETTINGS overrides the path");
		SetEnvironmentVariableW(L"AHKDECK_SETTINGS", nullptr);

		std::wstring fallback = ahkdeck::DefaultSettingsPath();
		wchar_t self[MAX_PATH] = {0};
		DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
		std::wstring exeDir(self, n > 0 ? n : 0);
		const size_t cut = exeDir.find_last_of(L"\\/");
		if (cut != std::wstring::npos)
			exeDir.resize(cut);
		Check(fallback == exeDir + L"\\ui.ini", "default path is portable (next to the exe)");
	}

	DeleteFileW(path.c_str());

	if (gFailures)
	{
		std::printf("settings: %d failure(s)\n", gFailures);
		return 1;
	}
	std::printf("settings: all checks passed\n");
	return 0;
}
