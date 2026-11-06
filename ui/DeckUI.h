// deck UI - the FLTK shell (m014).
//
// The UI is a consumer of the engine's session interface (m011): it lists
// scripts, starts / stops sessions and renders the records that PopRecord()
// already attributed and wrote to each session's JSONL. No routing or logging
// logic lives here - a second copy would be a second truth.
//
// Direction: ui depends on engine and core; engine never includes ui.

#pragma once

#include <windows.h>

namespace ahkdeck
{
namespace ui
{

// Runs the deck window and its event loop. Returns the process exit code
// (0 on a normal close). The engine is loaded lazily by the window, so a
// missing kernel shows up in the console panel instead of failing to start.
//
// aServeMcpStdio comes only from "AHKDeck.exe --mcp-stdio" (m064): the window
// additionally speaks MCP on stdin/stdout, so an external agent's tool calls
// land in this same deck - same editing area, same sessions, same lifetime.
int Run(HINSTANCE aInstance, bool aServeMcpStdio = false);

} // namespace ui
} // namespace ahkdeck
