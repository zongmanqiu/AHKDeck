#include "core/TextFile.h"

#include <windows.h>

#include <cstring>

namespace ahkdeck
{
namespace
{

bool ReadAllBytes(const std::wstring &aPath, std::string &aOut, std::string &aError)
{
	HANDLE file = CreateFileW(aPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		aError = "cannot open the file for reading";
		return false;
	}

	char chunk[8192];
	DWORD got = 0;
	while (ReadFile(file, chunk, sizeof(chunk), &got, nullptr) && got)
		aOut.append(chunk, got);
	CloseHandle(file);
	return true;
}

// m094: every write goes to <path>.tmp, is flushed, and is then renamed over
// the destination. A power cut halfway through used to leave a truncated
// settings.json, which the UI then refused to overwrite forever; now the old
// file either survives intact or is replaced by a complete new one.
bool WriteAllBytes(const std::wstring &aPath, const std::string &aBytes, std::string &aError)
{
	const std::wstring tempPath = aPath + L".tmp";
	HANDLE file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		aError = "cannot create the file for writing";
		return false;
	}

	size_t offset = 0;
	while (offset < aBytes.size())
	{
		DWORD chunk = (DWORD)((aBytes.size() - offset) > 0x10000 ? 0x10000 : (aBytes.size() - offset));
		DWORD wrote = 0;
		if (!WriteFile(file, aBytes.data() + offset, chunk, &wrote, nullptr) || wrote == 0)
		{
			CloseHandle(file);
			DeleteFileW(tempPath.c_str());
			aError = "the write failed (disk full or access denied)";
			return false;
		}
		offset += wrote;
	}
	FlushFileBuffers(file);
	CloseHandle(file);

	// Same volume, so the rename is atomic. WRITE_THROUGH makes the metadata
	// durable before we report success.
	if (!MoveFileExW(tempPath.c_str(), aPath.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		DeleteFileW(tempPath.c_str());
		aError = "cannot replace the existing file";
		return false;
	}
	return true;
}

// Converts CRLF / lone CR to LF and reports whether any CRLF was present.
// aCrlfOnly is true only when every newline was CRLF (mixed / LF files are not
// canonical and must be rewritten by the UI).
std::string NormalizeNewlines(const std::string &aIn, bool &aCrlf, bool *aCrlfOnly = nullptr)
{
	std::string out;
	out.reserve(aIn.size());
	aCrlf = false;
	bool crlfOnly = true;
	for (size_t i = 0; i < aIn.size(); ++i)
	{
		char c = aIn[i];
		if (c == '\r')
		{
			if (i + 1 < aIn.size() && aIn[i + 1] == '\n')
			{
				aCrlf = true;
				++i;
			}
			else
				crlfOnly = false; // lone CR
			out += '\n';
		}
		else
		{
			if (c == '\n')
				crlfOnly = false; // lone LF
			out += c;
		}
	}
	if (aCrlfOnly)
		*aCrlfOnly = crlfOnly;
	return out;
}

std::string ToCrlf(const std::string &aLf)
{
	std::string out;
	out.reserve(aLf.size() + aLf.size() / 16);
	for (char c : aLf)
	{
		if (c == '\n')
			out += '\r';
		out += c;
	}
	return out;
}

bool Utf16LeToUtf8(const std::string &aBytes, std::string &aOut, std::string &aError)
{
	if (aBytes.size() % 2 != 0)
	{
		aError = "the UTF-16LE file has an odd byte count";
		return false;
	}
	int count = (int)(aBytes.size() / 2);
	if (count == 0)
	{
		aOut.clear();
		return true;
	}
	const wchar_t *wide = reinterpret_cast<const wchar_t *>(aBytes.data());
	int n = WideCharToMultiByte(CP_UTF8, 0, wide, count, nullptr, 0, nullptr, nullptr);
	if (n <= 0)
	{
		aError = "the UTF-16LE text could not be decoded";
		return false;
	}
	aOut.assign((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide, count, aOut.data(), n, nullptr, nullptr);
	return true;
}

bool Utf8ToUtf16Le(const std::string &aUtf8, std::string &aOut, std::string &aError)
{
	if (aUtf8.empty())
	{
		aOut.clear();
		return true;
	}
	int n = MultiByteToWideChar(CP_UTF8, 0, aUtf8.data(), (int)aUtf8.size(), nullptr, 0);
	if (n <= 0)
	{
		aError = "the text could not be converted to UTF-16LE";
		return false;
	}
	std::wstring wide((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, aUtf8.data(), (int)aUtf8.size(), wide.data(), n);
	aOut.assign(reinterpret_cast<const char *>(wide.data()), wide.size() * sizeof(wchar_t));
	return true;
}

} // namespace

const char *TextEncodingName(TextEncoding aEncoding)
{
	switch (aEncoding)
	{
	case TextEncoding::Utf8Bom: return "UTF-8 BOM";
	case TextEncoding::Utf16Le: return "UTF-16LE";
	case TextEncoding::Ansi: return "ANSI";
	default: return "UTF-8";
	}
}

bool ReadTextDocument(const std::wstring &aPath, TextDocument &aOut, std::string &aError)
{
	std::string bytes;
	if (!ReadAllBytes(aPath, bytes, aError))
		return false;

	TextEncoding encoding = TextEncoding::Utf8NoBom;
	size_t begin = 0;
	if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF
		&& (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
	{
		encoding = TextEncoding::Utf8Bom;
		begin = 3;
	}
	else if (bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xFE)
	{
		encoding = TextEncoding::Utf16Le;
		begin = 2;
	}

	std::string decoded;
	if (encoding == TextEncoding::Utf16Le)
	{
		std::string payload(bytes.data() + begin, bytes.size() - begin);
		if (!Utf16LeToUtf8(payload, decoded, aError))
			return false;
	}
	else
	{
		// m080: a BOM-less file is UTF-8 only if it actually validates as UTF-8.
		// The old code assigned the raw bytes unconditionally, so an ANSI/GBK
		// script was read as mojibake and the editor's canonical rewrite
		// (ui/DeckUI.cpp LoadIntoEditor) persisted it - opening the file was
		// enough to corrupt it. The kernel keeps the official rule (BOM-less =
		// UTF-8, invalid sequences become U+FFFD: script.h DEFAULT_READ_FLAGS has
		// no DETECT_UTF8 and the default script codepage is CP_UTF8); the editor
		// is the deck's own component and may read smarter, while the write side
		// stays UTF-8-only (see WriteTextDocument).
		const int utf8Len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			bytes.data() + begin, (int)(bytes.size() - begin), nullptr, 0);
		if (utf8Len > 0 || bytes.size() == begin)
		{
			decoded.assign(bytes.data() + begin, bytes.size() - begin);
		}
		else
		{
			const int wideLen = MultiByteToWideChar(CP_ACP, 0,
				bytes.data() + begin, (int)(bytes.size() - begin), nullptr, 0);
			if (wideLen <= 0)
			{
				aError = "the file is neither valid UTF-8 nor text in the system ANSI code page";
				return false;
			}
			std::wstring wide((size_t)wideLen, L'\0');
			MultiByteToWideChar(CP_ACP, 0, bytes.data() + begin,
				(int)(bytes.size() - begin), wide.data(), wideLen);
			const int outLen = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wideLen,
				nullptr, 0, nullptr, nullptr);
			decoded.assign((size_t)outLen, '\0');
			WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wideLen,
				decoded.data(), outLen, nullptr, nullptr);
			encoding = TextEncoding::Ansi;
		}
	}

	bool crlf = false;
	bool crlfOnly = false;
	std::string normalized = NormalizeNewlines(decoded, crlf, &crlfOnly);

	aOut.utf8 = std::move(normalized);
	aOut.encoding = encoding;
	aOut.crlf = crlf;
	aOut.crlfOnly = crlfOnly;
	return true;
}

bool WriteTextDocument(const std::wstring &aPath, const std::string &aUtf8,
	TextEncoding aEncoding, bool aCrlf, std::size_t *aBytesWritten, std::string &aError)
{
	// m080: the write side never produces ANSI. A read-side Ansi document
	// already carries corrected text; persisting it as UTF-8 without a BOM is
	// the m042 canonical form, and official AutoHotkey v2 reads that cleanly.
	if (aEncoding == TextEncoding::Ansi)
		aEncoding = TextEncoding::Utf8NoBom;

	bool ignored = false;
	std::string lf = NormalizeNewlines(aUtf8, ignored);
	std::string payload = aCrlf ? ToCrlf(lf) : lf;

	std::string bytes;
	if (aEncoding == TextEncoding::Utf16Le)
	{
		std::string utf16;
		if (!Utf8ToUtf16Le(payload, utf16, aError))
			return false;
		bytes.reserve(utf16.size() + 2);
		bytes += '\xFF';
		bytes += '\xFE';
		bytes += utf16;
	}
	else
	{
		if (aEncoding == TextEncoding::Utf8Bom)
		{
			bytes.reserve(payload.size() + 3);
			bytes += '\xEF';
			bytes += '\xBB';
			bytes += '\xBF';
		}
		bytes += payload;
	}

	if (!WriteAllBytes(aPath, bytes, aError))
		return false;
	if (aBytesWritten)
		*aBytesWritten = bytes.size();
	return true;
}

} // namespace ahkdeck
