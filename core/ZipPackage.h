// core/ZipPackage.h - safe extraction of script packages (m211).
//
// A package is a zip whose root holds exactly one folder `<名>/`, and
// `<名>/<名>.ahk` inside it is the entry script. Extraction is the risky half
// of the package install (the archive comes off the network), so the reader
// here validates every entry name before anything touches the disk: no path
// traversal, no absolute paths, no drive letters, caps on size and count.
#pragma once

#include <string>

namespace ahkdeck
{

// Extracts aZipPath into aDestDir (created on demand). Returns false and fills
// aError on anything suspicious or broken; on failure whatever was already
// extracted is removed, so the caller never keeps a half package.
bool ExtractZipSafe(const std::wstring &aZipPath, const std::wstring &aDestDir,
	std::string &aError);

// Recursively removes a directory tree. Best effort: returns false when some
// entries could not be removed (a running process keeps a lock), but whatever
// could be removed is removed.
bool RemoveTreeWide(const std::wstring &aDir);

// Moves a directory into aTargetDir, generating `<stem>-<stamp>` (plus a
// numeric suffix when needed) so repeated backups never collide. On success
// aOutLeafUtf8 carries the generated leaf name.
bool MoveDirToBackup(const std::wstring &aDir, const std::wstring &aTargetDir,
	std::string &aOutLeafUtf8);

} // namespace ahkdeck
