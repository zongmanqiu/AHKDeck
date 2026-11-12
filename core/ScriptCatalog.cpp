// core/ScriptCatalog.cpp - see core/ScriptCatalog.h.
#include "core/ScriptCatalog.h"

#include "core/Json.h"
#include "core/ScriptHeader.h"
#include "core/TextFile.h"
#include "core/Updater.h"

#include <windows.h>

#include <cstddef>
#include <utility>

namespace ahkdeck
{
namespace
{

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

bool IsHttpsUrl(const std::string &aUrl)
{
	if (aUrl.size() < 12 || aUrl.compare(0, 8, "https://") != 0)
		return false;
	for (char c : aUrl)
	{
		if ((unsigned char)c < 0x20)
			return false;
	}
	return true;
}

void RemoveFile(const std::wstring &aPath)
{
	if (!aPath.empty())
		DeleteFileW(aPath.c_str());
}

bool ReplaceFileWin(const std::wstring &aFrom, const std::wstring &aTo)
{
	return MoveFileExW(aFrom.c_str(), aTo.c_str(),
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

} // namespace

bool IsSafeScriptName(const std::string &aName)
{
	// The name is used verbatim as a filename. Reject separators, wildcards,
	// drive letters, dot segments, trailing dots/spaces and control bytes.
	if (aName.empty() || aName.size() > 100)
		return false;
	if (aName.front() == '.' || aName.back() == '.' || aName.back() == ' ')
		return false;
	if (aName.find("..") != std::string::npos)
		return false;
	static const char kBad[] = "\\/:*?\"<>|";
	for (char c : aName)
	{
		if ((unsigned char)c < 0x20)
			return false;
		for (size_t i = 0; i < sizeof(kBad) - 1; ++i)
		{
			if (c == kBad[i])
				return false;
		}
	}
	return true;
}

CatalogResult ParseScriptCatalog(const std::string &aText)
{
	CatalogResult out;
	std::string text = aText;
	// Json::Parse does not strip a UTF-8 BOM (Updater.cpp has the same step).
	if (text.size() >= 3 && (unsigned char)text[0] == 0xEF
		&& (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
		text.erase(0, 3);

	Json root;
	std::string error;
	if (!Json::Parse(text, root, error))
	{
		out.error = "清单不是合法 JSON：" + error;
		return out;
	}
	if (!root.IsArray())
	{
		out.error = "清单顶层不是数组";
		return out;
	}

	for (size_t i = 0; i < root.Size(); ++i)
	{
		const Json &item = root[i];
		CatalogEntry e;
		e.name = item["name"].AsString();
		e.version = item["version"].AsString();
		e.date = item["date"].AsString();
		e.description = item["description"].AsString();
		e.author = item["author"].AsString();
		e.license = item["license"].AsString();
		e.requires = item["requires"].AsString();
		e.host = item["host"].AsString();
		e.entry = item["entry"].AsString();
		e.path = item["path"].AsString();
		e.sizeKb = item["size_kb"].AsNumber(0.0);
		e.gitee = item["gitee"].AsString();
		e.github = item["github"].AsString();

		if (!IsSafeScriptName(e.name) || e.version.empty())
		{
			++out.skipped;
			continue;
		}
		// Only https URLs are accepted. An unusable mirror column becomes
		// empty (the row greys out for that source) instead of killing the row.
		if (!IsHttpsUrl(e.gitee))
			e.gitee.clear();
		if (!IsHttpsUrl(e.github))
			e.github.clear();
		if (e.gitee.empty() && e.github.empty())
		{
			++out.skipped;
			continue;
		}
		out.entries.push_back(std::move(e));
	}
	return out;
}

std::string LocalScriptVersion(const std::wstring &aPath)
{
	// m151: one parser for the header block, shared with the info dialog. The
	// hand-rolled version this replaced took the text after the LAST blank,
	// which happened to work only because @version never contains a space.
	return ReadScriptVersion(aPath);
}

ScriptInstallState ClassifyInstall(const CatalogEntry &aEntry,
	const std::string &aLocalVersion, bool aLocalExists)
{
	if (!aLocalExists)
		return ScriptInstallState::Absent;
	// A file with no readable header is treated as an older copy: upgrading is
	// still allowed, but the dialog asks for two clicks before overwriting it.
	if (aLocalVersion.empty())
		return ScriptInstallState::Upgradable;
	const int cmp = CompareVersions(aLocalVersion, aEntry.version);
	if (cmp > 0)
		return ScriptInstallState::Upgradable;
	if (cmp == 0)
		return ScriptInstallState::Installed;
	return ScriptInstallState::Ahead;
}

const std::string &DownloadUrlFor(const CatalogEntry &aEntry, CatalogSource aSource)
{
	return aSource == CatalogSource::Github ? aEntry.github : aEntry.gitee;
}

std::string ScriptsDirUtf8()
{
	return GetExeDirUtf8() + "Scripts\\";
}

std::string BundledCatalogPathUtf8()
{
	return GetExeDirUtf8() + "docs\\scripts.json";
}

bool ReadBundledCatalog(std::string &aOut, std::string &aError)
{
	const std::wstring path = Utf8ToWideLocal(BundledCatalogPathUtf8());
	if (path.empty())
	{
		aError = "无法确定程序目录";
		return false;
	}
	TextDocument doc;
	if (!ReadTextDocument(path, doc, aError))
	{
		aError = "随包清单不可用：" + aError;
		return false;
	}
	aOut = doc.utf8;
	return true;
}

bool RefreshBundledCatalog(const std::string &aUrl, std::string &aOut,
	std::string &aError)
{
	std::string text;
	std::string fetchError;
	if (!FetchText(aUrl, 15000, text, fetchError))
	{
		aError = fetchError.empty() ? "无法连接清单服务器" : ("清单获取失败：" + fetchError);
		return false;
	}
	// A 200 that is actually an HTML error page must not replace a good copy.
	const CatalogResult probe = ParseScriptCatalog(text);
	if (!probe.error.empty())
	{
		aError = probe.error;
		return false;
	}
	if (probe.entries.empty())
	{
		aError = "清单中没有可用脚本";
		return false;
	}

	const std::string finalPath = BundledCatalogPathUtf8();
	const std::string partPath = finalPath + ".part";
	const std::wstring wPart = Utf8ToWideLocal(partPath);
	const std::wstring wFinal = Utf8ToWideLocal(finalPath);
	const std::wstring wDocs = Utf8ToWideLocal(GetExeDirUtf8() + "docs");
	if (!wDocs.empty())
		CreateDirectoryW(wDocs.c_str(), nullptr);
	if (!wPart.empty() && !wFinal.empty())
	{
		std::string writeError;
		if (WriteTextDocument(wPart, text, TextEncoding::Utf8NoBom, false,
			nullptr, writeError))
		{
			if (!ReplaceFileWin(wPart, wFinal))
				RemoveFile(wPart);
		}
		else
		{
			RemoveFile(wPart);
		}
	}
	// A read-only install directory is not a reason to discard a good fetch:
	// the caller keeps this text in memory and the old copy stays on disk.
	aOut = text;
	return true;
}

bool VerifyDownloadedScript(const std::wstring &aPartPath,
	const ScriptInstallRequest &aRequest, std::string &aError)
{
	// m160: the whole verification, kept in one testable function so the gate
	// never needs the network. Same parser as the info window (m151) - there
	// must not be a second dialect of the header block.
	ScriptInspection inspection;
	if (!InspectScript(aPartPath, inspection) || !inspection.headerFound)
	{
		aError = "下载内容不是有效的 AHKDeck 脚本";
		return false;
	}
	// User rule (m159): official scripts - the download only has to prove it
	// IS the named script at the advertised version. Strict equality, because
	// the manifest is generated FROM these very header fields (m141): any
	// difference means these bytes are not the advertised script. No hashing,
	// no size check; empty expectations never match (fail closed).
	if (inspection.Get("name") != aRequest.expectedName
		|| inspection.Get("version") != aRequest.expectedVersion)
	{
		aError = "下载内容与清单不符（名称或版本不一致）";
		return false;
	}
	return true;
}

bool InstallScriptFile(const ScriptInstallRequest &aRequest, std::string &aError,
	ScriptInstallProgress aProgress)
{
	const std::string partUtf8 = aRequest.targetPathUtf8 + ".part";
	const std::string oldUtf8 = aRequest.targetPathUtf8 + ".old";
	const std::wstring wPart = Utf8ToWideLocal(partUtf8);
	const std::wstring wTarget = Utf8ToWideLocal(aRequest.targetPathUtf8);
	const std::wstring wOld = Utf8ToWideLocal(oldUtf8);
	if (wPart.empty() || wTarget.empty() || wOld.empty())
	{
		aError = "脚本路径编码错误";
		return false;
	}

	RemoveFile(wPart);
	RemoveFile(wOld);

	// 1. Stage. Never point DownloadFile at the live script: on failure it
	//    deletes the target it was given.
	if (!DownloadFile(aRequest.url, partUtf8, aProgress))
	{
		RemoveFile(wPart);
		aError = "下载失败（网络中断或安装目录不可写）";
		return false;
	}

	// 2. Verify it IS the script the manifest advertised: the same header
	//    parser as the info window, @name and @version must match exactly.
	//    m160 user rule: name and version only - no hashing, no size check
	//    (the old 5% tolerance verified exactly what the rule excludes, while
	//    @name itself went unchecked).
	std::string verifyError;
	if (!VerifyDownloadedScript(wPart, aRequest, verifyError))
	{
		RemoveFile(wPart);
		aError = verifyError;
		return false;
	}

	// 3. Keep the old file as both a backup of local edits and a restore point.
	const bool hadOld = GetFileAttributesW(wTarget.c_str()) != INVALID_FILE_ATTRIBUTES;
	if (hadOld && !ReplaceFileWin(wTarget, wOld))
	{
		RemoveFile(wPart);
		aError = "无法备份原脚本（它可能正在运行）";
		return false;
	}

	// 4. Swap, restoring the old file if the swap itself fails.
	if (!ReplaceFileWin(wPart, wTarget))
	{
		if (hadOld)
			ReplaceFileWin(wOld, wTarget);
		RemoveFile(wPart);
		aError = "无法写入脚本（安装目录不可写）";
		return false;
	}
	if (hadOld)
		RemoveFile(wOld);
	return true;
}

} // namespace ahkdeck
