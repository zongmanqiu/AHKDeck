// m015: the editor's disk codec must round-trip every format AHK accepts.
// Runs with cwd = .temp/textfile_gate (ctest), so nothing lands in the source tree.

#include "core/TextFile.h"

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

bool ReadRaw(const std::wstring &aPath, std::string &aOut)
{
	HANDLE file = CreateFileW(aPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	char buf[4096];
	DWORD got = 0;
	while (ReadFile(file, buf, sizeof(buf), &got, nullptr) && got)
		aOut.append(buf, got);
	CloseHandle(file);
	return true;
}

bool WriteRaw(const std::wstring &aPath, const std::string &aBytes)
{
	HANDLE file = CreateFileW(aPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	DWORD wrote = 0;
	const bool ok = WriteFile(file, aBytes.data(), (DWORD)aBytes.size(), &wrote, nullptr)
		&& wrote == aBytes.size();
	CloseHandle(file);
	return ok;
}

// ReadTextDocument normalizes to LF; expectations built from CRLF text must go
// through the same shape before comparing.
std::string ToLf(std::string aText)
{
	std::string out;
	for (size_t i = 0; i < aText.size(); ++i)
	{
		if (aText[i] == '\r' && i + 1 < aText.size() && aText[i + 1] == '\n')
			continue;
		out += aText[i];
	}
	return out;
}

} // namespace

int main()
{
	const std::wstring dir = L"textfile_gate";
	CreateDirectoryW(dir.c_str(), nullptr);

	std::string error;
	std::size_t bytes = 0;
	ahkdeck::TextDocument document;

	// UTF-8 without BOM, CRLF preserved.
	{
		const std::wstring path = dir + L"\\utf8.ahk";
		Check(ahkdeck::WriteTextDocument(path, "A\nB\n", ahkdeck::TextEncoding::Utf8NoBom,
			true, &bytes, error), "utf8 no-bom write");
		Check(bytes == 6, "utf8 no-bom byte count (CRLF)");
		std::string raw;
		Check(ReadRaw(path, raw) && raw == "A\r\nB\r\n", "utf8 no-bom bytes on disk");
		Check(ahkdeck::ReadTextDocument(path, document, error), "utf8 no-bom read");
		Check(document.encoding == ahkdeck::TextEncoding::Utf8NoBom, "utf8 no-bom encoding detected");
		Check(document.crlf, "utf8 no-bom CRLF detected");
		Check(document.crlfOnly, "utf8 no-bom is pure CRLF");
		Check(document.utf8 == "A\nB\n", "utf8 no-bom content normalized");
	}

	// UTF-8 with BOM, Chinese, LF preserved.
	{
		const std::wstring path = dir + L"\\utf8bom.ahk";
		const std::string text = "\xE4\xBD\xA0\xE5\xA5\xBD\n\xE4\xB8\x96\xE7\x95\x8C\n"; // 你好\n世界\n
		Check(ahkdeck::WriteTextDocument(path, text, ahkdeck::TextEncoding::Utf8Bom,
			false, &bytes, error), "utf8 bom write");
		Check(bytes == text.size() + 3, "utf8 bom byte count");
		std::string raw;
		Check(ReadRaw(path, raw) && raw.size() >= 3
			&& (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB
			&& (unsigned char)raw[2] == 0xBF, "utf8 bom prefix");
		Check(ahkdeck::ReadTextDocument(path, document, error), "utf8 bom read");
		Check(document.encoding == ahkdeck::TextEncoding::Utf8Bom, "utf8 bom encoding detected");
		Check(!document.crlf, "utf8 bom LF detected");
		Check(!document.crlfOnly, "utf8 bom is not pure CRLF");
		Check(document.utf8 == text, "utf8 bom content round-trip");
	}

	// UTF-16LE with BOM, Chinese, CRLF.
	{
		const std::wstring path = dir + L"\\utf16.ahk";
		const std::string text = "\xE7\xAC\xAC\xE4\xB8\x80\xE8\xA1\x8C\n"      // 第一行\n
			"\xE7\xAC\xAC\xE4\xBA\x8C\xE8\xA1\x8C\n";                          // 第二行\n
		Check(ahkdeck::WriteTextDocument(path, text, ahkdeck::TextEncoding::Utf16Le,
			true, &bytes, error), "utf16 write");
		Check(bytes == 2 + (3 + 2 + 3 + 2) * 2, "utf16 byte count (BOM + CRLF)");
		std::string raw;
		Check(ReadRaw(path, raw) && raw.size() >= 2
			&& (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE, "utf16 BOM");
		Check(ahkdeck::ReadTextDocument(path, document, error), "utf16 read");
		Check(document.encoding == ahkdeck::TextEncoding::Utf16Le, "utf16 encoding detected");
		Check(document.crlf, "utf16 CRLF detected");
		Check(document.crlfOnly, "utf16 is pure CRLF");
		Check(document.utf8 == text, "utf16 content round-trip");
	}

	// m080: a BOM-less file that is not valid UTF-8 is ANSI text. The editor
	// used to pass the raw bytes through and the canonical rewrite persisted
	// the mojibake - opening a GBK script corrupted it. Build the ANSI bytes
	// from this machine's own code page so the case is machine-independent:
	// whatever the ACP can encode must come back as the same characters.
	{
		const std::wstring path = dir + L"\\ansi.ahk";
		const std::wstring wide = L"s := \"你好\"\r\nif (s = \"你好\")\r\n    ExitApp(0)\r\n";
		const int ansiLen = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), (int)wide.size(),
			nullptr, 0, nullptr, nullptr);
		std::string ansi((size_t)ansiLen, '\0');
		WideCharToMultiByte(CP_ACP, 0, wide.c_str(), (int)wide.size(),
			ansi.data(), ansiLen, nullptr, nullptr);
		const int backLen = MultiByteToWideChar(CP_ACP, 0, ansi.data(), ansiLen, nullptr, 0);
		std::wstring back((size_t)backLen, L'\0');
		MultiByteToWideChar(CP_ACP, 0, ansi.data(), ansiLen, back.data(), backLen);
		const bool lossless = back == wide;
		Check(WriteRaw(path, ansi), "ansi raw write");
		Check(ahkdeck::ReadTextDocument(path, document, error), "ansi read");
		Check(document.crlf, "ansi CRLF detected");
		// The characters must survive the round trip whatever the code page is.
		const int expectLen = WideCharToMultiByte(CP_UTF8, 0, back.c_str(), backLen,
			nullptr, 0, nullptr, nullptr);
		std::string expected((size_t)expectLen, '\0');
		WideCharToMultiByte(CP_UTF8, 0, back.c_str(), backLen,
			expected.data(), expectLen, nullptr, nullptr);
		Check(document.utf8 == ToLf(expected), "ansi content round-trips through the code page");
		if (lossless)
		{
			Check(document.encoding == ahkdeck::TextEncoding::Ansi, "ansi encoding detected");
			const int wantLen = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(),
				nullptr, 0, nullptr, nullptr);
			std::string want((size_t)wantLen, '\0');
			WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(),
				want.data(), wantLen, nullptr, nullptr);
			Check(document.utf8 == ToLf(want), "ansi decoded with the system code page (CJK ACP)");
		}
		DeleteFileW(path.c_str());
	}

	// Mixed line endings are decoded but cannot be called canonical.
	{
		const std::wstring path = dir + L"\\mixed.ahk";
		Check(WriteRaw(path, "A\r\nB\nC\r"), "mixed raw write");
		Check(ahkdeck::ReadTextDocument(path, document, error), "mixed read");
		Check(document.crlf, "mixed contains CRLF");
		Check(!document.crlfOnly, "mixed is not pure CRLF");
		Check(document.utf8 == "A\nB\nC\n", "mixed content normalized");
		DeleteFileW(path.c_str());
	}

	// Empty BOM-only file decodes to an empty document.
	{
		const std::wstring path = dir + L"\\empty.ahk";
		Check(ahkdeck::WriteTextDocument(path, "", ahkdeck::TextEncoding::Utf8Bom,
			false, &bytes, error), "empty write");
		Check(bytes == 3, "empty file keeps its BOM");
		Check(ahkdeck::ReadTextDocument(path, document, error), "empty read");
		Check(document.utf8.empty(), "empty document");
	}

	// Missing file is an error, not a crash or an empty document.
	{
		error.clear();
		document.utf8 = "sentinel";
		Check(!ahkdeck::ReadTextDocument(dir + L"\\does-not-exist.ahk", document, error),
			"missing file reported as error");
		Check(!error.empty(), "missing file has a reason");
	}

	DeleteFileW((dir + L"\\utf8.ahk").c_str());
	DeleteFileW((dir + L"\\utf8bom.ahk").c_str());
	DeleteFileW((dir + L"\\utf16.ahk").c_str());
	DeleteFileW((dir + L"\\empty.ahk").c_str());

	if (gFailures)
	{
		std::printf("textfile: %d failure(s)\n", gFailures);
		return 1;
	}
	std::printf("textfile: all checks passed\n");
	return 0;
}
