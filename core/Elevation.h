// core/Elevation.h - is this process running as administrator (m239).
//
// Windows fixes a process's integrity level at create time: a running process
// cannot elevate itself. The only way to gain admin is to start a NEW elevated
// process (ShellExecute "runas" -> UAC) and exit the current one. So 管理员权限
// is a "set the intent, apply on next launch" preference, never an in-place
// toggle. The relaunch itself lives in ui::Run (it needs the loaded config and
// the MCP-mode flag); this header only answers "are we already elevated?".
#pragma once

namespace ahkdeck
{

// True when the current process token is elevated (full administrator). Drives
// the title-bar 「(管理员权限)」 marker - which must reflect the REAL state, not
// the saved intent, so a window that was checked but not yet restarted is not
// mislabelled as admin.
bool IsElevated();

} // namespace ahkdeck
