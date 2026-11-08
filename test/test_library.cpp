// m016: the script library must round-trip paths, tolerate a missing file and
// dedupe case/slash variants. cwd = .temp/library_gate (ctest).

#include "core/Library.h"
#include "core/TextFile.h"

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

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

} // namespace

int main()
{
	const std::wstring dir = L"library_gate";
	CreateDirectoryW(dir.c_str(), nullptr);
	const std::wstring path = dir + L"\\library.txt";

	// Round-trip with a case/slash duplicate.
	{
		std::vector<std::wstring> saved;
		saved.push_back(L"C:\\scripts\\a.ahk");
		saved.push_back(L"C:/scripts/B.ahk");
		saved.push_back(L"C:\\scripts\\A.AHK"); // same as #1 (case + slash)

		std::string error;
		Check(ahkdeck::SaveScriptLibrary(path, saved, error), "save library");
		Check(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES, "library file exists");

		std::vector<std::wstring> loaded;
		Check(ahkdeck::LoadScriptLibrary(path, loaded, error), "load library");
		Check(loaded.size() == 2, "duplicates collapsed to 2 entries");
		Check(loaded.size() > 0 && loaded[0] == L"C:\\scripts\\a.ahk", "order preserved (first)");
		Check(loaded.size() > 1 && loaded[1] == L"C:/scripts/B.ahk", "order preserved (second)");
	}

	// A missing file is the first run, not an error.
	{
		std::string error;
		std::vector<std::wstring> loaded;
		loaded.push_back(L"sentinel");
		Check(ahkdeck::LoadScriptLibrary(dir + L"\\missing.txt", loaded, error), "missing library is not an error");
		Check(loaded.empty(), "missing library yields an empty list");
	}

	// Blank lines and CRLF from a hand-edited file are tolerated.
	{
		std::string error;
		Check(ahkdeck::WriteTextDocument(dir + L"\\hand.txt", "\n  \nC:\\x\\y.ahk\n\nC:\\x\\y.ahk\n",
			ahkdeck::TextEncoding::Utf8NoBom, true, nullptr, error), "write hand-edited file");
		std::vector<std::wstring> loaded;
		Check(ahkdeck::LoadScriptLibrary(dir + L"\\hand.txt", loaded, error), "load hand-edited file");
		Check(loaded.size() == 1, "blank lines skipped, duplicate removed");
	}

	// Save creates the parent directory.
	{
		std::string error;
		std::vector<std::wstring> saved;
		saved.push_back(L"D:\\one.ahk");
		Check(ahkdeck::SaveScriptLibrary(dir + L"\\sub\\library.txt", saved, error), "save creates parent dir");
		Check(GetFileAttributesW((dir + L"\\sub\\library.txt").c_str()) != INVALID_FILE_ATTRIBUTES,
			"nested library file exists");
	}

	// Default path honors the test/portable override first.
	{
		SetEnvironmentVariableW(L"AHKDECK_LIBRARY", L"Z:\\explicit\\library.txt");
		Check(ahkdeck::DefaultLibraryPath() == L"Z:\\explicit\\library.txt", "AHKDECK_LIBRARY overrides the default path");
		SetEnvironmentVariableW(L"AHKDECK_LIBRARY", nullptr);

		std::wstring fallback = ahkdeck::DefaultLibraryPath();
		wchar_t self[MAX_PATH] = {0};
		DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
		std::wstring exeDir(self, n > 0 ? n : 0);
		const size_t cut = exeDir.find_last_of(L"\\/");
		if (cut != std::wstring::npos)
			exeDir.resize(cut);
		Check(fallback == exeDir + L"\\library.txt", "default path is portable (next to the exe)");
	}

	DeleteFileW(path.c_str());
	DeleteFileW((dir + L"\\hand.txt").c_str());
	DeleteFileW((dir + L"\\sub\\library.txt").c_str());
	RemoveDirectoryW((dir + L"\\sub").c_str());

	if (gFailures)
	{
		std::printf("library: %d failure(s)\n", gFailures);
		return 1;
	}
	std::printf("library: all checks passed\n");
	return 0;
}
