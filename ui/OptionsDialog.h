// ui/OptionsDialog.h - 设置/选项… (m177, extended m179).
//
// The deck used to have no preferences window at all: every knob lived in
// settings.json and the only way to change one was to edit the file by hand.
// That was fine while the knobs were developer-only (m149 decided against a
// settings page). The temp retention window is not: 「30 天后我的文件去哪了」
// is a question a user has to be able to answer in the UI.
//
// m179 moved the official mirror choice (Gitee / GitHub) here from the two
// dialogs that used to carry their own picker. It is one global preference -
// the same value decides the script catalog URL, which mirror a script is
// downloaded from, the update manifest URL, and which release asset the
// updater takes - so it belongs in the one window that owns preferences, and
// the dialogs show it read-only.
//
// The window is deliberately one topic wide per section. It is the place
// future options go (one field row each), not a place to re-expose the View
// menu's switches - those stay where they can be toggled without a modal.
#pragma once

namespace ahkdeck
{
namespace ui
{

struct OptionsResult
{
	int days = 30;   // 临时文件保留时长, 0 meaning 永不
	int source = 0;  // 官方下载源: 0 = Gitee, 1 = GitHub
};

// Modal; returns when the window closes.
//   aCurrentDays / aCurrentSource - the stored values.
//   aOut - the chosen values, written only when the call returns true.
bool ShowOptionsDialog(int aCurrentDays, int aCurrentSource, bool aRounded,
	bool aShadow, OptionsResult &aOut);

} // namespace ui
} // namespace ahkdeck
