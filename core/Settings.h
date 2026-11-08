// Window geometry persistence (m017).
//
// Four integers in a hand-editable key=value file under the user's data
// directory. The positional data is the user's, not ours: never put it in the
// source tree or logs (AGENTS.md rule 9 applies to shipped artifacts).

#pragma once

#include <string>

namespace ahkdeck
{

struct WindowGeometry
{
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;
	// m027: frame appearance. Defaults are on; an old ui.ini without these
	// keys keeps the deck look instead of silently turning it off.
	bool rounded = true;
	bool shadow = true;
	bool valid = false;
};

// AHKDECK_SETTINGS override first, then ui.ini next to the exe (portable: a
// copied folder carries its settings; %LOCALAPPDATA% did not).
std::wstring DefaultSettingsPath();

// A missing file is normal (first run): returns true with valid=false.
bool LoadUiSettings(const std::wstring &aPath, WindowGeometry &aOut, std::string &aError);

// Creates the parent directory when needed; writes CRLF/UTF-8 without BOM.
bool SaveUiSettings(const std::wstring &aPath, const WindowGeometry &aGeometry, std::string &aError);

} // namespace ahkdeck
