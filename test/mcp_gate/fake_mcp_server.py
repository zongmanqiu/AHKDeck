"""A deliberately wrong MCP server, for mcp_client.py --selftest.

The gate's red-proof (m064): the client must catch every one of these faults,
otherwise a green run says nothing about the real endpoint. What is wrong here
is exactly what our own server is asserted against:

  * initialize answers a different protocolVersion than the client asked for
  * serverInfo.name is not "ahkdeck"
  * capabilities has no "tools" member
  * ping is answered with an error
  * tools/list reports two tools, one of them renamed
  * an unknown method is answered with a result instead of -32601
  * tools/call returns an empty result with no content blocks

Usage: python fake_mcp_server.py --mcp-stdio   (stdio, newline-delimited)
"""

from __future__ import annotations

import json
import sys

# m072: never leave __pycache__ in the source tree (AGENTS.md rule 2).
sys.dont_write_bytecode = True

TOOLS = [
    {"name": "list_scripts", "description": "wrong", "inputSchema": {"type": "object"}},
    {"name": "read_script", "description": "wrong", "inputSchema": {"type": "object"}},
    # Listed on purpose: the client has a cell asserting write_script is NOT
    # exposed. Without it that cell passes on an empty list and proves nothing.
    {"name": "write_script", "description": "wrong", "inputSchema": {"type": "object"}},
]

# Payloads per tool. list_scripts answers with an absolute path because the
# client asserts the tools never show one; a fake that returns nothing would let
# that cell pass without measuring it either.
TOOL_RESULT = {
    "list_scripts": {"ok": True, "scripts": [{"id": "S1", "name": "C:\\users\\x\\evil.ahk"}]},
    "edit_editor": {"ok": True},
    # Quoted with a drive-letter path and without the script name, so the two
    # cells that police rule 9 have something to catch.
    "run_editor": {
        "ok": True,
        "state": "finished",
        "rc": 2,
        "output": '{"file":"C:\\\\users\\\\x\\\\.build\\\\.temp\\\\wrong.ahk","message":"space"}',
    },
    "write_script": {"ok": True, "script": "S2"},
}


def main() -> int:
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            request = json.loads(line)
        except json.JSONDecodeError:
            continue
        method = request.get("method") or ""
        reply_id = request.get("id")
        if reply_id is None:  # notification: fine to ignore
            continue

        if method == "initialize":
            result = {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "serverInfo": {"name": "not-ahkdeck", "version": "0"},
            }
        elif method == "tools/list":
            result = {"tools": TOOLS}
        elif method == "tools/call":
            name = ((request.get("params") or {}).get("name")) or ""
            text = json.dumps(TOOL_RESULT.get(name, {"ok": True}), ensure_ascii=False)
            # ok:true even for an unknown tool: the client must notice isError is
            # missing, not get to fail for free.
            result = {"content": [{"type": "text", "text": text}]}
        else:
            result = {}  # unknown method answered instead of -32601

        frame = {"jsonrpc": "2.0", "id": reply_id, "result": result}
        sys.stdout.write(json.dumps(frame, ensure_ascii=False) + "\n")
        sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
