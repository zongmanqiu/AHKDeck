// m018: the AI's file boundary. These are the cases that must never pass.

#include "core/AiPolicy.h"

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

} // namespace

int main()
{
	const std::wstring dir = L"policy_gate";
	const std::wstring root = L"policy_gate\\Scripts";
	CreateDirectoryW(dir.c_str(), nullptr);
	CreateDirectoryW(root.c_str(), nullptr);

	wchar_t fullRoot[MAX_PATH] = {0};
	GetFullPathNameW(root.c_str(), MAX_PATH, fullRoot, nullptr);
	const std::wstring rootFull(fullRoot);

	const std::wstring libraryFile = L"policy_gate\\kept.ahk";
	HANDLE created = CreateFileW(libraryFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (created != INVALID_HANDLE_VALUE)
		CloseHandle(created);

	ahkdeck::AiScriptPolicy policy;
	policy.roots.push_back(root);
	policy.files.push_back(libraryFile);

	std::wstring canonical;
	std::string error;

	Check(ahkdeck::ResolveAiScriptPath(policy, L"a.ahk", true, canonical, error), "relative name allowed");
	Check(canonical == rootFull + L"\\a.ahk", "relative name resolves under the root");

	Check(ahkdeck::ResolveAiScriptPath(policy, L"sub\\b.AHK", true, canonical, error), "subdir + uppercase extension");
	Check(ahkdeck::ResolveAiScriptPath(policy, rootFull + L"\\c.ahk", true, canonical, error), "absolute inside allowed");
	Check(ahkdeck::ResolveAiScriptPath(policy, libraryFile, true, canonical, error), "library file explicitly allowed");

	error.clear();
	Check(!ahkdeck::ResolveAiScriptPath(policy, L"..\\evil.ahk", true, canonical, error), "parent escape denied");
	Check(!error.empty(), "parent escape has a reason");

	Check(!ahkdeck::ResolveAiScriptPath(policy, L"C:\\Windows\\evil.ahk", true, canonical, error),
		"absolute outside denied");
	Check(!ahkdeck::ResolveAiScriptPath(policy, L"..\\..\\kept.ahk", true, canonical, error),
		"escape to a library file denied");

	Check(!ahkdeck::ResolveAiScriptPath(policy, L"notes.txt", true, canonical, error), "non-.ahk denied");
	Check(!ahkdeck::ResolveAiScriptPath(policy, L"evil.ahk.txt", true, canonical, error), "double extension denied");
	Check(!ahkdeck::ResolveAiScriptPath(policy, L"CON.ahk", true, canonical, error), "reserved device name denied");
	// Windows' GetFullPathNameW strips a trailing dot, so this either resolves to
	// "trailing.ahk" or is rejected - both are safe; it must never keep a trailing dot.
	{
		error.clear();
		const bool resolved = ahkdeck::ResolveAiScriptPath(policy, L"trailing.ahk.", true, canonical, error);
		const std::wstring suffix = L"trailing.ahk";
		const bool normalized = resolved && canonical.size() >= suffix.size()
			&& canonical.compare(canonical.size() - suffix.size(), suffix.size(), suffix) == 0;
		Check(!resolved || normalized, "trailing dot is normalized or denied");
	}
	Check(!ahkdeck::ResolveAiScriptPath(policy, L"stream.ahk:bad", true, canonical, error), "alternate stream denied");
	Check(!ahkdeck::ResolveAiScriptPath(policy, L"", true, canonical, error), "empty path denied");

	// Read shares the write rules.
	Check(ahkdeck::ResolveAiScriptPath(policy, L"a.ahk", false, canonical, error), "read inside allowed");
	Check(!ahkdeck::ResolveAiScriptPath(policy, L"..\\evil.ahk", false, canonical, error), "read escape denied");

	// No roots means no relative resolution at all.
	{
		ahkdeck::AiScriptPolicy empty;
		Check(!ahkdeck::ResolveAiScriptPath(empty, L"a.ahk", true, canonical, error), "no root -> relative denied");
	}

	DeleteFileW(libraryFile.c_str());
	RemoveDirectoryW(root.c_str());
	RemoveDirectoryW(dir.c_str());

	if (gFailures)
	{
		std::printf("aipolicy: %d failure(s)\n", gFailures);
		return 1;
	}
	std::printf("aipolicy: all checks passed\n");
	return 0;
}
