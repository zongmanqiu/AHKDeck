// agent/McpServer - the MCP endpoint of the deck (m064).
//
// Shape, from the two clients we must satisfy (WorkBuddy and Qoder): both
// configure servers as {"mcpServers":{"<name>":{"command","args","env"}}} -
// there is no url field, so the client starts our process and talks to its
// stdin/stdout. That is the stdio transport, and in this product it means the
// process the client spawns IS the deck: one exe, one window, one kernel, so
// edit_editor can still reach the editing area the user is looking at and
// "宿主关闭 = 脚本全关" stays true (AGENTS 11a). A separate proxy process would
// have owned a second script world the user never sees.
//
// Protocol, per the 2025-06-18 specification: newline-delimited JSON-RPC 2.0;
// messages MUST NOT contain embedded newlines (so every string goes through the
// JSON escaper, which is core/Json's Dump()); stdout carries frames only and all
// logging goes to stderr. A server that offers tools must answer `tools/list`
// and `tools/call`; -32602 is the invalid-params error.
//
// Threading: this runs on its own thread and never touches FLTK. Everything the
// window must do goes through agent::Toolbox's sinks.

#pragma once

#include "agent/Toolbox.h"

#include <atomic>
#include <functional>
#include <string>

namespace ahkdeck
{
namespace agent
{
namespace mcp
{

// Blocks until stdin closes, aRunning goes false, or the handles die.
// aLog writes to stderr (never stdout - that is the protocol channel).
void ServeStdio(Toolbox &aToolbox, const std::atomic<bool> &aRunning,
	const std::function<void(const std::string &)> &aLog);

// The MCP dialect of the one tool list: Toolbox::Definitions() rendered as
// [{"name","description","inputSchema"}]. Split out so a gate can assert the
// count against Definitions() without running a server.
Json McpToolArray(const std::vector<ToolDef> &aDefs);

} // namespace mcp
} // namespace agent
} // namespace ahkdeck
