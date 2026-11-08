// m109: startup housekeeping for the three per-machine directories.
//
// Every launch the deck deletes files older than a retention window from
// .temp\ (MCP/validate scratch), Backups\ (pre-write script copies) and Logs\
// (session JSONL). The window is 30 days by default and lives in settings.json
// (housekeeping.max_age_days) - an advanced-user knob, never a UI control.
//
// The time rules are pure functions so the test suite can pin them without
// creating files: a file exactly N days old is KEPT ("older than" is strict),
// and 0 or a negative setting turns the cleanup off.

#pragma once

#include <cstdint>
#include <string>

namespace ahkdeck
{

class Json;

inline constexpr int kHousekeepingDefaultDays = 30;

// settings.json -> housekeeping.max_age_days. Missing or non-numeric = the
// default; 0 and negatives = cleanup disabled (the user asked for it).
int RetentionDaysFromJson(const Json &aRoot);

// FILETIME ticks are 100 ns since 1601-01-01. aDays <= 0 means "nothing expires".
std::uint64_t CutoffTicks(std::uint64_t aNowTicks, int aDays);

bool IsOlderThan(std::uint64_t aLastWriteTicks, std::uint64_t aCutoffTicks);

// Deletes files under aDir whose last write is older than aDays, then removes
// directories that became empty (aDir itself is kept). Junctions are never
// followed. Files that cannot be deleted (still open elsewhere) are counted in
// aError, not fatal. Returns the number of deleted files; a missing directory
// is normal and returns 0.
int PruneOlderThan(const std::wstring &aDir, int aDays, std::uint64_t aNowTicks,
	std::string &aError);

} // namespace ahkdeck
