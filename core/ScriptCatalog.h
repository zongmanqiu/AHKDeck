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
//
// Gitee goes through the raw CDN host on purpose: gitee.com/<owner>/<repo>/raw/...
// answers with a 302 to raw.giteeusercontent.com/...?signature=..., a signed URL
// that expires, so a cached or shared copy of it stops working. The direct CDN
// form has no redirect and no signature.
inline constexpr const char *kDefaultCatalogUrlGitee =
	"https://raw.giteeusercontent.com/qiuzongman/ahk-v2-scripts/raw/master/scripts.json";
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
	// m221: stable ASCII id (release tag key) + optional version-history
	// manifest URLs (two mirrors). All optional so pre-m221 manifests still
	// parse; empty means "this script has no published history link".
	std::string id;
	std::string history;
	std::string historyGithub;
	// m211: package mode. Empty for the classic single .ahk file; non-empty
	// means the script ships as a zip (root folder `<名>/`, entry
	// `<名>/<名>.ahk`) that must be extracted, not just overwritten. Two
	// mirrors like the text URLs; `packageGithub` may stay empty.
	std::string package;
	std::string packageGithub;
};

struct CatalogResult
{
	std::vector<CatalogEntry> entries;
	// m180: the manifest's own name, when it ships one (a top-level object
	// with "name"). Ours is a bare array and stays nameless; a subscriber's
	// manifest can say who it is, which is the only honest way for the 订阅
	// list to label it - a host name is a fallback, not a title.
	std::string name;
	int skipped = 0;    // malformed or unsafe items (logged, never shown as rows)
	std::string error;  // empty on success
};

// One subscribed manifest (m180). `name` is cached from the last successful
// fetch so 订阅管理 can list a source it cannot reach right now; empty means
// "fall back to the host" - see CustomSourceLabel().
struct CustomCatalogSource
{
	std::string url;
	std::string name;
};

// m179: which manifest a row came from, and - for the two official mirrors -
// which per-entry URL field to download from. One value travels with each row
// so a merged list never has to ask "where did this come from" a second time.
//
// A custom manifest is the same format as ours (name/version/gitee/github), so
// it needs no new parser; it only differs in that the user's mirror choice in
// 选项 has no meaning for it - see DownloadUrlFor().
enum class ManifestSource
{
	OfficialGitee = 0,
	OfficialGithub = 1,
	Custom = 2
};

// m181: m146 §7 ruled that third-party manifests need a trust marker, because
// the reason official scripts need no hashing ("every artifact comes out of
// our own build", m160) does not hold for a stranger's catalog: a subscribed
// manifest declares its own @name/@version and supplies its own bytes, so the
// name+version check proves identity, NOT integrity or authorship.
//
// We deliberately do NOT add a mandatory hash/signature field: that would
// break the "any same-shape json works" contract the subscription UI is built
// on. Instead the boundary is made explicit to the user before the download
// (two-click confirm), in the info window, and in the subscription window.
// Both the UI and the unit gate go through these two functions so the rule
// cannot silently drift apart.
bool IsThirdPartyManifest(ManifestSource aSource);

// Empty for the two official mirrors; otherwise the warning to show verbatim.
const char *TrustNoticeFor(ManifestSource aSource);

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
	std::string targetPathUtf8;  // <exe>\Scripts\<name>.ahk
	std::string url;
	// m160: the manifest entry the download must prove to be. The user's rule:
	// official scripts, so only name and version are verified - no hashing and
	// no size check (the old expectedSizeKb verified exactly what this rule
	// excludes, while @name itself went unchecked).
	std::string expectedName;
	std::string expectedVersion;
	// m211: non-empty switches InstallScriptFile into package mode - download
	// the zip, validate, extract to a staging dir, verify the entry script,
	// then swap the whole `<名>/` directory. The text `url` is unused then.
	std::string packageUrl;
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

// Official rows follow the mirror the user picked in 选项; a custom manifest
// has no mirror of its own, so it takes whichever field it actually publishes
// (gitee first - the audience is Chinese, and our own format always fills it).
const std::string &DownloadUrlFor(const CatalogEntry &aEntry,
	ManifestSource aSource);

// m211: the package zip URL for the picked mirror, or empty when the entry is
// a single-file script (or the mirror carries no package).
const std::string &PackageUrlFor(const CatalogEntry &aEntry,
	ManifestSource aSource);

// Both are derived from GetExeDirUtf8(), which already ends in a backslash.
std::string ScriptsDirUtf8();
std::string BundledCatalogPathUtf8();

// Where the last fetched copy of a manifest is cached. Official mirrors share
// ONE file - they publish the same catalog - while every custom URL gets its
// own, so switching or adding a source can never show another source's rows.
// Custom caches are pure scratch and therefore live under temp\work\ (m177).
std::string CatalogCachePathUtf8(ManifestSource aSource, const std::string &aUrl);

// The short name a custom source is shown under (the URL's host, without
// "www."). Not a user-supplied name: m179 settled on "one URL box, no naming
// step", so the label has to be derivable. Returns an empty string for an
// unusable URL.
std::string CustomSourceLabel(const std::string &aUrl);

// Local-first: opening the dialog reads the cached copy, never the network.
bool ReadCatalogCache(ManifestSource aSource, const std::string &aUrl,
	std::string &aOut, std::string &aError);

// 刷新: fetch, verify it parses, then atomically overwrite the cache. A failed
// write keeps the old copy; a failed fetch keeps the old catalog.
bool RefreshCatalogCache(ManifestSource aSource, const std::string &aUrl,
	std::string &aOut, std::string &aError);

// The safe install. DownloadFile() writes CREATE_ALWAYS and deletes its target
// on failure, so it must NEVER be pointed at the live script. This stages into
// <name>.ahk.part, verifies, renames the old file to .old, swaps, and restores
// on any error. The user's script is never deleted.
bool InstallScriptFile(const ScriptInstallRequest &aRequest, std::string &aError,
	ScriptInstallProgress aProgress = nullptr);

// m227: ONE layout. A script is `Scripts\<名>.ahk` and nothing else; the folder
// `Scripts\<名>\` next to it belongs to that script (attachments and runtime
// output), which is why the list must never look inside folders for scripts.
// 0.4.x installed packages as `Scripts\<名>\<名>.ahk`; this moves that entry file
// out to the flat name on the first scan and leaves the folder alone (dropped
// only when empty). Idempotent, one directory enumeration, never overwrites a
// flat script that already owns the name. Returns how many were moved.
int MigrateLegacyScriptFolders(const std::wstring &aScriptsDir);

// m211: the package install once the zip is already on disk (staged by
// InstallScriptFile, or handed over directly by a test). aRequest.targetPathUtf8
// names the entry as `Scripts\<名>.ahk` (m227: flat, like every script); the zip
// must carry `<名>/<名>.ahk` plus any companions, the entry header is verified
// against the request, then the entry lands flat and the remaining files land in
// `Scripts\<名>\`. A pre-existing flat script and a pre-existing folder both move
// to temp\deleted (never destroyed) and are restored if the swap fails.
bool InstallPackageFromZip(const std::wstring &aZipPath,
	const ScriptInstallRequest &aRequest, std::string &aError);

// m160: the download verification as one standalone function (the .part file
// is already on disk). Uses the SAME header parser as the info window, then
// requires @name and @version to match the manifest entry exactly. No content
// hashing and no size check - the user's rule for official scripts is name and
// version only. Empty expectedName/expectedVersion never match (fail closed).
bool VerifyDownloadedScript(const std::wstring &aPartPath,
	const ScriptInstallRequest &aRequest, std::string &aError);

} // namespace ahkdeck
