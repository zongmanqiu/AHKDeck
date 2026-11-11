// core/ScriptCatalog.h - the official script catalog and its safe install (m146).
//
// The manifest (scripts.json) is a top-level array of flat objects shipped by
// the script repository. Everything here is FLTK-free: the dialog only renders
// what this layer returns, and every file/network operation happens on a worker
// thread. That split is what lets the four install states be reasoned about
// without a window.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace ahkdeck
{

// The catalog URL must follow a BRANCH (it has to change when the repository
// publishes new scripts), while each script's own download URL inside the
// catalog is pinned to a tag (m145) so the downloaded bytes stay stable.
// Both are overridable in settings.json under the "scripts" node.
inline constexpr const char *kDefaultCatalogUrlGitee =
	"https://gitee.com/qiuzongman/ahk-v2-scripts/raw/master/scripts.json";
inline constexpr const char *kDefaultCatalogUrlGithub =
	"https://raw.githubusercontent.com/zongmanqiu/ahk-v2-scripts/master/scripts.json";

// One row of scripts.json. `name` doubles as the on-disk file stem, which is
// why ParseScriptCatalog refuses unsafe names instead of merely trimming them.
struct CatalogEntry
{
	std::string name;
	std::string version;
	std::string date;
	std::string description;
	std::string author;
	std::string license;
	std::string requires;
	std::string host;
	std::string entry;
	std::string path;
	double sizeKb = 0.0;
	std::string gitee;
	std::string github;
};

struct CatalogResult
{
	std::vector<CatalogEntry> entries;
	int skipped = 0;    // malformed or unsafe items (logged, never shown as rows)
	std::string error;  // empty on success
};

enum class CatalogSource
{
	Gitee = 0,
	Github = 1
};

// rupix's four states, minus the ABI check (a script is a single text file):
// Absent -> 下载, Upgradable -> 升级, Installed/Ahead -> 已下载.
enum class ScriptInstallState
{
	Absent,
	Upgradable,
	Installed,
	Ahead
};

using ScriptInstallProgress = std::function<void(long long, long long)>;

struct ScriptInstallRequest
{
	std::string targetPathUtf8; // <exe>\Scripts\<name>.ahk
	std::string url;
	double expectedSizeKb = 0.0;
};

// A UTF-8 BOM is tolerated. Items with an unsafe name, no version, or no
// https URL are skipped and counted rather than failing the whole list.
CatalogResult ParseScriptCatalog(const std::string &aText);

bool IsSafeScriptName(const std::string &aName);

// `; @version      1.0.0` from the leading ==UserScript== block. Empty when
// the file is absent, unreadable, or has no such line.
std::string LocalScriptVersion(const std::wstring &aPath);

ScriptInstallState ClassifyInstall(const CatalogEntry &aEntry,
	const std::string &aLocalVersion, bool aLocalExists);

const std::string &DownloadUrlFor(const CatalogEntry &aEntry, CatalogSource aSource);

// Both are derived from GetExeDirUtf8(), which already ends in a backslash.
std::string ScriptsDirUtf8();
std::string BundledCatalogPathUtf8();

// Local-first: opening the dialog reads the bundled copy, never the network.
bool ReadBundledCatalog(std::string &aOut, std::string &aError);

// 刷新: fetch, verify it parses, then atomically overwrite the bundled copy.
// A failed write keeps the old copy; a failed fetch keeps the old catalog.
bool RefreshBundledCatalog(const std::string &aUrl, std::string &aOut,
	std::string &aError);

// The safe install. DownloadFile() writes CREATE_ALWAYS and deletes its target
// on failure, so it must NEVER be pointed at the live script. This stages into
// <name>.ahk.part, verifies, renames the old file to .old, swaps, and restores
// on any error. The user's script is never deleted.
bool InstallScriptFile(const ScriptInstallRequest &aRequest, std::string &aError,
	ScriptInstallProgress aProgress = nullptr);

} // namespace ahkdeck
