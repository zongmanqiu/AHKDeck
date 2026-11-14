// m160: catalog parsing and the download verification. cwd-independent
// (writes into %TEMP%), network-free — the point of this gate is the verify
// rule itself: name and version, nothing else. The size nails here are the
// user's rule from m159 turned into code: official scripts are verified by
// name and version only; no hashing, no size check.

#include "core/ScriptCatalog.h"

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

	if (gFailures)
	{
		std::printf("scriptcatalog: %d failure(s)\n", gFailures);
		return 1;
	}
	std::printf("scriptcatalog: all checks passed\n");
	return 0;
}
