#include "core/Library.h"

#include "core/TextFile.h"

#include <windows.h>

#include <cstring>
#include <cwctype>

namespace ahkdeck
{
namespace
{

std::string Utf8Of(const std::wstring &aWide)
{
	if (aWide.empty())
		return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, aWide.c_str(), (int)aWide.size(), nullptr, 0, nullptr, nullptr);
	std::string out((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, aWide.c_str(), (int)aWide.size(), out.data(), n, nullptr, nullptr);
	return out;
}

std::wstring WideOf(const std::string &aUtf8)
{
	if (aUtf8.empty())
		return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, aUtf8.c_str(), (int)aUtf8.size(), nullptr, 0);
	std::wstring out((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, aUtf8.c_str(), (int)aUtf8.size(), out.data(), n);
	return out;
}

bool SamePathI(const std::wstring &aLeft, const std::wstring &aRight)
{
	if (aLeft.size() != aRight.size())
		return false;
	for (size_t i = 0; i < aLeft.size(); ++i)
	{
		wchar_t l = aLeft[i] == L'/' ? L'\\' : aLeft[i];
		wchar_t r = aRight[i] == L'/' ? L'\\' : aRight[i];
		if (towlower(l) != towlower(r))
			return false;
	}
	return true;
}

bool IsBlank(const std::string &aLine)
{
	for (char c : aLine)
	{
		if (c != ' ' && c != '\t' && c != '\r')
			return false;
	}
	return true;
}

} // namespace

std::wstring DefaultLibraryPath()
{
	// Explicit override: portable mode and the UI gate both need a library that
	// is not the signed-in user's real one.
	wchar_t explicitPath[MAX_PATH] = {0};
	DWORD n = GetEnvironmentVariableW(L"AHKDECK_LIBRARY", explicitPath, MAX_PATH);
	if (n > 0 && n < MAX_PATH)
		return explicitPath;

	wchar_t base[MAX_PATH] = {0};
	n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return std::wstring();
	return std::wstring(base) + L"\\AHKDeck\\library.txt";
}

bool LoadScriptLibrary(const std::wstring &aPath, std::vector<std::wstring> &aOut, std::string &aError)
{
	aOut.clear();
	if (GetFileAttributesW(aPath.c_str()) == INVALID_FILE_ATTRIBUTES)
		return true; // first run: an absent library is a normal state

	TextDocument document;
	if (!ReadTextDocument(aPath, document, aError))
		return false;

	size_t begin = 0;
	while (begin <= document.utf8.size())
	{
		size_t end = document.utf8.find('\n', begin);
		if (end == std::string::npos)
			end = document.utf8.size();

		std::string line = document.utf8.substr(begin, end - begin);
		if (!IsBlank(line))
		{
			std::wstring path = WideOf(line);
			bool duplicate = false;
			for (const auto &existing : aOut)
			{
				if (SamePathI(existing, path))
				{
					duplicate = true;
					break;
				}
			}
			if (!duplicate)
				aOut.push_back(std::move(path));
		}

		if (end == document.utf8.size())
			break;
		begin = end + 1;
	}
	return true;
}

bool SaveScriptLibrary(const std::wstring &aPath, const std::vector<std::wstring> &aPaths, std::string &aError)
{
	size_t slash = aPath.find_last_of(L"\\/");
	if (slash != std::wstring::npos)
	{
		std::wstring directory = aPath.substr(0, slash);
		CreateDirectoryW(directory.c_str(), nullptr);
	}

	std::string content;
	for (const auto &path : aPaths)
	{
		if (path.empty())
			continue;
		content += Utf8Of(path);
		content += "\r\n";
	}
	return WriteTextDocument(aPath, content, TextEncoding::Utf8NoBom, true, nullptr, aError);
}

} // namespace ahkdeck
