// core/Updater.h - self-update support (m092).
//
// Flow:
//   1. CheckUpdate()   fetch the manifest (update.json) from the selected
//                      source, fall back to the other one, then to the local
//                      cache; compare versions.
//   2. DownloadFile()  fetch the new zip next to the running exe.
//   3. WriteUpdateBat() emit ahkdeck_update.bat that replaces the files and
//                      restarts the program.
//   4. LaunchUpdater() start that bat hidden; the caller then exits.
//
// The manifest is a static file served raw by both mirrors, not a releases
// API: one URL per source instead of two API dialects, and the release notes
// can be published in more than one language.
//
// No package hash, on purpose: the manifest and the package
// live in the same repository, so a hash there cannot stop anyone who could
// swap the package anyway.
//
// FLTK-free, so it can run on a worker thread.
#pragma once

#include <functional>
#include <string>

namespace ahkdeck
{

// The published manifest (update.json). Flat keys - see the parser.
struct UpdateManifest
{
	std::string version;   // "0.1.1"
	std::string published; // "2026-11-07", display only
	std::string notes;     // Chinese changelog
	std::string notesEn;   // English changelog; empty = fall back to Chinese
	std::string urlGitee;  // full download URL (never constructed here)
	std::string urlGithub;
};

// Outcome of one check attempt.
//
// `online` is the honesty bit: only a manifest fetched during THIS call may be
// reported as "you are up to date". A cache left over from an older build
// reads as "not checked" - claiming "latest" while a newer release exists
// would be a lie.
struct UpdateCheckResult
{
	bool online = false;      // a manifest was fetched this call
	bool fromCache = false;   // the verdict came from update.cache.json
	bool hasUpdate = false;   // manifest.version > current version
	int sourceUsed = 0;       // 0 = Gitee, 1 = GitHub
	long long cacheAgeSec = -1; // only with fromCache
	UpdateManifest manifest;
	std::string error;        // set when neither source nor cache worked
};

// Default manifest URLs. The repositories exist; the manifest itself is
// published separately, so until a release ships update.json a check reports
// "cannot reach the update server" instead of pretending to be up to date.
// Both can be overridden in settings.json (update.urlGitee / update.urlGithub).
inline constexpr const char *kDefaultUpdateUrlGitee =
	"https://gitee.com/qiuzongman/AHKDeck/raw/main/update.json";
inline constexpr const char *kDefaultUpdateUrlGithub =
	"https://raw.githubusercontent.com/zongmanqiu/AHKDeck/main/update.json";

// Tries `aSource` first, then the other non-empty URL, then the local cache.
// A successful fetch is written to the cache. Synchronous and blocking -
// call from a worker thread.
UpdateCheckResult CheckUpdate(const std::string &aCurrentVersion,
	const std::string &aUrlGitee, const std::string &aUrlGithub, int aSource);

// "x.y.z" comparison (a leading "v" is tolerated): 1 when `aLatest` is newer
// than `aCurrent`, -1 when older, 0 when equal. Public because the dialog has
// to judge a cached manifest exactly the way CheckUpdate judges a fresh one.
int CompareVersions(const std::string &aCurrent, const std::string &aLatest);

// Reads the cached manifest next to the exe. Returns false when the file is
// absent or unparsable; `aAgeSeconds` is the file's age at call time.
bool ReadCachedManifest(UpdateManifest &aOut, long long &aAgeSeconds);

// Absolute UTF-8 path of the cache (exe dir + update.cache.json). The name is
// deliberately NOT "update.json": the published manifest must never be shipped
// inside the zip, or the copy would overwrite the live cache on every update.
std::string UpdateCachePathUtf8();

// Progress report: (bytesReceived, totalBytes). `total` is 0 when the server
// sent no Content-Length, so callers must treat 0 as "unknown" and show an
// indeterminate indicator. Invoked on the downloading (worker) thread.
using DownloadProgress = std::function<void(long long, long long)>;

// Download `aUrl` to `aSavePathUtf8` (UTF-8, may contain Chinese). Wide file
// APIs throughout, so a Chinese install directory works. Returns true on
// success; a failed/truncated transfer leaves no partial file behind.
bool DownloadFile(const std::string &aUrl, const std::string &aSavePathUtf8,
	DownloadProgress aProgress = nullptr);

// Writes ahkdeck_update.bat. Once the app exits, the bat kills this
// installation's AHKDeck.exe, extracts the zip, robocopies over the install,
// restarts, and deletes itself. Paths may contain Chinese (UTF-8 BOM +
// chcp 65001); lines are CRLF.
bool WriteUpdateBat(const std::string &aExeDirUtf8,
	const std::string &aZipPathUtf8, const std::string &aBatPathUtf8);

// Starts the updater bat with a hidden window. The caller should exit the
// process immediately afterwards so the bat can replace the exe.
void LaunchUpdater(const std::string &aBatPathUtf8);

// Directory of the running executable, UTF-8, with a trailing backslash.
std::string GetExeDirUtf8();

} // namespace ahkdeck
