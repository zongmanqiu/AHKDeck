// core/ScriptGroups.cpp - see core/ScriptGroups.h.
#include "core/ScriptGroups.h"

#include <algorithm>
#include <cctype>

namespace ahkdeck
{
namespace
{

// Keys are relative to Scripts\ and stored with '\'. Accept '/' too, because a
// hand-edited settings file or a script written on another platform may carry
// the other separator; normalise once, compare everywhere.
std::string NormalizeKey(const std::string &aKey)
{
	std::string out;
	out.reserve(aKey.size());
	size_t start = 0;
	while (start < aKey.size() && (aKey[start] == '\\' || aKey[start] == '/'
		|| aKey[start] == '.'))
	{
		// Strip leading separators and a leading "./" only; a file called
		// ".hidden.ahk" keeps its dot because the next char is not a separator.
		if (aKey[start] == '.')
		{
			const size_t next = start + 1;
			if (next < aKey.size() && (aKey[next] == '\\' || aKey[next] == '/'))
			{
				start = next + 1;
				continue;
			}
			break;
		}
		++start;
	}
	for (size_t i = start; i < aKey.size(); ++i)
	{
		const char c = aKey[i] == '/' ? '\\' : aKey[i];
		out.push_back(c);
	}
	while (!out.empty() && (out.back() == '\\' || out.back() == ' '))
		out.pop_back();
	return out;
}

char LowerAscii(char aChar)
{
	if (aChar >= 'A' && aChar <= 'Z')
		return (char)(aChar - 'A' + 'a');
	return aChar;
}

// Windows file names are case-insensitive, so group membership is too.
bool SameKey(const std::string &aLeft, const std::string &aRight)
{
	if (aLeft.size() != aRight.size())
		return false;
	for (size_t i = 0; i < aLeft.size(); ++i)
		if (LowerAscii(aLeft[i]) != LowerAscii(aRight[i]))
			return false;
	return true;
}

std::string TrimName(const std::string &aName)
{
	size_t begin = 0;
	size_t end = aName.size();
	while (begin < end && (unsigned char)aName[begin] <= ' ')
		++begin;
	while (end > begin && (unsigned char)aName[end - 1] <= ' ')
		--end;
	return aName.substr(begin, end - begin);
}

} // namespace

void ScriptGroups::Load(const Json &aScriptsNode)
{
	mGroups.clear();
	mUngroupedCollapsed = false;
	if (!aScriptsNode.IsObject())
		return;

	mUngroupedCollapsed = aScriptsNode["ungrouped_collapsed"].AsBool(false);
	const Json &groups = aScriptsNode["groups"];
	if (!groups.IsArray())
		return;

	for (size_t i = 0; i < groups.Size(); ++i)
	{
		const Json &node = groups[i];
		if (!node.IsObject())
			continue;
		ScriptGroup def;
		def.name = TrimName(node["name"].AsString());
		if (def.name.empty() || Find(def.name) >= 0)
			continue;
		def.collapsed = node["collapsed"].AsBool(false);

		const Json &members = node["members"];
		if (members.IsArray())
		{
			for (size_t j = 0; j < members.Size(); ++j)
			{
				const std::string key = NormalizeKey(members[j].AsString());
				if (key.empty() || GroupOf(key) >= 0) // one script, one group
					continue;
				// The group above is not in mGroups yet, so its own list is
				// checked separately - a hand-edited file can repeat a member.
				bool duplicate = false;
				for (const std::string &existing : def.members)
					if (SameKey(existing, key))
					{
						duplicate = true;
						break;
					}
				if (duplicate)
					continue;
				def.members.push_back(key);
			}
		}
		mGroups.push_back(std::move(def));
	}
}

void ScriptGroups::Save(Json &aScriptsNode) const
{
	// Only this class's own keys are written: the node also carries
	// `custom` (subscription sources), and replacing it whole would delete the
	// user's subscriptions.
	aScriptsNode.Set("ungrouped_collapsed", Json(mUngroupedCollapsed));
	Json groups = Json::MakeArray();
	for (const ScriptGroup &group : mGroups)
	{
		Json item = Json::MakeObject();
		item.Set("name", Json(group.name));
		item.Set("collapsed", Json(group.collapsed));
		Json members = Json::MakeArray();
		for (const std::string &member : group.members)
			members.Push(Json(member));
		item.Set("members", members);
		groups.Push(item);
	}
	aScriptsNode.Set("groups", groups);
}

int ScriptGroups::Find(const std::string &aName) const
{
	for (size_t i = 0; i < mGroups.size(); ++i)
		if (SameKey(mGroups[i].name, aName))
			return (int)i;
	return -1;
}

bool ScriptGroups::Create(const std::string &aName, std::string *aError)
{
	const std::string name = TrimName(aName);
	if (name.empty())
	{
		if (aError)
			*aError = "分组名不能为空";
		return false;
	}
	if (Find(name) >= 0)
	{
		if (aError)
			*aError = "已经有同名分组";
		return false;
	}
	ScriptGroup def;
	def.name = name;
	mGroups.push_back(std::move(def));
	return true;
}

bool ScriptGroups::Rename(int aIndex, const std::string &aName, std::string *aError)
{
	if (aIndex < 0 || (size_t)aIndex >= mGroups.size())
		return false;
	const std::string name = TrimName(aName);
	if (name.empty())
	{
		if (aError)
			*aError = "分组名不能为空";
		return false;
	}
	const int existing = Find(name);
	if (existing >= 0 && existing != aIndex)
	{
		if (aError)
			*aError = "已经有同名分组";
		return false;
	}
	mGroups[(size_t)aIndex].name = name;
	return true;
}

void ScriptGroups::Remove(int aIndex)
{
	if (aIndex < 0 || (size_t)aIndex >= mGroups.size())
		return;
	// Members simply stop being members: the files are untouched, and the next
	// BuildRows puts them back under 未分组.
	mGroups.erase(mGroups.begin() + aIndex);
}

void ScriptGroups::SetCollapsed(int aIndex, bool aValue)
{
	if (aIndex < 0 || (size_t)aIndex >= mGroups.size())
		return;
	mGroups[(size_t)aIndex].collapsed = aValue;
}

int ScriptGroups::GroupOf(const std::string &aKey) const
{
	const std::string key = NormalizeKey(aKey);
	if (key.empty())
		return kUngrouped;
	for (size_t g = 0; g < mGroups.size(); ++g)
		for (const std::string &member : mGroups[g].members)
			if (SameKey(member, key))
				return (int)g;
	return kUngrouped;
}

void ScriptGroups::Move(const std::string &aKey, int aGroup)
{
	const std::string key = NormalizeKey(aKey);
	if (key.empty())
		return;
	for (ScriptGroup &group : mGroups)
		group.members.erase(std::remove_if(group.members.begin(), group.members.end(),
			[&](const std::string &m) { return SameKey(m, key); }), group.members.end());
	if (aGroup < 0 || (size_t)aGroup >= mGroups.size())
		return; // "移动到未分组" is exactly the removal above
	mGroups[(size_t)aGroup].members.push_back(key);
}

bool ScriptGroups::RenameMember(const std::string &aOld, const std::string &aNew)
{
	const std::string oldKey = NormalizeKey(aOld);
	const std::string newKey = NormalizeKey(aNew);
	if (oldKey.empty() || newKey.empty() || SameKey(oldKey, newKey))
		return false;
	for (ScriptGroup &group : mGroups)
		for (std::string &member : group.members)
			if (SameKey(member, oldKey))
			{
				member = newKey;
				return true;
			}
	return false;
}

void ScriptGroups::ForgetMember(const std::string &aKey)
{
	const std::string key = NormalizeKey(aKey);
	if (key.empty())
		return;
	for (ScriptGroup &group : mGroups)
		group.members.erase(std::remove_if(group.members.begin(), group.members.end(),
			[&](const std::string &m) { return SameKey(m, key); }), group.members.end());
}

int ScriptGroups::PruneMissing(const std::vector<std::string> &aKeysOnDisk)
{
	int dropped = 0;
	for (ScriptGroup &group : mGroups)
	{
		const size_t before = group.members.size();
		group.members.erase(std::remove_if(group.members.begin(), group.members.end(),
			[&](const std::string &m) {
				for (const std::string &key : aKeysOnDisk)
					if (SameKey(m, key))
						return false;
				return true;
			}), group.members.end());
		dropped += (int)(before - group.members.size());
	}
	return dropped;
}

std::vector<ViewScriptRow> ScriptGroups::BuildRows(const std::vector<std::string> &aKeys,
	const std::vector<bool> &aMatched, const std::vector<int> &aOrder,
	bool aFiltering) const
{
	std::vector<ViewScriptRow> rows;
	rows.reserve(aOrder.size() + mGroups.size() + 1);

	const auto matched = [&](int aIndex) {
		return aIndex >= 0 && (size_t)aIndex < aKeys.size()
			&& (size_t)aIndex < aMatched.size() && aMatched[(size_t)aIndex];
	};

	// Search: flat, folded state ignored. A hit inside a folded group must show.
	if (aFiltering)
	{
		for (const int index : aOrder)
			if (matched(index))
				rows.push_back({ViewRowKind::Script, kUngrouped, index});
		return rows;
	}

	for (int g = 0; g < Count(); ++g)
	{
		// The header is drawn even when the group is empty or nothing matched:
		// otherwise a freshly created (or emptied) group is unreachable in the
		// list and cannot be renamed, deleted or filled.
		rows.push_back({ViewRowKind::Group, g, -1});
		if (mGroups[(size_t)g].collapsed)
			continue;
		for (const int index : aOrder)
			if (matched(index) && GroupOf(aKeys[(size_t)index]) == g)
				rows.push_back({ViewRowKind::Script, g, index});
	}

	bool anyUngrouped = false;
	for (const int index : aOrder)
		if (matched(index) && GroupOf(aKeys[(size_t)index]) == kUngrouped)
		{
			anyUngrouped = true;
			break;
		}
	if (!anyUngrouped)
		return rows;

	rows.push_back({ViewRowKind::Group, kUngrouped, -1});
	if (mUngroupedCollapsed)
		return rows;
	for (const int index : aOrder)
		if (matched(index) && GroupOf(aKeys[(size_t)index]) == kUngrouped)
			rows.push_back({ViewRowKind::Script, kUngrouped, index});
	return rows;
}

} // namespace ahkdeck
