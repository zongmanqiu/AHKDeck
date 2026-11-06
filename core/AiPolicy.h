// Which paths the AI may touch (m018).
//
// This is the entire security boundary for AI file access: the agent exposes no
// generic file API, and every script tool funnels its path through here. Roots
// are directories (the deck's Scripts folder); files are the explicit entries
// of the user's library. Canonicalisation uses GetFullPathNameW, so ".." and
// 8.3 names cannot escape a root.

#pragma once

#include <string>
#include <vector>

namespace ahkdeck
{

struct AiScriptPolicy
{
	std::vector<std::wstring> roots; // directories the AI may create/edit scripts in
	std::vector<std::wstring> files; // exact script files the user added to the library
};

// aRequested may be relative (resolved against roots[0]) or absolute. When
// aForWrite is true the path must be a legal .ahk file name; when false it only
// has to resolve inside the allowlist. Returns the canonical full path in
// aCanonical on success; aError is a short reason (never a machine path unless
// it is the requested one).
bool ResolveAiScriptPath(const AiScriptPolicy &aPolicy, const std::wstring &aRequested,
	bool aForWrite, std::wstring &aCanonical, std::string &aError);

// True when aPath (canonical) is one of the policy's exact files or inside a root.
bool IsInsideAiPolicy(const AiScriptPolicy &aPolicy, const std::wstring &aCanonical);

} // namespace ahkdeck
