// The user's script library (m016).
//
// One UTF-8 path per line. The list lives in the user's data directory, never
// next to the exe (the shipped folder must stay reproducible). Only paths the
// user added by hand belong here - Scripts\*.ahk is rescanned on every start.
//
// Direction: core has no UI and no engine dependency.

#pragma once

#include <string>
#include <vector>

namespace ahkdeck
{

// %LOCALAPPDATA%\AHKDeck\library.txt, or empty when the variable is missing.
std::wstring DefaultLibraryPath();

// A missing file is not an error: it is the first run. Blank lines are skipped
// and duplicate paths (case-insensitive, slash-insensitive) are dropped.
bool LoadScriptLibrary(const std::wstring &aPath, std::vector<std::wstring> &aOut, std::string &aError);

// Creates the parent directory when needed and writes CRLF/UTF-8 without BOM.
bool SaveScriptLibrary(const std::wstring &aPath, const std::vector<std::wstring> &aPaths, std::string &aError);

} // namespace ahkdeck
