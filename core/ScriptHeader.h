// core/ScriptHeader.h - what a script file says about itself (m151).
//
// Two independent layers, because a script the user typed in Notepad has only
// the second one:
//   * the leading `; ==UserScript==` block, an oil-monkey style header with
//     11 known keys (name/version/author/date/license/description/...);
//   * plain file facts - bytes, mtime, line count - which every .ahk has.
// The info window shows the header when it exists and always shows the facts,
// so a header-less script is never an empty window.
//
// Direction: core only. No FLTK, no window; `InspectScript` is cheap enough to
// run on the UI thread (it reads one text file the deck already opens).

#pragma once

#include <string>
#include <vector>

namespace ahkdeck
{

struct ScriptHeaderField
{
	std::string key;   // without the leading '@', e.g. "author"
	std::string value; // everything after the key, trimmed (may contain spaces)
};

struct ScriptInspection
{
	bool headerFound = false;
	// File order is preserved. The info dialog imposes its own display order so
	// three scripts with differently ordered headers still read the same way;
	// keys this build does not know about are not dropped.
	std::vector<ScriptHeaderField> fields;

	long long sizeBytes = -1; // -1 when the file could not be stat'ed
	std::string modified;     // local time "YYYY-MM-DD HH:MM", empty on failure
	int lineCount = -1;       // -1 when the file could not be decoded
	std::string error;        // read failure reason (relative-safe, short)

	// Empty when the key is absent or the header was not found.
	std::string Get(const char *aKey) const;
};

// Reads the file once: decodes it (BOM/UTF-16/ANSI tolerant via TextFile),
// counts lines, parses the header block, and stats the file. Returns false only
// when the file could not be read at all; `aOut.error` then says why.
bool InspectScript(const std::wstring &aPath, ScriptInspection &aOut);

// `; @version      1.0.0` from the leading block, empty when absent. Thin
// wrapper over InspectScript so the catalog and the info window cannot drift
// into two different parsers - which is exactly what the old hand-rolled
// `find_last_of(' ')` did: it returned "ms）" for a description containing
// spaces, harmless only because @version never has any.
std::string ReadScriptVersion(const std::wstring &aPath);

// "42.8 KB（43,788 字节）" - both numbers, because the catalog's size_kb is an
// LF byte count while the file on disk may be CRLF and therefore larger.
std::string FormatScriptSize(long long aBytes);

} // namespace ahkdeck
