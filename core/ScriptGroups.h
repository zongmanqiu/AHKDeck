// core/ScriptGroups.h - m227: grouping is a view over the Scripts folder.
//
// The disk only answers "which scripts exist". Which group a script belongs to,
// what the groups are called, in what order they appear and whether they are
// folded live in settings.json. Folders on disk are deliberately NOT the
// grouping mechanism: a script owns `Scripts\<名>\` for its attachments and its
// own output, and moving a script while it runs would break its session (the
// row's session is keyed by path) and every A_ScriptDir-relative write.
//
// This class is pure data plus the list plan, so ctest can cover the rules
// without a window.
#pragma once

#include "core/Json.h"

#include <string>
#include <vector>

namespace ahkdeck
{

struct ScriptGroup
{
	std::string name;
	bool collapsed = false;
	std::vector<std::string> members; // keys relative to Scripts\, e.g. "夜灯.ahk"
};

enum class ViewRowKind
{
	Group,
	Script
};

// One line of the list. Group rows exist so the browser can draw a header
// without inventing a script; script rows carry the caller's own index.
struct ViewScriptRow
{
	ViewRowKind kind = ViewRowKind::Script;
	int group = -1;  // group index; -1 = 未分组 (Group rows only)
	int script = -1; // index into the caller's script list (Script rows only)
};

class ScriptGroups
{
public:
	static const int kUngrouped = -1;

	// Tolerant read: a missing/odd node means "everything ungrouped", never an
	// error - a settings file is the user's, and a broken group list must not
	// cost them their window geometry.
	void Load(const Json &aScriptsNode);
	void Save(Json &aScriptsNode) const;

	int Count() const { return (int)mGroups.size(); }
	const ScriptGroup &At(int aIndex) const { return mGroups[(size_t)aIndex]; }
	bool UngroupedCollapsed() const { return mUngroupedCollapsed; }
	void SetUngroupedCollapsed(bool aValue) { mUngroupedCollapsed = aValue; }

	int Find(const std::string &aName) const; // -1 when absent (case-insensitive)
	bool Create(const std::string &aName, std::string *aError);
	bool Rename(int aIndex, const std::string &aName, std::string *aError);
	void Remove(int aIndex); // members fall back to 未分组; no file is touched
	void SetCollapsed(int aIndex, bool aValue);

	int GroupOf(const std::string &aKey) const; // -1 = 未分组
	void Move(const std::string &aKey, int aGroup);
	bool RenameMember(const std::string &aOld, const std::string &aNew);
	void ForgetMember(const std::string &aKey);

	// Drops members that are no longer on disk. Returns how many were dropped,
	// so the caller knows whether the settings file needs a save.
	int PruneMissing(const std::vector<std::string> &aKeysOnDisk);

	// The list plan. aOrder is the caller's already-sorted script indices;
	// groups and 未分组 keep their own order, members follow aOrder.
	// aFiltering = the search box is non-empty: the plan is flat (no headers,
	// every matched script, folded groups ignored) because a search must never
	// hide a hit inside a folded group.
	std::vector<ViewScriptRow> BuildRows(const std::vector<std::string> &aKeys,
		const std::vector<bool> &aMatched, const std::vector<int> &aOrder,
		bool aFiltering) const;

private:
	std::vector<ScriptGroup> mGroups;
	bool mUngroupedCollapsed = false;
};

} // namespace ahkdeck
