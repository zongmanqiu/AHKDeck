// m227: script groups - the data rules and the list plan, no window involved.

#include "core/Json.h"
#include "core/ScriptGroups.h"

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

using ahkdeck::Json;
using ahkdeck::ScriptGroups;
using ahkdeck::ViewRowKind;
using ahkdeck::ViewScriptRow;

std::string Kinds(const std::vector<ViewScriptRow> &aRows)
{
	std::string out;
	for (const ViewScriptRow &row : aRows)
	{
		if (!out.empty())
			out += ",";
		if (row.kind == ViewRowKind::Group)
			out += row.group < 0 ? "G*" : "G" + std::to_string(row.group);
		else
			out += "s" + std::to_string(row.script);
	}
	return out;
}

} // namespace

int main()
{
	const std::vector<std::string> keys = { "夜灯.ahk", "防休眠.ahk", "硬件信息.ahk", "文件哈希.ahk" };
	const std::vector<bool> all = { true, true, true, true };
	const std::vector<int> order = { 0, 1, 2, 3 };

	// --- empty settings: everything ungrouped, no group header at all -------
	{
		ScriptGroups groups;
		groups.Load(Json());
		Check(groups.Count() == 0, "no node -> no groups");
		Check(groups.GroupOf("夜灯.ahk") == ScriptGroups::kUngrouped,
			"unknown script is ungrouped");
		const auto rows = groups.BuildRows(keys, all, order, false);
		Check(Kinds(rows) == "G*,s0,s1,s2,s3", "flat list keeps 未分组 order");
	}

	// --- create / rename / duplicate --------------------------------------
	{
		ScriptGroups groups;
		std::string error;
		Check(groups.Create("系统维护", &error), "create group");
		Check(!groups.Create("系统维护", &error) && !error.empty(),
			"duplicate name is refused");
		Check(!groups.Create("   ", &error), "blank name is refused");
		Check(groups.Create("开发", &error), "second group");
		Check(groups.Find("开发") == 1, "find by name");
		Check(groups.Rename(1, "系统维护", &error) == false, "rename onto a taken name fails");
		Check(groups.Rename(1, "开发工具", &error), "rename to a fresh name");
		Check(groups.At(1).name == "开发工具", "rename sticks");
	}

	// --- membership: one group per script, move, dedupe --------------------
	{
		ScriptGroups groups;
		std::string error;
		groups.Create("系统维护", &error);
		groups.Create("开发", &error);
		groups.Move("夜灯.ahk", 0);
		groups.Move("夜灯.ahk", 1);
		Check(groups.GroupOf("夜灯.ahk") == 1, "move re-homes the script");
		Check(groups.At(0).members.empty(), "old group no longer holds it");
		groups.Move("夜灯.ahk", ScriptGroups::kUngrouped);
		Check(groups.GroupOf("夜灯.ahk") == ScriptGroups::kUngrouped,
			"move to 未分组 removes membership");
		groups.Move("夜灯.ahk", 0);
		groups.Move("夜灯.ahk", 0);
		Check(groups.At(0).members.size() == 1, "re-adding the same member does not duplicate");
		// Case-insensitive, like the file system.
		groups.Move("夜灯.AHK", 1);
		Check(groups.At(0).members.empty() && groups.GroupOf("夜灯.ahk") == 1,
			"membership compares case-insensitively");
	}

	// --- delete a group: members fall back, files untouched ---------------
	{
		ScriptGroups groups;
		std::string error;
		groups.Create("系统维护", &error);
		groups.Move("夜灯.ahk", 0);
		groups.Remove(0);
		Check(groups.Count() == 0, "group removed");
		Check(groups.GroupOf("夜灯.ahk") == ScriptGroups::kUngrouped,
			"members fall back to 未分组");
	}

	// --- rename / delete a script keeps the record honest ------------------
	{
		ScriptGroups groups;
		std::string error;
		groups.Create("系统维护", &error);
		groups.Move("夜灯.ahk", 0);
		Check(groups.RenameMember("夜灯.ahk", "小夜灯.ahk"), "member rename");
		Check(groups.GroupOf("小夜灯.ahk") == 0 && groups.GroupOf("夜灯.ahk") < 0,
			"membership follows the new name");
		groups.ForgetMember("小夜灯.ahk");
		Check(groups.At(0).members.empty(), "deleting the script forgets it");
	}

	// --- prune: a member that is gone from disk is dropped ----------------
	{
		ScriptGroups groups;
		std::string error;
		groups.Create("系统维护", &error);
		groups.Move("夜灯.ahk", 0);
		groups.Move("防休眠.ahk", 0);
		Check(groups.PruneMissing({ "夜灯.ahk" }) == 1, "one missing member is pruned");
		Check(groups.At(0).members.size() == 1, "the present member stays");
		Check(groups.PruneMissing({ "夜灯.ahk" }) == 0, "pruning is idempotent");
	}

	// --- the list plan ----------------------------------------------------
	{
		ScriptGroups groups;
		std::string error;
		groups.Create("系统维护", &error);
		groups.Create("开发", &error);
		groups.Move("夜灯.ahk", 0);      // index 0
		groups.Move("防休眠.ahk", 0);    // index 1
		groups.Move("硬件信息.ahk", 1);  // index 2
		// 文件哈希.ahk (3) stays ungrouped.
		const auto rows = groups.BuildRows(keys, all, order, false);
		Check(Kinds(rows) == "G0,s0,s1,G1,s2,G*,s3", "groups first, 未分组 last");

		// Collapse group 0: header stays, members go.
		groups.SetCollapsed(0, true);
		Check(Kinds(groups.BuildRows(keys, all, order, false)) == "G0,G1,s2,G*,s3",
			"collapsed group hides its members");

		// Collapse 未分组 too.
		groups.SetUngroupedCollapsed(true);
		Check(Kinds(groups.BuildRows(keys, all, order, false)) == "G0,G1,s2,G*",
			"collapsed 未分组 keeps its header and hides its members");

		// Search: flat, folded state ignored, only the matches.
		const std::vector<bool> some = { false, true, false, true };
		Check(Kinds(groups.BuildRows(keys, some, order, true)) == "s1,s3",
			"search is flat and ignores folding");
	}

	// --- an empty group still gets a header (otherwise it is unreachable) --
	{
		ScriptGroups groups;
		std::string error;
		groups.Create("空组", &error);
		Check(Kinds(groups.BuildRows(keys, all, order, false)) == "G0,G*,s0,s1,s2,s3",
			"empty group keeps its header");
	}

	// --- the caller's sort order is what the members follow ---------------
	{
		ScriptGroups groups;
		std::string error;
		groups.Create("系统维护", &error);
		groups.Move("夜灯.ahk", 0);
		groups.Move("防休眠.ahk", 0);
		const std::vector<int> reversed = { 1, 0, 3, 2 };
		Check(Kinds(groups.BuildRows(keys, all, reversed, false)) == "G0,s1,s0,G*,s3,s2",
			"members follow the caller's sorted order");
	}

	// --- save / load round trip (Chinese names, backslash keys) -----------
	{
		ScriptGroups groups;
		std::string error;
		groups.Create("系统维护", &error);
		groups.Create("开发", &error);
		groups.Move("夜灯.ahk", 0);
		groups.Move("硬件信息.ahk", 1);
		groups.SetCollapsed(1, true);
		groups.SetUngroupedCollapsed(true);

		Json node;
		// scripts.custom (the subscription sources) lives in the same node, so a
		// group save must add its keys without dropping the others.
		node.Set("custom", Json("keep-me"));
		groups.Save(node);
		Check(node["custom"].AsString() == "keep-me",
			"saving groups keeps the other keys of the scripts node");
		const std::string text = node.Dump();
		Json parsed;
		std::string parseError;
		Check(Json::Parse(text, parsed, parseError), "saved groups parse back");

		ScriptGroups again;
		again.Load(parsed);
		Check(again.Count() == 2, "round trip keeps both groups");
		Check(again.At(0).name == "系统维护" && again.At(1).name == "开发",
			"round trip keeps names in order");
		Check(again.GroupOf("夜灯.ahk") == 0 && again.GroupOf("硬件信息.ahk") == 1,
			"round trip keeps membership");
		Check(again.At(1).collapsed && again.UngroupedCollapsed(),
			"round trip keeps folded state");
	}

	// --- tolerant load: junk never costs the user their settings ----------
	{
		ScriptGroups groups;
		Json node = Json::MakeObject();
		node.Set("groups", Json("not-an-array"));
		groups.Load(node);
		Check(groups.Count() == 0, "bad groups node degrades to no groups");

		Json mixed = Json::MakeObject();
		Json list = Json::MakeArray();
		list.Push(Json("garbage"));
		Json good = Json::MakeObject();
		good.Set("name", Json("开发"));
		Json members = Json::MakeArray();
		members.Push(Json("夜灯.ahk"));
		members.Push(Json("夜灯.ahk"));   // duplicate
		members.Push(Json(""));          // empty
		members.Push(Json("硬件信息.ahk"));
		good.Set("members", members);
		list.Push(good);
		mixed.Set("groups", list);
		ScriptGroups tolerant;
		tolerant.Load(mixed);
		Check(tolerant.Count() == 1, "non-object group entries are skipped");
		Check(tolerant.At(0).members.size() == 2, "duplicate/blank members are dropped");
	}

	// --- keys normalise: '/' vs '\', leading separators -------------------
	{
		ScriptGroups groups;
		std::string error;
		groups.Create("开发", &error);
		groups.Move("工具/取数.ahk", 0);
		Check(groups.GroupOf("工具\\取数.ahk") == 0, "slash forms are the same key");
		Check(groups.GroupOf(".\\工具\\取数.ahk") == 0, "leading .\\ is ignored");
	}

	if (gFailures)
	{
		std::printf("\n%d failure(s)\n", gFailures);
		return 1;
	}
	std::printf("\nall group checks passed\n");
	return 0;
}
