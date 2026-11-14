// core/AppPaths.h - the ONE place the deck writes at run time (m177).
//
// Before m177 the deck scattered its scratch across four siblings of the exe:
// .temp\ (validation reports, compile captures, MCP trial runs), Backups\
// (pre-write copies), Logs\ (session JSONL) and, during an update,
// ahkdeck_backup\. A user opening the folder saw five names and had no way to
// tell which of them were disposable.
//
// Everything now lives under one root:
//
//   <exe>\temp\logs\      session JSONL (one file per run)
//   <exe>\temp\backups\   pre-write script copies, update rollback copies
//   <exe>\temp\work\      validation reports, compile captures, trial runs
//   <exe>\temp\deleted\   scripts deleted from the list (recoverable by hand)
//
// The retention pass (core/Housekeeping.h) walks <exe>\temp once and does not
// care which subdirectory a file sits in: one rule, one place to look. A
// subdirectory that the pass empties is simply created again on next use.
//
// Nothing here migrates the pre-m177 directories: this is a test-phase product
// and the old names are not supported any more (user decision, 2026-11-14).
#pragma once

#include <string>

namespace ahkdeck
{

// Subdirectory names under <exe>\temp. Spelling them once is what keeps the
// writer (this file's callers) and the cleaner (the retention walk) agreed.
inline constexpr const wchar_t *kTempLogs = L"logs";
inline constexpr const wchar_t *kTempBackups = L"backups";
inline constexpr const wchar_t *kTempWork = L"work";
inline constexpr const wchar_t *kTempDeleted = L"deleted";

// Directory of the running executable, without a trailing separator. Empty
// when the module path cannot be resolved (never expected in practice).
std::wstring AppDirectory();

// <aAppDir>\temp\<aSub>. aAppDir may end in a separator. When aCreate is true
// the leaf directory is created (parents included, existing ones are fine).
std::wstring TempDirUnder(const std::wstring &aAppDir, const wchar_t *aSub,
	bool aCreate = true);

// <exe>\temp, created on demand.
std::wstring AppTempRoot(bool aCreate = true);

// <exe>\temp\<aSub>, created on demand.
std::wstring AppTempDir(const wchar_t *aSub, bool aCreate = true);

// The path as it is SHOWN (rule 9: relative, never the machine's absolute
// path): "temp\logs\<leaf>". aSub may be empty for a file directly in temp\.
std::string AppTempRelative(const wchar_t *aSub, const std::string &aLeafUtf8);

// Creates temp\ and the four subdirectories. Called once at startup so the
// folder is there before anything tries to write into it, and so a user can
// see what the deck keeps before the first run produces anything.
void EnsureAppTempLayout();

} // namespace ahkdeck
