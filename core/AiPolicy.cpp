#include "core/AiPolicy.h"

#include <windows.h>

#include <cwchar>
#include <cwctype>

namespace ahkdeck
{
namespace
{

std::wstring NormalizeSlashes(std::wstring aPath)
{
	for (auto &c : aPath)
	{
		if (c == L'/')
			c = L'\\';
	}
	return aPath;
}

std::wstring Lowercase(std::wstring aText)
{
	for (auto &c : aText)
		c = (wchar_t)towlower(c);
	return aText;
}

bool Canonicalize(const std::wstring &aPath, std::wstring &aOut, std::string &aError)
{
	std::wstring buffer(512, L'\0');
	DWORD written = GetFullPathNameW(aPath.c_str(), (DWORD)buffer.size(), buffer.data(), nullptr);
	if (written == 0)
	{
		aError = "the path could not be resolved";
		return false;
	}
	if (written >= buffer.size())
	{
		buffer.assign(written + 1, L'\0');
		written = GetFullPathNameW(aPath.c_str(), (DWORD)buffer.size(), buffer.data(), nullptr);
		if (written == 0 || written >= buffer.size())
		{
			aError = "the path could not be resolved";
			return false;
		}
	}
	buffer.resize(written);
	aOut = NormalizeSlashes(std::move(buffer));
	return true;
}

bool IsReservedName(const std::wstring &aStem)
{
	static const wchar_t *reserved[] = {
		L"CON", L"PRN", L"AUX", L"NUL",
		L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
		L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"
	};
	const std::wstring upper = [&]()
	{
		std::wstring text = aStem;
		for (auto &c : text)
			c = (wchar_t)towupper(c);
		return text;
	}();
	for (const wchar_t *name : reserved)
	{
		if (upper == name)
			return true;
	}
	return false;
}

bool ValidateLeaf(const std::wstring &aCanonical, std::string &aError)
{
	const size_t slash = aCanonical.find_last_of(L'\\');
	const std::wstring leaf = slash == std::wstring::npos ? aCanonical : aCanonical.substr(slash + 1);
	if (leaf.empty())
	{
		aError = "the path has no file name";
		return false;
	}

	const size_t dot = leaf.find_last_of(L'.');
	if (dot == std::wstring::npos || _wcsicmp(leaf.c_str() + dot, L".ahk") != 0)
	{
		aError = "only .ahk scripts are allowed";
		return false;
	}

	for (wchar_t c : leaf)
	{
		if (c < 0x20 || std::wcschr(L"<>:\"|?*", c) != nullptr)
		{
			aError = "the file name contains an invalid character";
			return false;
		}
	}
	if (leaf.back() == L'.' || leaf.back() == L' ')
	{
		aError = "the file name ends with a dot or space";
		return false;
	}
	if (IsReservedName(leaf.substr(0, dot)))
	{
		aError = "the file name is a reserved device name";
		return false;
	}
	return true;
}

bool EndsWithRoot(const std::wstring &aLowerPath, const std::wstring &aLowerRoot)
{
	if (aLowerRoot.empty())
		return false;
	if (aLowerPath == aLowerRoot)
		return true;
	if (aLowerPath.size() <= aLowerRoot.size())
		return false;
	if (aLowerPath.compare(0, aLowerRoot.size(), aLowerRoot) != 0)
		return false;
	return aLowerPath[aLowerRoot.size()] == L'\\';
}

// m160: a canonical path can still smuggle a request out of the whitelist
// through a junction or a symlink, and GetFullPathNameW never resolves those.
// Walk the path component by component and reject any level that carries a
// reparse point. A missing component ends the walk: it has no reparse point,
// and the later open/create fails on its own. The caller rejects UNC and
// device paths before this, so only "X:\..." forms arrive here; the root
// itself is host-configured and trusted.
bool CrossesReparsePoint(const std::wstring &aCanonical, std::string &aError)
{
	if (aCanonical.size() < 3 || aCanonical[1] != L':')
		return false;
	const size_t prefix = 3; // keep "X:\" and start at the first component
	size_t level = prefix;
	while (level < aCanonical.size())
	{
		const size_t slash = aCanonical.find(L'\\', level);
		const std::wstring part = aCanonical.substr(0,
			slash == std::wstring::npos ? aCanonical.size() : slash);
		const DWORD attrs = GetFileAttributesW(part.c_str());
		if (attrs == INVALID_FILE_ATTRIBUTES)
			return false; // nothing there yet; no reparse point to cross
		if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
		{
			aError = "the path crosses a reparse point (junction or symlink)";
			return true;
		}
		if (slash == std::wstring::npos)
			break;
		level = slash + 1;
	}
	return false;
}

} // namespace

bool IsInsideAiPolicy(const AiScriptPolicy &aPolicy, const std::wstring &aCanonical)
{
	const std::wstring lowerPath = Lowercase(NormalizeSlashes(aCanonical));

	for (const auto &root : aPolicy.roots)
	{
		std::wstring canonicalRoot;
		std::string ignored;
		if (Canonicalize(root, canonicalRoot, ignored) && EndsWithRoot(lowerPath, Lowercase(canonicalRoot)))
			return true;
	}
	for (const auto &file : aPolicy.files)
	{
		std::wstring canonicalFile;
		std::string ignored;
		if (Canonicalize(file, canonicalFile, ignored) && lowerPath == Lowercase(canonicalFile))
			return true;
	}
	return false;
}

bool ResolveAiScriptPath(const AiScriptPolicy &aPolicy, const std::wstring &aRequested,
	bool aForWrite, std::wstring &aCanonical, std::string &aError)
{
	aCanonical.clear();
	if (aRequested.empty())
	{
		aError = "the path is empty";
		return false;
	}
	if (aRequested.find(L'\0') != std::wstring::npos)
	{
		aError = "the path contains a NUL character";
		return false;
	}

	std::wstring path = NormalizeSlashes(aRequested);
	// m160: UNC and \\?\ device paths are out of scope for the AI tools - they
	// only ever address local script ids. Reject them before root joining so
	// the denial names the real reason instead of a whitelist mismatch, and so
	// no network share or device-path syntax can sidestep the checks below.
	if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\')
	{
		aError = "UNC paths are not allowed";
		return false;
	}
	const bool absolute = path.size() >= 2 && path[1] == L':';
	if (!absolute && path[0] != L'\\')
	{
		if (aPolicy.roots.empty())
		{
			aError = "no script folder is configured";
			return false;
		}
		path = NormalizeSlashes(aPolicy.roots[0]) + L"\\" + path;
	}

	if (!Canonicalize(path, aCanonical, aError))
		return false;
	if (CrossesReparsePoint(aCanonical, aError))
	{
		aCanonical.clear();
		return false;
	}
	if (!IsInsideAiPolicy(aPolicy, aCanonical))
	{
		aError = "the path is outside the allowed script area";
		aCanonical.clear();
		return false;
	}
	if (!ValidateLeaf(aCanonical, aError))
	{
		aCanonical.clear();
		return false;
	}
	(void)aForWrite; // reads and writes share the same .ahk-only rule
	return true;
}

} // namespace ahkdeck
