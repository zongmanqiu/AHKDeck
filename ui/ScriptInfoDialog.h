// ui/ScriptInfoDialog.h - the read-only "what is in this file" window (m151).
//
// Lifted out of ScriptDownloadDialog.cpp, where it was a private class that
// could only show manifest rows. Two callers now share the same chrome:
//   * 脚本下载's 「!」 button, fed from scripts.json (remote metadata);
//   * the script list's row menu > 信息, fed from the local file's header.
// Keeping one dialog means the two views cannot drift in layout, and adding a
// field is a one-line change at the call site instead of a second layout.
//
// Layout rules that the callers rely on:
//   * a field whose value is empty is NOT drawn - a script without a header
//     would otherwise produce eleven rows of 「（无）」;
//   * groups are separated by a hairline, so header facts and file facts stay
//     visibly different kinds of information;
//   * values may wrap (description / entry lines run to thirty-odd characters).
#pragma once

#include <string>
#include <vector>

namespace ahkdeck
{
namespace ui
{

struct InfoField
{
	std::string label;
	std::string value;
	bool wrap = false;       // long prose: wrap instead of clipping
	bool selectable = false; // a path: a borderless Fl_Output the user can copy
};

using InfoGroup = std::vector<InfoField>;

// Modal; returns when the window is closed. Empty fields are dropped, empty
// groups are dropped too (no stray divider at the top or bottom).
void ShowInfoDialog(const char *aTitle, const std::vector<InfoGroup> &aGroups,
	bool aRounded = true, bool aShadow = true);

} // namespace ui
} // namespace ahkdeck
