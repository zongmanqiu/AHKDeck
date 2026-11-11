// core/ScriptHeader.cpp - see core/ScriptHeader.h.
#include "core/ScriptHeader.h"

#include "core/TextFile.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace ahkdeck
{
namespace
{

std::string TrimBoth(const std::string &aText)
{
	size_t first = 0;
	while (first < aText.size() && (aText[first] == ' ' || aText[first] == '\t'
		|| aText[first] == '\r'))
		++first;
	size_t last = aText.size();
	while (last > first && (aText[last - 1] == ' ' || aText[last - 1] == '\t'
		|| aText[last - 1] == '\r'))
		--last;
	return aText.substr(first, last - first);
}

// A header block that never closes must not swallow the whole file: real
// headers are 10-14 lines, so a generous cap keeps a broken file from turning
// its body into "metadata".
constexpr int kMaxHeaderLines = 80;

void ParseHeaderBody(const std::string &aUtf8, ScriptInspection &aOut)
{
	size_t pos = 0;
	bool started = false;
	for (int lineNo = 0; lineNo < kMaxHeaderLines; ++lineNo)
	{
		// break, never return: a block that runs to the end of the file is still
		// a header, and the caller has to be told so after the loop.
		if (pos >= aUtf8.size())
			break;
		const size_t end = aUtf8.find('\n', pos);
		const std::string line = aUtf8.substr(pos,
			end == std::string::npos ? std::string::npos : end - pos);

		const std::string trimmed = TrimBoth(line);
		if (!started)
		{
			// The marker is looked for anywhere in the opening lines rather than
			// on line 1: a hand-written script often starts with #Requires or
			// #SingleInstance before the metadata block. It must be a COMMENT,
			// though - a script that merely mentions the marker in a string or
			// in its body is not publishing a header.
			if (!trimmed.empty() && trimmed[0] == ';'
				&& trimmed.find("==UserScript==") != std::string::npos)
				started = true;
		}
		else
		{
			if (trimmed.find("==/UserScript==") != std::string::npos)
			{
				aOut.headerFound = true;
				return;
			}
			// Only `; @key  value` lines are metadata. The block is pure
			// comments, so a line that does not start with ';' ends it.
			if (trimmed.empty() || trimmed[0] != ';')
			{
				aOut.headerFound = true;
				return;
			}
			const std::string inner = TrimBoth(trimmed.substr(1));
			if (inner.empty() || inner[0] != '@')
				continue;

			// Key runs to the first blank, the value is EVERYTHING after it.
			// The old LocalScriptVersion used find_last_of(' \t'), which would
			// have returned "ms）" for `@description ... （10~500 ms）`.
			const size_t split = inner.find_first_of(" \t");
			ScriptHeaderField field;
			if (split == std::string::npos)
			{
				field.key = inner.substr(1);
			}
			else
			{
				field.key = inner.substr(1, split - 1);
				field.value = TrimBoth(inner.substr(split + 1));
			}
			if (!field.key.empty())
				aOut.fields.push_back(field);
		}

		if (end == std::string::npos)
			break;
		pos = end + 1;
	}
	if (started)
		aOut.headerFound = true;
}

int CountLines(const std::string &aUtf8)
{
	if (aUtf8.empty())
		return 0;
	int lines = 0;
	for (char c : aUtf8)
	{
		if (c == '\n')
			++lines;
	}
	if (aUtf8.back() != '\n')
		++lines;
	return lines;
}

std::string FormatFileTime(const FILETIME &aUtc)
{
	FILETIME local = {};
	if (!FileTimeToLocalFileTime(&aUtc, &local))
		return std::string();
	SYSTEMTIME st = {};
	if (!FileTimeToSystemTime(&local, &st))
		return std::string();
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u", st.wYear,
		st.wMonth, st.wDay, st.wHour, st.wMinute);
	return std::string(buf);
}

std::string WithThousands(long long aBytes)
{
	char digits[32];
	std::snprintf(digits, sizeof(digits), "%lld", aBytes < 0 ? 0 : aBytes);
	const std::string raw(digits);
	std::string out;
	int count = 0;
	for (size_t i = raw.size(); i-- > 0;)
	{
		out.insert(out.begin(), raw[i]);
		if (++count % 3 == 0 && i > 0 && raw[i - 1] != '-')
			out.insert(out.begin(), ',');
	}
	return out;
}

} // namespace

std::string ScriptInspection::Get(const char *aKey) const
{
	if (!aKey)
		return std::string();
	for (const ScriptHeaderField &field : fields)
	{
		if (_stricmp(field.key.c_str(), aKey) == 0)
			return field.value;
	}
	return std::string();
}

bool InspectScript(const std::wstring &aPath, ScriptInspection &aOut)
{
	aOut = ScriptInspection();

	WIN32_FILE_ATTRIBUTE_DATA info = {};
	if (GetFileAttributesExW(aPath.c_str(), GetFileExInfoStandard, &info))
	{
		LARGE_INTEGER size;
		size.HighPart = (LONG)info.nFileSizeHigh;
		size.LowPart = info.nFileSizeLow;
		aOut.sizeBytes = (long long)size.QuadPart;
		aOut.modified = FormatFileTime(info.ftLastWriteTime);
	}
	else
	{
		aOut.error = "无法读取文件属性";
	}

	TextDocument doc;
	std::string error;
	if (!ReadTextDocument(aPath, doc, error))
	{
		if (aOut.error.empty())
			aOut.error = error.empty() ? "无法读取脚本文件" : error;
		return aOut.sizeBytes >= 0;
	}

	aOut.lineCount = CountLines(doc.utf8);
	ParseHeaderBody(doc.utf8, aOut);
	return true;
}

std::string ReadScriptVersion(const std::wstring &aPath)
{
	ScriptInspection scan;
	if (!InspectScript(aPath, scan))
		return std::string();
	return scan.Get("version");
}

std::string FormatScriptSize(long long aBytes)
{
	if (aBytes < 0)
		return std::string();
	const std::string exact = WithThousands(aBytes);
	if (aBytes < 1024)
		return exact + " 字节";
	const double kb = (double)aBytes / 1024.0;
	char buf[32];
	if (kb < 1024.0)
		std::snprintf(buf, sizeof(buf), "%.1f KB", kb);
	else
		std::snprintf(buf, sizeof(buf), "%.1f MB", kb / 1024.0);
	return std::string(buf) + "（" + exact + " 字节）";
}

} // namespace ahkdeck
