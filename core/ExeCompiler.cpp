// core/ExeCompiler.cpp - m172, rewritten in m188.
//
// m172 shelled out to the official Ahk2Exe.exe. That exe is 995 KB because it
// is itself a compiled AutoHotkey **v1.1** script: it carries a whole previous-
// generation runtime whose only job is to run a converter that ends up doing
// one Win32 call. m188 measured the actual job:
//
//     copy the base exe, then UpdateResource(RT_RCDATA, id 1, script text)
//
// The result runs, is the same size as Ahk2Exe's output (512 bytes of resource
// directory apart), and needs neither Ahk2Exe nor a v1.1 runtime. m192 P4:
// the base IS the one shipped kernel (exeDir\AutoHotkey64.exe) - the same PE
// the editor spawns per session and the same one every compiled script gets.
// tools\ is retired; this file does the bundling in-process.
//
// What the official converter did BEYOND bundling, and what happens here:
//   * syntax/#Include validation -> done by spawning AHKDeck.exe with
//     --validate, which in turn starts its own kernel child (m192 P4). A
//     compile often happens while scripts are running, so the check must not
//     share any live interpreter.
//   * #@Ahk2Exe-* directives (icon, version block, console subsystem, MPRESS)
//     -> NOT supported. None of the bundled scripts use them, and the deck's
//     compile command never passed /icon or /compress either. A script that
//     carries such directives compiles to a plain exe; the lines are comments
//     to the runtime, so nothing breaks.
//   * #Include inlining -> not done; the runtime resolves #Include itself,
//     relative to the exe. For a single-file script (all six bundled ones) this
//     is identical. A script that #Includes a sibling becomes a multi-file
//     product, which is exactly what Ahk2Exe's inlining was hiding.

#include "core/ExeCompiler.h"

#include "core/AppPaths.h"

#include "core/TextFile.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ahkdeck
{
namespace
{

// m192 P4: one kernel, one place. ResolveCompileTools used to look under
// tools\ for a second copy of the interpreter (the m172/m190 compile base);
// the out-of-process engine already resolves <exeDir>\AutoHotkey64.exe, and
// bundling a script into that very image is what makes "one PE" true.
constexpr const wchar_t *kBaseLeaf = L"AutoHotkey64.exe";

// A compiled AutoHotkey v2 image finds its script in RCDATA id 1, language
// 0x409. m188 probe: the numeric id runs, the string name L"1" does not (that
// build showed a dialog instead of executing). Do not "tidy" this into a name.
constexpr WORD kRcData = 10;
constexpr WORD kScriptResourceId = 1;
constexpr WORD kResourceLanguage = 0x409;

bool FileExists(const std::wstring &aPath)
{
	const DWORD attr = GetFileAttributesW(aPath.c_str());
	return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring TrimTrailingSlash(std::wstring aPath)
{
	while (!aPath.empty() && (aPath.back() == L'\\' || aPath.back() == L'/'))
		aPath.pop_back();
	return aPath;
}

bool ReadRawFile(const std::wstring &aPath, std::string &aOut)
{
	aOut.clear();
	const HANDLE file = CreateFileW(aPath.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	char chunk[4096];
	DWORD got = 0;
	while (ReadFile(file, chunk, sizeof(chunk), &got, nullptr) && got > 0)
		aOut.append(chunk, got);
	CloseHandle(file);
	return true;
}

// The helper's last line is {"ok":...,"rc":N}; earlier lines are one JSON
// record per diagnostic. Fall back to the child's exit code when the tail is
// not a verdict (a crash mid-write, for instance).
int RcFromReport(const std::string &aReport, int aFallback)
{
	int rc = aFallback;
	size_t start = 0;
	while (start <= aReport.size())
	{
		size_t end = aReport.find('\n', start);
		if (end == std::string::npos)
			end = aReport.size();
		const std::string line = aReport.substr(start, end - start);
		if (line.find("\"ok\"") != std::string::npos &&
			line.find("\"rc\"") != std::string::npos)
		{
			const size_t at = line.find("\"rc\":");
			if (at != std::string::npos)
				rc = std::atoi(line.c_str() + at + 5);
		}
		if (end == aReport.size())
			break;
		start = end + 1;
	}
	return rc;
}

// One diagnostic record is {"line":12,"severity":"error","phase":"load",
// "message":"...","extra":"..."}. The console shows the last few lines of
// `output`, so handing it raw JSON would tell the user nothing; pull out the
// part a person can act on.
std::string ReadableDiagnostics(const std::string &aReport)
{
	std::string out;
	size_t start = 0;
	while (start <= aReport.size())
	{
		size_t end = aReport.find('\n', start);
		if (end == std::string::npos)
			end = aReport.size();
		const std::string line = aReport.substr(start, end - start);
		if (line.find("\"message\"") != std::string::npos)
		{
			const size_t msgAt = line.find("\"message\":\"");
			const size_t lineAt = line.find("\"line\":");
			std::string message;
			if (msgAt != std::string::npos)
			{
				size_t from = msgAt + 11;
				std::string piece;
				while (from < line.size() && line[from] != '"')
				{
					if (line[from] == '\\' && from + 1 < line.size())
					{
						const char esc = line[from + 1];
						piece += esc == 'n' ? ' ' : esc;
						from += 2;
						continue;
					}
					piece += line[from++];
				}
				message = piece;
			}
			if (!message.empty())
			{
				if (lineAt != std::string::npos)
				{
					const int number = std::atoi(line.c_str() + lineAt + 7);
					if (number > 0)
						out += "第 " + std::to_string(number) + " 行：";
				}
				out += message;
				out += "\n";
			}
		}
		if (end == aReport.size())
			break;
		start = end + 1;
	}
	return out;
}

// Runs "<exeDir>\AHKDeck.exe --validate <script>" as a short-lived child and
// returns the kernel's verdict (0 = the script loads). Output goes through a
// temp file, not a pipe: same deadlock argument as the rest of the deck.
int ValidateInChild(const std::wstring &aExeDir, const std::wstring &aScriptPath,
	const std::wstring &aTempDir, const std::wstring &aTag, int aTimeoutMs,
	std::string &aReport, std::string &aError)
{
	const std::wstring self = aExeDir + L"\\AHKDeck.exe";
	if (!FileExists(self))
	{
		aError = "找不到 AHKDeck.exe，无法校验脚本";
		return -1;
	}

	const std::wstring log = aTempDir + L"\\compile-" + aTag + L".validate";
	SECURITY_ATTRIBUTES inheritable = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
	const HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (out == INVALID_HANDLE_VALUE)
	{
		aError = "无法创建校验输出捕获文件";
		return -1;
	}

	std::wstring command = L"\"" + self + L"\" --validate \"" + aScriptPath + L"\"";
	std::vector<wchar_t> line(command.begin(), command.end());
	line.push_back(0);

	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	si.hStdInput = out;
	si.hStdOutput = out;
	si.hStdError = out;

	PROCESS_INFORMATION pi = {};
	const BOOL started = CreateProcessW(self.c_str(), line.data(), nullptr, nullptr,
		TRUE, CREATE_NO_WINDOW, nullptr, aTempDir.c_str(), &si, &pi);
	CloseHandle(out);
	if (!started)
	{
		aError = "无法启动校验进程（错误码 " + std::to_string(GetLastError()) + "）";
		DeleteFileW(log.c_str());
		return -1;
	}
	CloseHandle(pi.hThread);

	const DWORD waited = WaitForSingleObject(pi.hProcess,
		aTimeoutMs > 0 ? static_cast<DWORD>(aTimeoutMs) : INFINITE);
	int rc = -1;
	if (waited == WAIT_TIMEOUT)
	{
		TerminateProcess(pi.hProcess, 1);
		WaitForSingleObject(pi.hProcess, 5000);
		aError = "校验超时未退出，已结束";
	}
	else
	{
		DWORD code = 1;
		if (GetExitCodeProcess(pi.hProcess, &code))
			rc = static_cast<int>(code);
	}
	CloseHandle(pi.hProcess);

	ReadRawFile(log, aReport);
	DeleteFileW(log.c_str());
	if (rc >= 0)
		rc = RcFromReport(aReport, rc);
	return rc;
}

// ---------------------------------------------------------------------------
// m189: artwork. The base image carries AutoHotkey's own icon and version
// block; someone who compiles "我的脚本.ahk" wants their script's face on it.
// Both are plain resource writes on the update handle that already holds the
// script, so nothing here needs a second tool or a second pass over the file.
//
// The icon routine follows the official converter's proven order (its
// IconChanger.ahk): read the group the base ships, delete exactly the images
// that group references, then add the new images under those same ids and
// rewrite the group. Reusing the ids keeps any other icon group in the base
// pointing at the images it expects.
// ---------------------------------------------------------------------------

constexpr WORD kRtIcon = 3;
constexpr WORD kRtGroupIcon = 14;
constexpr WORD kRtVersion = 16;

// The language every resource in a deck product is written in, and the code
// page pair that names it inside a version block (0x0409 en-US, 1200 = UTF-16).
constexpr WORD kVersionLang = 0x0409;
constexpr const wchar_t *kVersionLangKey = L"040904B0";

std::string Narrow(const std::wstring &aText)
{
	if (aText.empty())
		return std::string();
	const int needed = WideCharToMultiByte(CP_UTF8, 0, aText.c_str(),
		static_cast<int>(aText.size()), nullptr, 0, nullptr, nullptr);
	if (needed <= 0)
		return std::string();
	std::string out(static_cast<size_t>(needed), '\0');
	WideCharToMultiByte(CP_UTF8, 0, aText.c_str(), static_cast<int>(aText.size()),
		out.data(), needed, nullptr, nullptr);
	return out;
}

struct ResourceRef
{
	WORD id;
	WORD lang;
};

// EnumResourceNames/EnumResourceLanguages need plain C callbacks, so what is
// being collected - and the name currently being walked - live here. The walk
// is synchronous on one worker thread, so a file static is honest.
std::vector<ResourceRef> &CollectedRefs()
{
	static std::vector<ResourceRef> refs;
	return refs;
}

WORD gWalkingName = 0;

BOOL CALLBACK CollectLanguageCb(HMODULE, LPCWSTR, LPCWSTR, WORD aLang, LONG_PTR)
{
	CollectedRefs().push_back({ gWalkingName, aLang });
	return TRUE;
}

BOOL CALLBACK CollectNameCb(HMODULE aModule, LPCWSTR aType, LPWSTR aName, LONG_PTR)
{
	const ULONG_PTR raw = reinterpret_cast<ULONG_PTR>(aName);
	if (raw > 0xFFFF)
		return TRUE; // a named resource; icons and versions in an exe are ids
	gWalkingName = static_cast<WORD>(raw);
	EnumResourceLanguagesW(aModule, aType, aName, CollectLanguageCb, 0);
	return TRUE;
}

// (id, lang) of every entry of one type, read as data - the file is never
// mapped as an image, so nothing here depends on the base's bitness.
std::vector<ResourceRef> ListResources(const std::wstring &aPath, LPCWSTR aType)
{
	CollectedRefs().clear();
	HMODULE module = LoadLibraryExW(aPath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
	if (!module)
		return std::vector<ResourceRef>();
	EnumResourceNamesW(module, aType, CollectNameCb, 0);
	FreeLibrary(module);
	return CollectedRefs();
}

// The group the base ships (id 1 in practice) and the image ids it references.
// A group that cannot be parsed yields no ids, which makes the caller number
// the new images from 1 instead.
bool ReadIconGroup(const std::wstring &aPath, WORD &aGroupId, std::vector<WORD> &aImageIds)
{
	aGroupId = 1;
	aImageIds.clear();
	const std::vector<ResourceRef> groups = ListResources(aPath, MAKEINTRESOURCEW(kRtGroupIcon));
	if (groups.empty())
		return false;
	aGroupId = groups.front().id;
	for (const ResourceRef &ref : groups)
		if (ref.id == 1)
		{
			aGroupId = 1;
			break;
		}

	HMODULE module = LoadLibraryExW(aPath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
	if (!module)
		return false;
	bool ok = false;
	HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(aGroupId), MAKEINTRESOURCEW(kRtGroupIcon));
	if (found)
	{
		const DWORD size = SizeofResource(module, found);
		const void *data = LockResource(LoadResource(module, found));
		if (data && size >= 6)
		{
			const BYTE *bytes = static_cast<const BYTE *>(data);
			WORD count = 0;
			memcpy(&count, bytes + 4, sizeof(count));
			if (6 + static_cast<size_t>(count) * 14 <= size)
			{
				for (WORD i = 0; i < count; ++i)
				{
					WORD imageId = 0;
					memcpy(&imageId, bytes + 6 + static_cast<size_t>(i) * 14 + 12,
						sizeof(imageId));
					aImageIds.push_back(imageId);
				}
				ok = true;
			}
		}
	}
	FreeLibrary(module);
	return ok;
}

// Replaces the base's icon with aIcoPath's. Fails closed: a corrupt .ico is
// reported by name rather than silently leaving AutoHotkey's icon in place.
bool ApplyIcon(HANDLE aUpdate, const std::wstring &aModulePath, const std::wstring &aIcoPath,
	std::string &aError)
{
	std::string ico;
	if (!ReadRawFile(aIcoPath, ico))
	{
		aError = "读取图标文件失败：" + Narrow(aIcoPath);
		return false;
	}
	const BYTE *bytes = reinterpret_cast<const BYTE *>(ico.data());
	const size_t size = ico.size();
	WORD reserved = 0;
	WORD type = 0;
	WORD count = 0;
	if (size < 6)
	{
		aError = "图标文件不是有效的 .ico（头部过短）：" + Narrow(aIcoPath);
		return false;
	}
	memcpy(&reserved, bytes + 0, sizeof(reserved));
	memcpy(&type, bytes + 2, sizeof(type));
	memcpy(&count, bytes + 4, sizeof(count));
	if (reserved != 0 || type != 1 || count == 0
		|| 6 + static_cast<size_t>(count) * 16 > size)
	{
		aError = "图标文件不是有效的 .ico（目录不完整）：" + Narrow(aIcoPath);
		return false;
	}

	struct IconFrame
	{
		BYTE width;
		BYTE height;
		BYTE colors;
		WORD planes;
		WORD bitCount;
		DWORD bytes;
		DWORD offset;
	};
	std::vector<IconFrame> frames;
	frames.reserve(count);
	for (WORD i = 0; i < count; ++i)
	{
		const BYTE *entry = bytes + 6 + static_cast<size_t>(i) * 16;
		IconFrame frame{};
		frame.width = entry[0];
		frame.height = entry[1];
		frame.colors = entry[2];
		memcpy(&frame.planes, entry + 4, sizeof(frame.planes));
		memcpy(&frame.bitCount, entry + 6, sizeof(frame.bitCount));
		memcpy(&frame.bytes, entry + 8, sizeof(frame.bytes));
		memcpy(&frame.offset, entry + 12, sizeof(frame.offset));
		if (frame.bytes == 0
			|| static_cast<size_t>(frame.offset) + frame.bytes > size)
		{
			aError = "图标文件有一帧越界，文件可能已损坏：" + Narrow(aIcoPath);
			return false;
		}
		frames.push_back(frame);
	}

	WORD groupId = 1;
	std::vector<WORD> oldIds;
	ReadIconGroup(aModulePath, groupId, oldIds);

	// Remove exactly the images the old group referenced; their ids are reused
	// below so no other group is left dangling.
	for (const WORD id : oldIds)
	{
		if (!UpdateResourceW(aUpdate, MAKEINTRESOURCEW(kRtIcon), MAKEINTRESOURCEW(id),
				kResourceLanguage, nullptr, 0))
		{
			aError = "无法移除旧图标图像（错误码 "
				+ std::to_string(GetLastError()) + "）";
			return false;
		}
	}

	std::vector<BYTE> group(6 + static_cast<size_t>(count) * 14, 0);
	const WORD iconType = 1;
	memcpy(group.data() + 2, &iconType, sizeof(iconType));
	memcpy(group.data() + 4, &count, sizeof(count));

	WORD nextId = 0;
	for (const WORD id : oldIds)
		if (id > nextId)
			nextId = id;

	for (WORD i = 0; i < count; ++i)
	{
		const WORD imageId = (i < oldIds.size()) ? oldIds[i] : ++nextId;
		if (!UpdateResourceW(aUpdate, MAKEINTRESOURCEW(kRtIcon), MAKEINTRESOURCEW(imageId),
				kResourceLanguage, const_cast<BYTE *>(bytes + frames[i].offset),
				frames[i].bytes))
		{
			aError = "写入图标图像失败（错误码 "
				+ std::to_string(GetLastError()) + "）：" + Narrow(aIcoPath);
			return false;
		}
		BYTE *entry = group.data() + 6 + static_cast<size_t>(i) * 14;
		entry[0] = frames[i].width;
		entry[1] = frames[i].height;
		entry[2] = frames[i].colors;
		entry[3] = 0;
		memcpy(entry + 4, &frames[i].planes, sizeof(frames[i].planes));
		memcpy(entry + 6, &frames[i].bitCount, sizeof(frames[i].bitCount));
		memcpy(entry + 8, &frames[i].bytes, sizeof(frames[i].bytes));
		memcpy(entry + 12, &imageId, sizeof(imageId));
	}

	if (!UpdateResourceW(aUpdate, MAKEINTRESOURCEW(kRtGroupIcon), MAKEINTRESOURCEW(groupId),
			kResourceLanguage, group.data(), static_cast<DWORD>(group.size())))
	{
		aError = "写入图标组失败（错误码 " + std::to_string(GetLastError()) + "）";
		return false;
	}
	return true;
}

// The values the version block is built from.
struct VersionFields
{
	std::wstring fileVersion;
	std::wstring productName;
	std::wstring fileDescription;
	std::wstring companyName;
	std::wstring legalCopyright;
	std::wstring originalFilename;
};

struct FixedFileInfo
{
	DWORD signature;
	DWORD structVersion;
	DWORD fileVersionMs;
	DWORD fileVersionLs;
	DWORD productVersionMs;
	DWORD productVersionLs;
	DWORD fileFlagsMask;
	DWORD fileFlags;
	DWORD fileOs;
	DWORD fileType;
	DWORD fileSubtype;
	DWORD fileDateMs;
	DWORD fileDateLs;
};
static_assert(sizeof(FixedFileInfo) == 52, "VS_FIXEDFILEINFO is 13 DWORDs");

void AlignBlock(std::vector<BYTE> &aOut)
{
	while (aOut.size() % 4)
		aOut.push_back(0);
}

void AppendWideZ(std::vector<BYTE> &aOut, const std::wstring &aText)
{
	const size_t at = aOut.size();
	aOut.resize(at + (aText.size() + 1) * sizeof(wchar_t));
	memcpy(aOut.data() + at, aText.c_str(), (aText.size() + 1) * sizeof(wchar_t));
}

void PatchWord(std::vector<BYTE> &aOut, size_t aAt, WORD aValue)
{
	memcpy(aOut.data() + aAt, &aValue, sizeof(aValue));
}

// Every VS_VERSIONINFO node is the same five fields, then the key, then the
// value, then children - each padded to a DWORD boundary, with wLength covering
// the whole node. Opening and closing separately keeps the lengths honest even
// when a value is empty.
size_t OpenBlock(std::vector<BYTE> &aOut, WORD aValueLength, WORD aType,
	const std::wstring &aKey)
{
	const size_t start = aOut.size();
	aOut.resize(start + 6);
	PatchWord(aOut, start + 2, aValueLength);
	PatchWord(aOut, start + 4, aType);
	AppendWideZ(aOut, aKey);
	AlignBlock(aOut);
	return start;
}

void CloseBlock(std::vector<BYTE> &aOut, size_t aStart)
{
	PatchWord(aOut, aStart, static_cast<WORD>(aOut.size() - aStart));
}

void AppendStringNode(std::vector<BYTE> &aOut, const wchar_t *aKey, const std::wstring &aValue)
{
	const size_t start = OpenBlock(aOut, static_cast<WORD>((aValue.size() + 1) * 2), 1, aKey);
	AppendWideZ(aOut, aValue);
	AlignBlock(aOut);
	CloseBlock(aOut, start);
}

// The n-th numeric group of a version string: 1 for a missing first group, 0
// for the rest, so "1", "1.2" and "" all produce a legal four-part version.
DWORD VersionPart(const std::wstring &aText, int aIndex)
{
	int seen = -1;
	unsigned long value = 0;
	bool inNumber = false;
	for (const wchar_t c : aText)
	{
		if (c >= L'0' && c <= L'9')
		{
			if (!inNumber)
			{
				++seen;
				inNumber = true;
			}
			if (seen == aIndex)
			{
				value = value * 10 + static_cast<unsigned long>(c - L'0');
				if (value > 0xFFFF)
					value = 0xFFFF;
			}
		}
		else
			inNumber = false;
	}
	if (seen < aIndex)
		return aIndex == 0 ? 1 : 0;
	return static_cast<DWORD>(value);
}

std::vector<BYTE> BuildVersionBlob(const VersionFields &aFields)
{
	const DWORD ms = (VersionPart(aFields.fileVersion, 0) << 16)
		| VersionPart(aFields.fileVersion, 1);
	const DWORD ls = (VersionPart(aFields.fileVersion, 2) << 16)
		| VersionPart(aFields.fileVersion, 3);

	std::vector<BYTE> out;
	const size_t root = OpenBlock(out, sizeof(FixedFileInfo), 0, L"VS_VERSION_INFO");

	FixedFileInfo fixed{};
	fixed.signature = 0xFEEF04BD;
	fixed.structVersion = 0x00010000;
	fixed.fileVersionMs = ms;
	fixed.fileVersionLs = ls;
	fixed.productVersionMs = ms;
	fixed.productVersionLs = ls;
	fixed.fileFlagsMask = 0x3F;
	fixed.fileOs = 0x00040004; // VOS_NT_WINDOWS32
	fixed.fileType = 1;        // VFT_APP
	const size_t fixedAt = out.size();
	out.resize(fixedAt + sizeof(fixed));
	memcpy(out.data() + fixedAt, &fixed, sizeof(fixed));
	AlignBlock(out);

	std::vector<std::pair<std::wstring, std::wstring>> strings;
	strings.push_back({ L"FileDescription", aFields.fileDescription });
	strings.push_back({ L"FileVersion", aFields.fileVersion });
	strings.push_back({ L"ProductName", aFields.productName });
	strings.push_back({ L"ProductVersion", aFields.fileVersion });
	strings.push_back({ L"OriginalFilename", aFields.originalFilename });
	if (!aFields.companyName.empty())
		strings.push_back({ L"CompanyName", aFields.companyName });
	if (!aFields.legalCopyright.empty())
		strings.push_back({ L"LegalCopyright", aFields.legalCopyright });

	const size_t fileInfo = OpenBlock(out, 0, 1, L"StringFileInfo");
	{
		const size_t table = OpenBlock(out, 0, 1, kVersionLangKey);
		for (const auto &item : strings)
			AppendStringNode(out, item.first.c_str(), item.second);
		CloseBlock(out, table);
	}
	CloseBlock(out, fileInfo);

	const size_t varInfo = OpenBlock(out, 0, 1, L"VarFileInfo");
	{
		const size_t var = OpenBlock(out, sizeof(DWORD), 0, L"Translation");
		const DWORD translation = (static_cast<DWORD>(kVersionLang) << 16) | 0x04B0;
		const size_t at = out.size();
		out.resize(at + sizeof(translation));
		memcpy(out.data() + at, &translation, sizeof(translation));
		AlignBlock(out);
		CloseBlock(out, var);
	}
	CloseBlock(out, varInfo);

	CloseBlock(out, root);
	return out;
}

bool ApplyVersion(HANDLE aUpdate, const std::wstring &aModulePath, const VersionFields &aFields,
	std::string &aError)
{
	// Drop whatever the base carried first: leaving two translation tables lets
	// Explorer's property page pick the stale one.
	for (const ResourceRef &ref : ListResources(aModulePath, MAKEINTRESOURCEW(kRtVersion)))
	{
		if (!UpdateResourceW(aUpdate, MAKEINTRESOURCEW(kRtVersion),
				MAKEINTRESOURCEW(ref.id), ref.lang, nullptr, 0))
		{
			aError = "无法移除底座自带的版本信息（错误码 "
				+ std::to_string(GetLastError()) + "）";
			return false;
		}
	}

	const std::vector<BYTE> blob = BuildVersionBlob(aFields);
	if (!UpdateResourceW(aUpdate, MAKEINTRESOURCEW(kRtVersion), MAKEINTRESOURCEW(1),
			kVersionLang, const_cast<BYTE *>(blob.data()), static_cast<DWORD>(blob.size())))
	{
		aError = "写入版本信息失败（错误码 " + std::to_string(GetLastError()) + "）";
		return false;
	}
	return true;
}

// Copies the base image and replaces its script resource. The result is built
// beside the destination and moved into place, so a failure can never leave a
// half-written exe where the user expects a working one.
bool BundleScript(const std::wstring &aBasePath, const std::wstring &aOutPath,
	const std::string &aScriptUtf8, const std::wstring &aIconPath,
	const VersionFields &aVersion, std::string &aError)
{
	const std::wstring temp = aOutPath + L".compiling";
	DeleteFileW(temp.c_str());
	if (!CopyFileW(aBasePath.c_str(), temp.c_str(), FALSE))
	{
		aError = "复制编译底座失败（错误码 " + std::to_string(GetLastError()) + "）";
		return false;
	}

	HANDLE update = BeginUpdateResourceW(temp.c_str(), FALSE);
	if (!update)
	{
		aError = "无法打开目标文件写入资源（错误码 "
			+ std::to_string(GetLastError()) + "）";
		DeleteFileW(temp.c_str());
		return false;
	}

	const DWORD bytes = static_cast<DWORD>(aScriptUtf8.size());
	const BOOL written = UpdateResourceW(update,
		reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(kRcData)),
		reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(kScriptResourceId)),
		kResourceLanguage,
		const_cast<void *>(static_cast<const void *>(aScriptUtf8.data())), bytes);
	const DWORD writeError = written ? 0 : GetLastError();

	// m189: artwork rides the same update handle. Icon first (it edits the group
	// the base ships), then the version block (built from scratch).
	bool applied = written;
	if (applied && !aIconPath.empty())
		applied = ApplyIcon(update, temp, aIconPath, aError);
	if (applied)
		applied = ApplyVersion(update, temp, aVersion, aError);

	// Discard on failure: keeping the update would commit a partial resource.
	if (!EndUpdateResourceW(update, applied ? FALSE : TRUE))
	{
		aError = "写入资源后保存失败（错误码 "
			+ std::to_string(GetLastError()) + "）";
		DeleteFileW(temp.c_str());
		return false;
	}
	if (!written)
	{
		aError = "写入脚本资源失败（错误码 " + std::to_string(writeError) + "）";
		DeleteFileW(temp.c_str());
		return false;
	}
	if (!applied)
	{
		DeleteFileW(temp.c_str());
		return false;
	}

	if (!MoveFileExW(temp.c_str(), aOutPath.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		aError = "无法把产物移动到目标位置（错误码 "
			+ std::to_string(GetLastError()) + "）";
		DeleteFileW(temp.c_str());
		return false;
	}
	return true;
}

} // namespace

bool ResolveCompileTools(const std::wstring &aExeDir, std::wstring &aCompilerOut,
	std::wstring &aBaseOut, std::string &aError)
{
	// m188: there is no external compiler any more. aCompilerOut stays in the
	// signature for callers that probe the tool set, and is always empty - a
	// non-empty value here now means nothing at all. m192 P4: the base is the
	// shipped kernel itself, not a second image under tools\.
	aCompilerOut.clear();
	aBaseOut.clear();
	aError.clear();

	const std::wstring root = TrimTrailingSlash(aExeDir);
	aBaseOut = root + L"\\" + kBaseLeaf;
	if (!FileExists(aBaseOut))
	{
		aError = "找不到内核 AutoHotkey64.exe——请用完整安装包安装 AHKDeck";
		aBaseOut.clear();
		return false;
	}
	return true;
}

std::wstring DefaultExePathFor(const std::wstring &aScriptPath)
{
	if (aScriptPath.size() >= 4 &&
		_wcsicmp(aScriptPath.c_str() + aScriptPath.size() - 4, L".ahk") == 0)
		return aScriptPath.substr(0, aScriptPath.size() - 4) + L".exe";
	return aScriptPath + L".exe";
}

namespace
{
std::wstring LeafOf(const std::wstring &aPath)
{
	const size_t slash = aPath.find_last_of(L"\\/");
	return slash == std::wstring::npos ? aPath : aPath.substr(slash + 1);
}

std::wstring WithoutExtension(const std::wstring &aPath)
{
	const size_t slash = aPath.find_last_of(L"\\/");
	const size_t dot = aPath.find_last_of(L'.');
	if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
		return aPath;
	return aPath.substr(0, dot);
}
} // namespace

ExeCompileOptions DefaultCompileOptionsFor(const std::wstring &aScriptPath)
{
	ExeCompileOptions options;
	options.fileVersion = L"1.0.0.0";
	// A sibling "<script>.ico" is the whole convention: drop one next to the
	// script and the next compile adopts it - no dialog, nothing to remember.
	const std::wstring ico = WithoutExtension(aScriptPath) + L".ico";
	if (FileExists(ico))
		options.iconPath = ico;
	return options;
}

ExeCompileOutcome CompileScriptToExe(const std::wstring &aExeDir,
	const std::wstring &aScriptPath, const std::wstring &aOutPath, int aTimeoutMs)
{
	return CompileScriptToExe(aExeDir, aScriptPath, aOutPath,
		DefaultCompileOptionsFor(aScriptPath), aTimeoutMs);
}

ExeCompileOutcome CompileScriptToExe(const std::wstring &aExeDir,
	const std::wstring &aScriptPath, const std::wstring &aOutPath,
	const ExeCompileOptions &aOptions, int aTimeoutMs)
{
	ExeCompileOutcome outcome;
	outcome.outPath = aOutPath;

	std::wstring unusedCompiler;
	std::wstring base;
	if (!ResolveCompileTools(aExeDir, unusedCompiler, base, outcome.error))
		return outcome;
	if (!FileExists(aScriptPath))
	{
		outcome.error = "脚本文件不存在或已被移动";
		return outcome;
	}

	// The runtime wants UTF-8 bytes, and the file on disk may be GBK or have a
	// BOM. Reading through TextFile means one encoding rule for the whole deck.
	TextDocument document;
	std::string readError;
	if (!ReadTextDocument(aScriptPath, document, readError))
	{
		outcome.error = "读取脚本失败：" + readError;
		return outcome;
	}

	const std::wstring root = TrimTrailingSlash(aExeDir);
	const std::wstring tempDir = TempDirUnder(root, kTempWork);
	const std::wstring tag = std::to_wstring(GetCurrentProcessId()) + L"-" +
		std::to_wstring(static_cast<unsigned long long>(GetTickCount64()));

	// Preserve an earlier artifact BEFORE anything is produced: the overwrite
	// contract is rename-to-.old (same as the downloader), never truncate.
	if (FileExists(aOutPath))
	{
		const std::wstring backup = aOutPath + L".old";
		DeleteFileW(backup.c_str());
		if (!MoveFileW(aOutPath.c_str(), backup.c_str()))
		{
			outcome.error = "旧产物无法备份（可能正被占用），编译已中止";
			return outcome;
		}
		outcome.backedUp = true;
	}

	// Validate first: a bundle of a script that cannot load is a trap the user
	// only discovers by double-clicking the result.
	std::string report;
	std::string validateError;
	const int verdict = ValidateInChild(root, aScriptPath, tempDir, tag, aTimeoutMs,
		report, validateError);
	if (verdict < 0)
	{
		outcome.error = validateError.empty() ? "脚本校验失败" : validateError;
		return outcome;
	}
	if (verdict != 0)
	{
		outcome.rc = verdict;
		const std::string readable = ReadableDiagnostics(report);
		outcome.output = readable.empty() ? report : readable;
		return outcome;
	}

	// Defaults come from the script, never from a question the user did not ask.
	const std::wstring scriptName = LeafOf(WithoutExtension(aScriptPath));
	VersionFields version;
	version.fileVersion = aOptions.fileVersion.empty() ? L"1.0.0.0" : aOptions.fileVersion;
	version.productName = aOptions.productName.empty() ? scriptName : aOptions.productName;
	version.fileDescription = aOptions.fileDescription.empty()
		? scriptName : aOptions.fileDescription;
	version.companyName = aOptions.companyName;
	version.legalCopyright = aOptions.legalCopyright;
	version.originalFilename = LeafOf(aOutPath);

	if (!BundleScript(base, aOutPath, document.utf8, aOptions.iconPath, version, outcome.error))
		return outcome;

	outcome.rc = 0;
	outcome.output = "已写入脚本资源：" + std::to_string(document.utf8.size()) + " 字节"
		+ "，版本 " + Narrow(version.fileVersion);
	if (!aOptions.iconPath.empty())
		outcome.output += "，图标 " + Narrow(LeafOf(aOptions.iconPath));
	outcome.produced = FileExists(aOutPath);
	if (!outcome.produced)
		outcome.error = "打包完成，但目标文件不存在";
	return outcome;
}

} // namespace ahkdeck
