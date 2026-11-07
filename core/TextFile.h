// Text file codec for the built-in editor (m015).
//
// The editor buffer is always UTF-8 with LF line endings. Everything the disk
// format needs - BOM detection, UTF-16 conversion and CRLF preservation - lives
// here, so it can be unit-tested without FLTK or a window.
//
// Direction: core has no UI and no engine dependency.

#pragma once

#include <cstddef>
#include <string>

namespace ahkdeck
{

enum class TextEncoding
{
	Utf8NoBom,
	Utf8Bom,
	Utf16Le,
	// m080: BOM-less bytes that are not valid UTF-8, decoded with the system
	// ANSI code page on the READ side only - WriteTextDocument never produces
	// it (see there).
	Ansi
};

const char *TextEncodingName(TextEncoding aEncoding);

struct TextDocument
{
	std::string utf8; // normalized to LF, no BOM
	TextEncoding encoding = TextEncoding::Utf8NoBom;
	bool crlf = false; // the file used CRLF (a mixed file is reported as CRLF)
	bool crlfOnly = false; // every newline was CRLF (mixed / LF files are false)
};

// Reads a script file, decodes it to UTF-8/LF and remembers the original
// encoding and line endings. aError is a short relative-safe reason.
bool ReadTextDocument(const std::wstring &aPath, TextDocument &aOut, std::string &aError);

// Writes UTF-8/LF text in the requested disk format. aBytesWritten receives
// the on-disk byte count (BOM included) for the console receipt.
bool WriteTextDocument(const std::wstring &aPath, const std::string &aUtf8,
	TextEncoding aEncoding, bool aCrlf, std::size_t *aBytesWritten, std::string &aError);

} // namespace ahkdeck
