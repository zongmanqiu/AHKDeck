// m151: the script header parser. cwd-independent (writes into %TEMP%).
//
// The cases below are the ones that actually broke while building the info
// window: a value containing spaces, a body that merely MENTIONS the marker,
// a block that never closes, and a file with no header at all.

#include "core/ScriptHeader.h"

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

void RemoveAt(const std::wstring &aPath)
{
	DeleteFileW(aPath.c_str());
}

} // namespace

int main()
{
	// 1. A well-formed block. The description contains spaces AND a unit with a
	//    space - the old find_last_of(' ') would have returned "ms".
	const std::wstring header = TempPath(L"m151_header.ahk");
	WriteRaw(header,
		"; ==UserScript==\n"
		"; @name         测试脚本\n"
		"; @version      1.2.0\n"
		"; @description  含空格的描述 10~500 ms\n"
		"; @author       某人\n"
		"; ==/UserScript==\n"
		"\n"
		"MsgBox \"hi\"\n");
	ahkdeck::ScriptInspection scan;
	Check(ahkdeck::InspectScript(header, scan), "read a header file");
	Check(scan.headerFound, "header found");
	Check(scan.fields.size() == 4, "four metadata keys");
	Check(scan.Get("version") == "1.2.0", "version value");
	Check(scan.Get("description") == "含空格的描述 10~500 ms",
		"description keeps everything after the key");
	Check(scan.Get("author") == "某人", "author value");
	Check(scan.Get("name") == "测试脚本", "name value");
	Check(scan.Get("license").empty(), "absent key is empty");
	Check(scan.lineCount == 8, "line count");
	Check(scan.sizeBytes > 0, "size in bytes");
	Check(!scan.modified.empty(), "modified time formatted");

	// 2. The marker on line 2, after a #Requires directive.
	const std::wstring after = TempPath(L"m151_after.ahk");
	WriteRaw(after,
		"#Requires AutoHotkey v2.0\n"
		"; ==UserScript==\n"
		"; @name  甲\n"
		"; ==/UserScript==\n");
	ahkdeck::ScriptInspection scan2;
	ahkdeck::InspectScript(after, scan2);
	Check(scan2.headerFound, "header after #Requires is found");
	Check(scan2.Get("name") == "甲", "two-space separator");

	// 3. No header, but the BODY mentions the marker. This used to be taken as
	//    a header and left the info window with an empty first group.
	const std::wstring body = TempPath(L"m151_body.ahk");
	WriteRaw(body,
		"#Requires AutoHotkey v2.0\n"
		"; 普通注释\n"
		"MsgBox \"==UserScript==\"\n");
	ahkdeck::ScriptInspection scan3;
	ahkdeck::InspectScript(body, scan3);
	Check(!scan3.headerFound, "a marker in the body is not a header");
	Check(scan3.fields.empty(), "no fields from the body");
	Check(scan3.lineCount == 3, "body line count");

	// 4. A block that never closes still yields its keys.
	const std::wstring open = TempPath(L"m151_open.ahk");
	WriteRaw(open,
		"; ==UserScript==\n"
		"; @name  乙\n");
	ahkdeck::ScriptInspection scan4;
	ahkdeck::InspectScript(open, scan4);
	Check(scan4.headerFound, "unterminated block still counts as a header");
	Check(scan4.Get("name") == "乙", "key before the end of file");

	// 5. Empty file: no header, zero lines, no crash.
	const std::wstring empty = TempPath(L"m151_empty.ahk");
	WriteRaw(empty, std::string());
	ahkdeck::ScriptInspection scan5;
	Check(ahkdeck::InspectScript(empty, scan5), "empty file is readable");
	Check(!scan5.headerFound, "empty file has no header");
	Check(scan5.lineCount == 0, "empty file has no lines");
	Check(scan5.sizeBytes == 0, "empty file has zero bytes");

	// 6. Missing file is a clean failure, not a crash.
	ahkdeck::ScriptInspection scan6;
	Check(!ahkdeck::InspectScript(TempPath(L"m151_absent.ahk"), scan6),
		"missing file reports failure");
	Check(!scan6.error.empty(), "failure carries a reason");

	// 7. Formatting: both numbers, one decimal above a kilobyte.
	const std::string big = ahkdeck::FormatScriptSize(43788);
	Check(big.find("42.8 KB") != std::string::npos, "size KB with one decimal");
	Check(big.find("43,788") != std::string::npos, "size keeps exact bytes");
	Check(ahkdeck::FormatScriptSize(500) == "500 字节", "sub-kilobyte size");
	Check(ahkdeck::FormatScriptSize(-1).empty(), "unknown size is empty");

	for (const std::wstring &path : { header, after, body, open, empty })
		RemoveAt(path);

	if (gFailures != 0)
	{
		std::printf("%d failure(s)\n", gFailures);
		return 1;
	}
	std::printf("all script-header checks passed\n");
	return 0;
}
