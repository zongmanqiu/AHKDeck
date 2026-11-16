// m160: catalog parsing and the download verification. cwd-independent
// (writes into %TEMP%), network-free — the point of this gate is the verify
// rule itself: name and version, nothing else. The size nails here are the
// user's rule from m159 turned into code: official scripts are verified by
// name and version only; no hashing, no size check.

#include "core/ScriptCatalog.h"

#include "core/AppPaths.h"
#include "core/ZipPackage.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

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

// --- test-only stored (uncompressed) zip builder ----------------------------
// Replaces the retired miniz writer: entries are STORE-method records with
// the UTF-8 name flag (bit 11) set, exactly what our 7z-based packer emits
// for non-ASCII names. Building the bytes by hand also lets this test craft
// a traversal entry that the filesystem itself would refuse to create.

struct ZipSpec
{
	std::string name; // zip-style '/' separators
	std::string data;
	bool isDir = false;
};

unsigned long Crc32(const unsigned char *aData, size_t aSize)
{
	unsigned long table[256];
	for (unsigned long i = 0; i < 256; ++i)
	{
		unsigned long c = i;
		for (int k = 0; k < 8; ++k)
			c = (c & 1) ? 0xEDB88320ul ^ (c >> 1) : c >> 1;
		table[i] = c;
	}
	unsigned long crc = 0xFFFFFFFFul;
	for (size_t i = 0; i < aSize; ++i)
		crc = table[(crc ^ aData[i]) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFul;
}

void PutU16(std::string &aOut, unsigned short aV)
{
	aOut.push_back((char)(aV & 0xFF));
	aOut.push_back((char)((aV >> 8) & 0xFF));
}

void PutU32(std::string &aOut, unsigned long aV)
{
	aOut.push_back((char)(aV & 0xFF));
	aOut.push_back((char)((aV >> 8) & 0xFF));
	aOut.push_back((char)((aV >> 16) & 0xFF));
	aOut.push_back((char)((aV >> 24) & 0xFF));
}

bool BuildZip(const std::wstring &aPath, const std::vector<ZipSpec> &aItems)
{
	const unsigned short dosTime = 0x6000; // 12:00:00
	const unsigned short dosDate = (unsigned short)
		(((2026 - 1980) << 9) | (11 << 5) | 16);
	const unsigned short flags = 0x0800; // names are UTF-8

	std::string blob;
	std::string central;
	std::vector<unsigned long> offsets;
	for (const ZipSpec &item : aItems)
	{
		const unsigned long crc = Crc32(
			(const unsigned char *)item.data.data(), item.data.size());
		offsets.push_back((unsigned long)blob.size());
		PutU32(blob, 0x04034b50); // local file header
		PutU16(blob, 20);         // version needed
		PutU16(blob, flags);
		PutU16(blob, 0);          // stored
		PutU16(blob, dosTime);
		PutU16(blob, dosDate);
		PutU32(blob, crc);
		PutU32(blob, (unsigned long)item.data.size());
		PutU32(blob, (unsigned long)item.data.size());
		PutU16(blob, (unsigned short)item.name.size());
		PutU16(blob, 0); // extra len
		blob += item.name;
		blob += item.data;

		PutU32(central, 0x02014b50); // central directory header
		PutU16(central, 20);         // version made by
		PutU16(central, 20);         // version needed
		PutU16(central, flags);
		PutU16(central, 0); // stored
		PutU16(central, dosTime);
		PutU16(central, dosDate);
		PutU32(central, crc);
		PutU32(central, (unsigned long)item.data.size());
		PutU32(central, (unsigned long)item.data.size());
		PutU16(central, (unsigned short)item.name.size());
		PutU16(central, 0); // extra len
		PutU16(central, 0); // comment len
		PutU16(central, 0); // disk start
		PutU16(central, 0); // internal attrs
		PutU32(central, item.isDir ? 0x10ul : 0ul); // external attrs
		PutU32(central, offsets.back());
		central += item.name;
	}

	const unsigned long centralOffset = (unsigned long)blob.size();
	blob += central;
	const unsigned long centralSize =
		(unsigned long)blob.size() - centralOffset;
	PutU32(blob, 0x06054b50); // end of central directory
	PutU16(blob, 0);
	PutU16(blob, 0);
	PutU16(blob, (unsigned short)aItems.size());
	PutU16(blob, (unsigned short)aItems.size());
	PutU32(blob, centralSize);
	PutU32(blob, centralOffset);
	PutU16(blob, 0); // comment len

	FILE *file = nullptr;
	if (_wfopen_s(&file, aPath.c_str(), L"wb") != 0 || !file)
		return false;
	const bool wrote = std::fwrite(blob.data(), 1, blob.size(), file)
		== blob.size();
	std::fclose(file);
	return wrote;
}

std::wstring Utf8ToWideLocal(const std::string &aText)
{
	if (aText.empty())
		return std::wstring();
	const int n = MultiByteToWideChar(CP_UTF8, 0, aText.c_str(),
		(int)aText.size(), nullptr, 0);
	if (n <= 0)
		return std::wstring();
	std::wstring out((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, aText.c_str(), (int)aText.size(),
		out.data(), n);
	return out;
}

std::string WideToUtf8Local(const std::wstring &aText)
{
	if (aText.empty())
		return std::string();
	const int n = WideCharToMultiByte(CP_UTF8, 0, aText.c_str(),
		(int)aText.size(), nullptr, 0, nullptr, nullptr);
	if (n <= 0)
		return std::string();
	std::string out((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, aText.c_str(), (int)aText.size(),
		out.data(), n, nullptr, nullptr);
	return out;
}

std::wstring TempPath(const wchar_t *aName)
{
	wchar_t dir[MAX_PATH] = { 0 };
	const DWORD n = GetTempPathW(MAX_PATH, dir);
	if (n == 0)
		return aName;
	return std::wstring(dir) + aName;
}

void WriteRaw(const std::wstring &aPath, const std::string &aUtf8)
{
	FILE *file = nullptr;
	if (_wfopen_s(&file, aPath.c_str(), L"wb") != 0 || !file)
		return;
	std::fwrite(aUtf8.data(), 1, aUtf8.size(), file);
	std::fclose(file);
}

// A script whose header says `name`/`version`, padded to a wildly different
// size on demand - the size nail in case 5 needs a real file, not a claim.
std::string ScriptText(const std::string &aName, const std::string &aVersion,
	bool aWithVersion = true)
{
	std::string text = "; ==UserScript==\n"
		"; @name         " + aName + "\n";
	if (aWithVersion)
		text += "; @version      " + aVersion + "\n";
	text += "; @description  演示脚本\n"
		"; ==/UserScript==\n"
		"\n"
		"#Requires AutoHotkey v2.0\n";
	return text;
}

bool Verify(const std::wstring &aPath, const std::string &aExpectedName,
	const std::string &aExpectedVersion, std::string &aError)
{
	ahkdeck::ScriptInstallRequest request;
	request.targetPathUtf8 = "x"; // unused here; Verify opens the path itself
	request.url = "https://example.invalid/x";
	request.expectedName = aExpectedName;
	request.expectedVersion = aExpectedVersion;
	return ahkdeck::VerifyDownloadedScript(aPath, request, aError);
}

} // namespace

int main()
{
	// m181: the trust boundary from m146 §7. Official mirrors must never be
	// labelled third-party (that would put a warning on every official row),
	// and Custom must always carry a notice - the UI reads both from here.
	namespace sc = ahkdeck;
	Check(!sc::IsThirdPartyManifest(sc::ManifestSource::OfficialGitee),
		"official Gitee is not third-party");
	Check(!sc::IsThirdPartyManifest(sc::ManifestSource::OfficialGithub),
		"official GitHub is not third-party");
	Check(sc::IsThirdPartyManifest(sc::ManifestSource::Custom),
		"subscribed manifest is third-party");
	Check(sc::TrustNoticeFor(sc::ManifestSource::OfficialGitee)[0] == '\0'
		&& sc::TrustNoticeFor(sc::ManifestSource::OfficialGithub)[0] == '\0',
		"official mirrors carry no trust warning");
	{
		const std::string notice = sc::TrustNoticeFor(sc::ManifestSource::Custom);
		Check(!notice.empty(), "subscribed manifest carries a trust warning");
		Check(notice.find("无完整性校验") != std::string::npos,
			"trust warning states there is no integrity check");
		Check(notice.find("第三方") != std::string::npos,
			"trust warning states the source is third-party");
	}

	// 1. The happy path: exact name and version.
	const std::wstring good = TempPath(L"m160_good.ahk");
	WriteRaw(good, ScriptText("夜灯", "1.0.2"));
	std::string error;
	Check(Verify(good, "夜灯", "1.0.2", error), "name and version match -> verified");

	// 2. A wrong name is denied EVEN when the version matches. This is the
	//    field the old code never checked.
	error.clear();
	Check(!Verify(good, "冒牌灯", "1.0.2", error), "wrong name denied");
	Check(error.find("名称或版本") != std::string::npos, "mismatch names the reason");

	// 3. A wrong version is denied even when the name matches.
	Check(!Verify(good, "夜灯", "9.9.9", error), "wrong version denied");

	// 4. No header at all: not a valid AHKDeck script.
	const std::wstring bare = TempPath(L"m160_bare.ahk");
	WriteRaw(bare, "#Requires AutoHotkey v2.0\nMsgBox \"no header\"\n");
	error.clear();
	Check(!Verify(bare, "裸脚本", "1.0.0", error), "headerless file denied");
	Check(error.find("不是有效") != std::string::npos, "invalid-script reason given");

	// 5. The no-size-check nail (user rule m159): identical advertised name
	//    and version, but the real file is far from any size the manifest
	//    would ever have claimed - it still installs. The old 5% tolerance
	//    rejected exactly this file.
	const std::wstring big = TempPath(L"m160_big.ahk");
	std::string bigText = ScriptText("夜灯", "1.0.2");
	for (int i = 0; i < 4000; ++i)
		bigText += "; padding line to make the bytes disagree with any size\n";
	WriteRaw(big, bigText);
	Check(Verify(big, "夜灯", "1.0.2", error), "size mismatch alone never denies (no size check)");

	// 6. A header without @name can never satisfy a manifest entry: the old
	//    substring check only demanded "@version", so this file used to pass.
	const std::wstring nameless = TempPath(L"m160_nameless.ahk");
	WriteRaw(nameless, "; ==UserScript==\n; @version      1.0.0\n; ==/UserScript==\n");
	Check(!Verify(nameless, "某个脚本", "1.0.0", error), "header without @name denied");

	// 7. Fail closed: an empty expectation NEVER matches.
	Check(!Verify(good, "", "1.0.2", error), "empty expected name never matches");
	Check(!Verify(good, "夜灯", "", error), "empty expected version never matches");

	// 8. A file that is not on disk.
	Check(!Verify(TempPath(L"m160_absent.ahk"), "夜灯", "1.0.2", error), "unreadable file denied");

	// 9. Catalog basics the verifier leans on: a good row parses, an unsafe
	//    name is skipped (never reaches the verifier), a row with no https
	//    mirror is skipped. (JSON backslash needs four C++ backslashes: the
	//    parsed value must be ..\evil, and IsSafeScriptName rejects it.)
	const char *json =
		"["
		"{\"name\":\"夜灯\",\"version\":\"1.0.2\",\"size_kb\":3.3,"
		"\"gitee\":\"https://gitee.example/a.ahk\",\"github\":\"\"},"
		"{\"name\":\"..\\\\evil\",\"version\":\"1.0.0\","
		"\"gitee\":\"https://gitee.example/b.ahk\",\"github\":\"\"},"
		"{\"name\":\"无镜像\",\"version\":\"1.0.0\","
		"\"gitee\":\"http://plain.example/c.ahk\",\"github\":\"\"}"
		"]";
	ahkdeck::CatalogResult parsed = ahkdeck::ParseScriptCatalog(json);
	Check(parsed.error.empty(), "catalog parses");
	Check(parsed.entries.size() == 1, "only the safe row survives");
	Check(parsed.skipped == 2, "unsafe name and mirrorless rows are skipped");
	if (parsed.entries.size() == 1)
	{
		Check(parsed.entries[0].name == "夜灯", "good row keeps its name");
		Check(parsed.entries[0].version == "1.0.2", "good row keeps its version");
	}

	DeleteFileW(good.c_str());
	DeleteFileW(bare.c_str());
	DeleteFileW(big.c_str());
	DeleteFileW(nameless.c_str());

	// m179: multi-source rules. One catalog entry with both mirrors filled, so
	// the three picks are observable.
	{
		ahkdeck::CatalogResult two = ahkdeck::ParseScriptCatalog(
			"[{\"name\":\"双镜\",\"version\":\"1.0.0\","
			"\"gitee\":\"https://gitee.example/g.ahk\","
			"\"github\":\"https://github.example/h.ahk\"}]");
		Check(two.entries.size() == 1, "two-mirror row parses");
		if (two.entries.size() == 1)
		{
			const ahkdeck::CatalogEntry &e = two.entries[0];
			Check(ahkdeck::DownloadUrlFor(e, ahkdeck::ManifestSource::OfficialGitee)
				== "https://gitee.example/g.ahk", "official Gitee takes the gitee field");
			Check(ahkdeck::DownloadUrlFor(e, ahkdeck::ManifestSource::OfficialGithub)
				== "https://github.example/h.ahk", "official GitHub takes the github field");
			// A custom manifest has no mirror of ours to obey: gitee wins when
			// both are filled, github is the fallback.
			Check(ahkdeck::DownloadUrlFor(e, ahkdeck::ManifestSource::Custom)
				== "https://gitee.example/g.ahk", "custom prefers the gitee field");
		}
		ahkdeck::CatalogResult onlyGithub = ahkdeck::ParseScriptCatalog(
			"[{\"name\":\"单镜\",\"version\":\"1.0.0\","
			"\"github\":\"https://github.example/only.ahk\"}]");
		Check(onlyGithub.entries.size() == 1, "github-only row parses");
		if (onlyGithub.entries.size() == 1)
			Check(ahkdeck::DownloadUrlFor(onlyGithub.entries[0],
					ahkdeck::ManifestSource::Custom)
				== "https://github.example/only.ahk", "custom falls back to github");
	}

	// The label a custom source is shown under is derived, never typed.
	Check(ahkdeck::CustomSourceLabel(
			"https://gitee.com/someone/scripts/raw/master/scripts.json")
		== "gitee.com", "label strips the path");
	Check(ahkdeck::CustomSourceLabel("https://www.example.com/x/scripts.json")
		== "example.com", "label strips www.");
	Check(ahkdeck::CustomSourceLabel("https://host.example:8443/x.json")
		== "host.example", "label strips the port");
	Check(ahkdeck::CustomSourceLabel("not-a-url").empty(), "junk URL has no label");

	// One cache file per URL, and the official mirror choice never splits it -
	// both official kinds publish the same catalog.
	{
		const std::string a = ahkdeck::CatalogCachePathUtf8(
			ahkdeck::ManifestSource::Custom, "https://a.example/scripts.json");
		const std::string b = ahkdeck::CatalogCachePathUtf8(
			ahkdeck::ManifestSource::Custom, "https://b.example/scripts.json");
		Check(!a.empty() && a != b, "custom sources get distinct cache files");
		Check(a.size() > 5 && a.compare(a.size() - 5, 5, ".json") == 0,
			"custom cache is a json file");
		Check(a.find("temp") != std::string::npos, "custom cache lives under temp");
		Check(ahkdeck::CatalogCachePathUtf8(ahkdeck::ManifestSource::OfficialGitee, "x")
				== ahkdeck::CatalogCachePathUtf8(
					ahkdeck::ManifestSource::OfficialGithub, "y"),
			"both official mirrors share one cache file");
	}

	// m180: a manifest may name itself. The bare array stays valid (ours is
	// one), and the object form is what lets 订阅管理 label a source with
	// something better than its host - so both shapes are pinned here.
	{
		const std::string named =
			"{ \"name\": \"某人的脚本集\", \"scripts\": ["
			"  { \"name\": \"demo\", \"version\": \"1.0\","
			"    \"gitee\": \"https://g.example/demo.ahk\" } ] }";
		const ahkdeck::CatalogResult r = ahkdeck::ParseScriptCatalog(named);
		Check(r.error.empty() && r.entries.size() == 1, "object form parses");
		Check(r.name == "某人的脚本集", "object form carries its name");
		Check(!r.entries.empty() && r.entries[0].name == "demo",
			"object form keeps its entries");

		const ahkdeck::CatalogResult bareForm = ahkdeck::ParseScriptCatalog(
			"[ { \"name\": \"demo\", \"version\": \"1.0\","
			"    \"gitee\": \"https://g.example/demo.ahk\" } ]");
		Check(bareForm.error.empty() && bareForm.entries.size() == 1
				&& bareForm.name.empty(),
			"array form still parses and has no name");

		const ahkdeck::CatalogResult broken =
			ahkdeck::ParseScriptCatalog("{ \"name\": \"x\" }");
		Check(!broken.error.empty(), "object without a scripts array is rejected");
	}

	// ---- m221: optional id + version-history links ---------------------
	// The host stays compatible with pre-m221 manifests (no such fields) and
	// keeps a row alive when a history link is poisoned: only that link is
	// cleared, the script remains downloadable.
	{
		const ahkdeck::CatalogResult full = ahkdeck::ParseScriptCatalog(
			"[{\"name\":\"历史脚本\",\"version\":\"2.0.0\","
			"\"id\":\"history-script\","
			"\"gitee\":\"https://gitee.example/h.ahk\","
			"\"github\":\"https://github.example/h.ahk\","
			"\"history\":\"https://gitee.example/raw/master/history/history-script.json\","
			"\"historyGithub\":\"https://raw.githubusercontent.com/o/r/master/history/history-script.json\"}]");
		Check(full.error.empty() && full.entries.size() == 1,
			"m221 manifest with history links parses");
		if (full.entries.size() == 1)
		{
			const ahkdeck::CatalogEntry &e = full.entries[0];
			Check(e.id == "history-script", "stable ascii id is kept");
			Check(e.history == "https://gitee.example/raw/master/history/history-script.json",
				"gitee history URL is kept");
			Check(e.historyGithub == "https://raw.githubusercontent.com/o/r/master/history/history-script.json",
				"github history URL is kept");
		}

		const ahkdeck::CatalogResult old = ahkdeck::ParseScriptCatalog(
			"[{\"name\":\"老脚本\",\"version\":\"1.0.0\","
			"\"gitee\":\"https://gitee.example/old.ahk\"}]");
		Check(old.error.empty() && old.entries.size() == 1,
			"pre-m221 manifest without id/history still parses");
		if (old.entries.size() == 1)
		{
			const ahkdeck::CatalogEntry &e = old.entries[0];
			Check(e.id.empty() && e.history.empty() && e.historyGithub.empty(),
				"absent optional fields stay empty");
		}

		const ahkdeck::CatalogResult bad = ahkdeck::ParseScriptCatalog(
			"[{\"name\":\"坏链脚本\",\"version\":\"1.0.0\","
			"\"gitee\":\"https://gitee.example/bad.ahk\","
			"\"history\":\"http://plain.example/history.json\","
			"\"historyGithub\":\"ftp://bad.example/h.json\"}]");
		Check(bad.error.empty() && bad.entries.size() == 1,
			"malformed history links do not invalidate the row");
		if (bad.entries.size() == 1)
		{
			const ahkdeck::CatalogEntry &e = bad.entries[0];
			Check(e.history.empty() && e.historyGithub.empty(),
				"non-HTTPS history links are cleared");
			Check(ahkdeck::DownloadUrlFor(e, ahkdeck::ManifestSource::OfficialGitee)
					== "https://gitee.example/bad.ahk",
				"row with cleared history links is still downloadable");
		}
	}

	// ---- m211: package install ----------------------------------------
	// Network-free: the zip is built locally by the test's own stored-zip
	// builder, then handed straight to InstallPackageFromZip (the same
	// function the network path lands on after its download). Extraction
	// itself goes through the system Shell namespace, so this block also
	// proves Explorer's unzip accepts what our packer produces. Layout:
	// Scripts is a sandbox under %TEMP%, so the live install directory is
	// never touched.
	{
		const std::wstring sandbox = TempPath(L"ahkdeck_pkg_test");
		const std::wstring zipPath = sandbox + L"\\pkg.zip";
		const std::string name = "测试包脚本";
		const std::string version = "1.2.3";

		ahkdeck::RemoveTreeWide(sandbox);
		CreateDirectoryW(sandbox.c_str(), nullptr);

		const std::string entryText = "; ==UserScript==\n"
			"; @name         " + name + "\n"
			"; @version      " + version + "\n"
			"; ==/UserScript==\n"
			"\n"
			"#Requires AutoHotkey v2.0\n";
		const std::string companion = "companion-bytes";

		Check(BuildZip(zipPath, {
				{ name + "/", "", true },
				{ name + "/" + name + ".ahk", entryText, false },
				{ name + "/data/", "", true },
				{ name + "/data/companion.bin", companion, false },
			}), "test zip builder writes the good package");

		ahkdeck::ScriptInstallRequest req;
		req.targetPathUtf8 = WideToUtf8Local(sandbox) + "\\Scripts\\"
			+ name + "\\" + name + ".ahk";
		req.expectedName = name;
		req.expectedVersion = version;

		std::string err;
		Check(ahkdeck::InstallPackageFromZip(zipPath, req, err),
			"package install succeeds");
		Check(err.empty(), "package install has no error");

		const std::wstring entryFile = sandbox + L"\\Scripts\\"
			+ Utf8ToWideLocal(name) + L"\\" + Utf8ToWideLocal(name) + L".ahk";
		const std::wstring companionFile = sandbox + L"\\Scripts\\"
			+ Utf8ToWideLocal(name) + L"\\data\\companion.bin";
		Check(GetFileAttributesW(entryFile.c_str()) != INVALID_FILE_ATTRIBUTES,
			"entry script lands at Scripts\\<名>\\<名>.ahk");
		Check(GetFileAttributesW(companionFile.c_str()) != INVALID_FILE_ATTRIBUTES,
			"companion lands inside the same folder");
		Check(!ahkdeck::LocalScriptVersion(entryFile).empty()
			&& ahkdeck::LocalScriptVersion(entryFile) == version,
			"entry script version reads back");
		const std::wstring staging = sandbox + L"\\Scripts\\"
			+ Utf8ToWideLocal(name) + L".part";
		Check(GetFileAttributesW(staging.c_str()) == INVALID_FILE_ATTRIBUTES,
			"staging dir is cleaned up");

		// A second install moves the old directory into temp\deleted instead
		// of destroying it (the m177 rule, now for packages too).
		const std::string entryText2 = "; ==UserScript==\n"
			"; @name         " + name + "\n"
			"; @version      9.9.9\n"
			"; ==/UserScript==\n"
			"\n"
			"#Requires AutoHotkey v2.0\n";
		Check(BuildZip(zipPath, {
				{ name + "/", "", true },
				{ name + "/" + name + ".ahk", entryText2, false },
			}), "second zip writes the upgrade package");

		ahkdeck::ScriptInstallRequest req2 = req;
		req2.expectedVersion = "9.9.9";
		Check(ahkdeck::InstallPackageFromZip(zipPath, req2, err),
			"package upgrade succeeds");
		Check(ahkdeck::LocalScriptVersion(entryFile) == "9.9.9",
			"upgraded entry version reads back");
		const std::wstring deleted = ahkdeck::AppTempDir(ahkdeck::kTempDeleted);
		bool foundBackup = false;
		WIN32_FIND_DATAW bfd;
		HANDLE bfind = FindFirstFileW((deleted + L"\\"
			+ Utf8ToWideLocal(name) + L"-*").c_str(), &bfd);
		if (bfind != INVALID_HANDLE_VALUE)
		{
			foundBackup = (bfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			FindClose(bfind);
		}
		Check(foundBackup, "old package dir moved to temp\\deleted");

		// Rejection: a traversal entry must never reach the disk. The
		// filesystem itself cannot hold a ".." component, so the builder
		// crafts the name directly - exactly what a hostile archive does.
		const std::string evil = "../evil.txt";
		Check(BuildZip(zipPath, { { evil, "x", false } }),
			"evil zip writes the traversal entry");
		std::string evilErr;
		Check(!ahkdeck::InstallPackageFromZip(zipPath, req, evilErr),
			"traversal package is rejected");
		Check(evilErr.find("..") != std::string::npos,
			"rejection explains the traversal entry");

		// Wrong @name must fail exactly like a single-file download would.
		const std::string wrongName = "; ==UserScript==\n"
			"; @name         别的脚本\n"
			"; @version      " + version + "\n"
			"; ==/UserScript==\n";
		Check(BuildZip(zipPath, {
				{ name + "/", "", true },
				{ name + "/" + name + ".ahk", wrongName, false },
			}), "wrong-name zip writes its entry");
		std::string nameErr;
		Check(!ahkdeck::InstallPackageFromZip(zipPath, req, nameErr),
			"wrong @name package is rejected");

		// m226: the installer staged the download as "<名>.zip.part", but the
		// Shell zip namespace is bound to the .zip extension - a valid archive
		// under any other name fails to open and surfaces as
		// "打不开 zip 包（文件损坏或不是 zip）". Extraction now normalises the
		// name itself, so this pins that no caller has to care.
		Check(BuildZip(zipPath, {
				{ name + "/", "", true },
				{ name + "/" + name + ".ahk", entryText, false },
			}), "part-suffix zip writes the good package");
		const std::wstring partZip = zipPath + L".part";
		Check(CopyFileW(zipPath.c_str(), partZip.c_str(), FALSE) != 0,
			"non-.zip copy of the package is created");
		std::string partErr;
		Check(ahkdeck::InstallPackageFromZip(partZip, req, partErr),
			"package whose path is not .zip still installs");
		Check(ahkdeck::LocalScriptVersion(entryFile) == version,
			"non-.zip install lands the manifest version");
		const std::wstring tempCopy = sandbox + L"\\Scripts\\"
			+ Utf8ToWideLocal(name) + L".part.package.zip";
		Check(GetFileAttributesW(tempCopy.c_str()) == INVALID_FILE_ATTRIBUTES,
			"the temporary .zip copy is cleaned up");
		Check(GetFileAttributesW(partZip.c_str()) != INVALID_FILE_ATTRIBUTES,
			"the caller's own package file is left alone");

		ahkdeck::RemoveTreeWide(sandbox);
	}

	if (gFailures)
	{
		std::printf("scriptcatalog: %d failure(s)\n", gFailures);
		return 1;
	}
	std::printf("scriptcatalog: all checks passed\n");
	return 0;
}
