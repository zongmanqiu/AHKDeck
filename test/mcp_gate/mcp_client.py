"""Minimal MCP stdio client for the AHKDeck gate (m064).

Why a hand-rolled client: the gate must prove OUR endpoint, not prove that some
other client works. It speaks the transport the spec defines - newline-delimited
JSON-RPC 2.0 on stdin/stdout - and nothing else.

The child's stderr goes to a file, never a pipe: an MCP server logs to stderr,
and a full stderr pipe blocks the server inside WriteFile. The AHK harnesses we
read document that deadlock, so the harness avoids the shape.

Usage:
    python mcp_client.py <exe-path> <work-dir>      # the real gate
    python mcp_client.py --selftest <work-dir>      # must go RED, see below

--selftest runs the same probe against test/fake_mcp_server.py, which answers
with a stale protocolVersion, two tools and no -32601. A client that stays green
there is not measuring anything, so a red run is the pass condition here.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

# m072: importing this file as a module must not leave __pycache__ in the source
# tree (AGENTS.md rule 2); the source_tree gate now enforces it.
sys.dont_write_bytecode = True

EXPECTED_TOOLS = {
    "list_scripts",
    "read_script",
    "edit_editor",
    "check_editor",
    "validate_editor",
    "patch_editor",
    "run_editor",
    "run_script",
    "get_session_output",
    "stop_script",
    "list_sessions",
    "stop_all",
}

# m101: the legal write_script success path needs a file inside Scripts; the
# gate removes it (and the backups it causes) when it is done.
WRITE_PROBE = "mcp-gate-write-probe.ahk"

# v1 comma syntax on purpose: this is the exact class of mistake m059/m060 kept
# chasing. The gate's point is that the loop SEES the failure through the
# endpoint, not that we claim it does.
BROKEN = "#Requires AutoHotkey v2.0\nMsgBox, 123\n"
FIXED = (
    "#Requires AutoHotkey v2.0\n"
    'FileAppend "mcp-gate-hello`n", A_ScriptDir "\\mcp-gate.txt"\n'
)
# The heartbeat shape (FileAppend into A_ScriptDir, then Sleep), which the
# session gates already prove stays running and stops on request. A one-shot
# `-50` timer was the first try here and it exited rc=0 for real: in v2 a
# one-shot timer is no reason to stay alive, so that fixture measured nothing.
RESIDENT = (
    "#Requires AutoHotkey v2.0\n"
    "Loop 40\n"
    "{\n"
    '    FileAppend "tick`n", A_ScriptDir "\\mcp-gate-resident.txt"\n'
    "    Sleep 200\n"
    "}\n"
)

RESULTS: list[tuple[str, str, str]] = []


def cell(name: str, ok: bool, detail: str = "") -> None:
    RESULTS.append((name, "PASS" if ok else "FAIL", detail))


class McpStdio:
    def __init__(self, cmd: list[str], work: Path, env: dict | None = None) -> None:
        self.stderr_path = work / "mcp-stderr.log"
        self.stderr = open(self.stderr_path, "wb")
        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.stderr,
            bufsize=0,
            env=env,
        )
        self.next_id = 0
        self.deadline = time.time() + 90

    def _send(self, payload: dict) -> None:
        assert self.proc.stdin is not None
        data = json.dumps(payload, ensure_ascii=False) + "\n"
        self.proc.stdin.write(data.encode("utf-8"))
        self.proc.stdin.flush()

    def _read_line(self) -> str:
        assert self.proc.stdout is not None
        while time.time() < self.deadline:
            raw = self.proc.stdout.readline()
            if not raw:
                raise RuntimeError("server closed stdout")
            line = raw.decode("utf-8", "replace").strip()
            if line:
                return line
        raise TimeoutError("no response from the MCP endpoint in 90 s")

    def request(self, method: str, params: dict | None = None) -> dict:
        self.next_id += 1
        payload = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            payload["params"] = params
        self._send(payload)
        return json.loads(self._read_line())

    def notify(self, method: str, params: dict | None = None) -> None:
        payload = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            payload["params"] = params
        self._send(payload)

    def tool(self, name: str, arguments: dict) -> tuple[dict, bool]:
        """Return (parsed tool payload, isError)."""
        reply = self.request("tools/call", {"name": name, "arguments": arguments})
        if "error" in reply:
            return ({"error": reply["error"]["message"]}, True)
        result = reply.get("result") or {}
        blocks = result.get("content") or []
        text = "".join(b.get("text", "") for b in blocks if b.get("type") == "text")
        try:
            payload = json.loads(text)
        except json.JSONDecodeError:
            payload = {"raw": text}
        return (payload, bool(result.get("isError")))

    def close(self) -> None:
        try:
            if self.proc.poll() is None:
                self.proc.terminate()
                self.proc.wait(timeout=15)
        except Exception:  # noqa: BLE001 - shutdown is best-effort in a gate
            pass
        self.stderr.close()


def probe(client: McpStdio, resident_file: Path, exe_dir: Path,
          autosave_probe: Path | None = None) -> None:
    # --- 1. handshake -------------------------------------------------------
    reply = client.request(
        "initialize",
        {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "ahkdeck-gate", "version": "1"},
        },
    )
    result = reply.get("result") or {}
    info = result.get("serverInfo") or {}
    cell("initialize echoes protocolVersion",
         result.get("protocolVersion") == "2025-06-18", str(result.get("protocolVersion")))
    cell("serverInfo.name", info.get("name") == "ahkdeck", str(info))
    cell("capabilities.tools present",
         isinstance(result.get("capabilities", {}).get("tools"), dict), str(result)[:160])

    client.notify("notifications/initialized")
    ping = client.request("ping")
    cell("ping answered", "result" in ping, json.dumps(ping, ensure_ascii=False)[:120])

    # --- 2. tools/list must equal the one tool truth ----------------------
    listed = client.request("tools/list")
    tools = (listed.get("result") or {}).get("tools") or []
    names = {t.get("name") for t in tools}
    cell("tools/list count == 12", len(tools) == len(EXPECTED_TOOLS),
         f"got {len(tools)}: {sorted(names)}")
    cell("tools/list names exact", names == EXPECTED_TOOLS,
         f"missing={sorted(EXPECTED_TOOLS - names)} extra={sorted(names - EXPECTED_TOOLS)}")
    cell("write_script stays off the list", "write_script" not in names)
    schemas = {t.get("name"): (t.get("inputSchema") or {}) for t in tools}
    cell("inputSchema required present",
         schemas.get("edit_editor", {}).get("required") == ["content"],
         json.dumps(schemas.get("edit_editor", {}), ensure_ascii=False)[:160])
    # A property described as a bare string is not a schema. Qoder tolerated it;
    # a strict client cannot tell the model what to put in the field, and the
    # cell above would still have been green.
    edit_props = schemas.get("edit_editor", {}).get("properties") or {}
    cell("properties are JSON Schema objects",
         isinstance(edit_props.get("content"), dict)
         and edit_props["content"].get("type") == "string",
         json.dumps(edit_props, ensure_ascii=False)[:200])
    run_props = schemas.get("run_editor", {}).get("properties") or {}
    cell("numeric argument declared numeric, not string",
         isinstance(run_props.get("timeout_ms"), dict)
         and run_props["timeout_ms"].get("type") == "integer",
         json.dumps(run_props, ensure_ascii=False)[:200])
    # m101: the descriptions carry the calling order, not just the function.
    # A model that guesses ids or stops a resident script before reading its
    # output wastes a session; the text is the only place that can prevent it.
    descs = {t.get("name"): str(t.get("description") or "") for t in tools}
    cell("list_scripts description carries the id-first rule",
         "id" in descs.get("list_scripts", "") and "list_scripts" in descs.get("list_scripts", ""),
         descs.get("list_scripts", "")[:160])
    cell("run_script description points at list_scripts",
         "list_scripts" in descs.get("run_script", ""), descs.get("run_script", "")[:160])
    cell("get_session_output description points at stop_script",
         "stop_script" in descs.get("get_session_output", ""),
         descs.get("get_session_output", "")[:160])
    cell("edit_editor description says it does not touch disk",
         "不落盘" in descs.get("edit_editor", ""), descs.get("edit_editor", "")[:160])
    out_props = schemas.get("get_session_output", {}).get("properties") or {}
    cell("max_bytes is an optional integer",
         isinstance(out_props.get("max_bytes"), dict)
         and out_props["max_bytes"].get("type") == "integer"
         and "max_bytes" not in (schemas.get("get_session_output", {}).get("required") or []),
         json.dumps(out_props, ensure_ascii=False)[:200])
    cell("after_offset is an optional integer",
         isinstance(out_props.get("after_offset"), dict)
         and out_props["after_offset"].get("type") == "integer"
         and "after_offset" not in (schemas.get("get_session_output", {}).get("required") or []),
         json.dumps(out_props, ensure_ascii=False)[:200])
    cell("session tools take no required arguments",
         (schemas.get("list_sessions", {}).get("required") or []) == []
         and (schemas.get("stop_all", {}).get("required") or []) == [],
         json.dumps(schemas.get("list_sessions", {}), ensure_ascii=False)[:160])
    re_props = schemas.get("run_editor", {}).get("properties") or {}
    cell("args is an optional array of strings",
         isinstance(re_props.get("args"), dict)
         and re_props["args"].get("type") == "array"
         and (re_props["args"].get("items") or {}).get("type") == "string"
         and "args" not in (schemas.get("run_editor", {}).get("required") or []),
         json.dumps(re_props, ensure_ascii=False)[:200])
    cell("check_editor takes no required arguments",
         "check_editor" in names
         and (schemas.get("check_editor", {}).get("required") or []) == [],
         json.dumps(schemas.get("check_editor", {}), ensure_ascii=False)[:160])
    cell("validate_editor takes no required arguments",
         "validate_editor" in names
         and (schemas.get("validate_editor", {}).get("required") or []) == [],
         json.dumps(schemas.get("validate_editor", {}), ensure_ascii=False)[:160])
    cell("validate_editor description says it only parses",
         "不执行" in descs.get("validate_editor", ""),
         descs.get("validate_editor", "")[:160])
    patch_edits = (schemas.get("patch_editor", {}).get("properties") or {}).get("edits") or {}
    patch_items = patch_edits.get("items") or {}
    cell("patch_editor edits are objects with an op",
         "patch_editor" in names
         and patch_edits.get("type") == "array"
         and ((patch_items.get("properties") or {}).get("op") or {}).get("type") == "string"
         and patch_items.get("required") == ["op"],
         json.dumps(patch_edits, ensure_ascii=False)[:200])

    # --- 3. protocol errors -----------------------------------------------
    bad = client.request("tools/nope")
    cell("unknown method -> -32601",
         (bad.get("error") or {}).get("code") == -32601, json.dumps(bad)[:160])
    unknown_tool, is_err = client.tool("no_such_tool", {})
    cell("unknown tool -> isError", is_err and "未知工具" in str(unknown_tool.get("error")),
         json.dumps(unknown_tool, ensure_ascii=False)[:160])

    # --- 3.5 an oversize frame must not break the frame stream (m083/B1) ---
    # The endpoint drops a frame bigger than 8 MB. Dropping it must consume
    # the WHOLE frame: if the leftovers are parsed as frames of their own,
    # every frame boundary after that is wrong - the session answers with a
    # burst of -32700 until the client reconnects. So the first frame after the
    # oversize one must be the ping's own answer.
    oversize_ok = True
    oversize_detail = ""
    try:
        assert client.proc.stdin is not None
        client.proc.stdin.write(b"x" * (9 * 1024 * 1024) + b"\n")
        client.proc.stdin.flush()
    except (BrokenPipeError, OSError) as exc:
        oversize_ok = False
        oversize_detail = f"the endpoint died on the oversize frame: {exc}"
    if oversize_ok:
        client.next_id += 1
        client._send({"jsonrpc": "2.0", "id": client.next_id, "method": "ping"})
        raw = client._read_line()
        try:
            reply = json.loads(raw)
        except json.JSONDecodeError:
            reply = {}
        oversize_ok = "result" in reply and "error" not in reply
        oversize_detail = raw[:180]
    cell("ping after an oversize frame answers ping", oversize_ok, oversize_detail)

    # --- 4. read the deck's own state -------------------------------------
    scripts, err = client.tool("list_scripts", {})
    entries = scripts.get("scripts") or []
    cell("list_scripts ok", not err and scripts.get("ok") is True, str(scripts)[:200])
    cell("list_scripts sees the shipped Scripts folder", len(entries) >= 1,
         f"{len(entries)} scripts: {[e.get('name') for e in entries][:6]}")
    cell("no machine path in tool output",
         all("\\" not in str(e.get("name", "")) for e in entries))

    # --- 5. the closed loop the user asked for ----------------------------
    client.tool("edit_editor", {"content": BROKEN})
    run1, _ = client.tool("run_editor", {"timeout_ms": 6000})
    broken_session = run1.get("session")
    saw_failure = (run1.get("rc") not in (None, 0)) or run1.get("fatal") is True
    cell("run_editor reports the broken script", saw_failure,
         f"state={run1.get('state')} rc={run1.get('rc')} fatal={run1.get('fatal')}")
    out1 = str(run1.get("output", ""))
    # The v2 loader's own wording ("... require a space or \"(\""), so this cell
    # can only pass by really reading the kernel's message.
    cell("the error text reaches the caller", "space" in out1.lower(), out1[:180].replace("\n", " | "))
    # Rule 9 over the wire. Qoder's first real run quoted
    # "file":"E:\\...\\.build\\.temp\\ai-editor.ahk" to the model; the JSONL on
    # disk keeps the kernel's wording (m010 proved it byte-for-byte), the tool
    # payload must not.
    leaks = [m.group(0) for m in re.finditer(r"[A-Za-z]:[\\/][^\"\\s]{0,60}", out1)]
    cell("no drive-letter path in run output", not leaks, str(leaks)[:200])
    cell("the failing file is still named",
         "AHKDeck-preview.ahk" in out1 or "mcp-autosave-probe.ahk" in out1,
         out1[:200].replace("\n", " | "))
    # m062: a run copy in the script's own folder means A_ScriptDir matches what
    # the user gets after saving; the tool must say which kind of copy it used.
    cell("run_editor names its run copy",
         run1.get("script_dir") in ("real", "sibling", "temp"),
         f"script_dir={run1.get('script_dir')}")
    cell("a broken script is a fail verdict", run1.get("verdict") == "fail",
         f"verdict={run1.get('verdict')}")
    # m062: errors are objects, not prose - the line number must be a field.
    errs = run1.get("errors") or []
    cell("errors carry the failing line as a number",
         bool(errs) and isinstance(errs[0].get("line"), int) and errs[0]["line"] >= 1,
         json.dumps(errs[:2], ensure_ascii=False)[:220])
    # m062 rule 9: every field we add is a new place to leak a machine path. The
    # kernel names the file as an absolute path, so the tool has to fold it to a
    # leaf before it reaches a caller (the same rule the older output cell uses).
    efiles = [str(e.get("file", "")) for e in errs]
    cell("errors carry no machine path",
         all(":" not in f and "\\" not in f and "/" not in f for f in efiles),
         str(efiles)[:200])

    client.tool("edit_editor", {"content": FIXED})
    run2, _ = client.tool("run_editor", {"timeout_ms": 8000})
    cell("fixed script passes", run2.get("rc") == 0 and not run2.get("fatal"),
         f"state={run2.get('state')} rc={run2.get('rc')} output={str(run2.get('output'))[:120]}")
    cell("a fixed script is a pass verdict", run2.get("verdict") == "pass",
         f"verdict={run2.get('verdict')}")

    # --- 5.5 a script with chinese text (m083) ---------------------------
    # A Chinese script is the normal thing to edit here. Replacing the editing
    # area with UTF-8 text used to trip FLTK's own consistency check
    # ("FI_Tex_Displayposition_to_line: Consitency check pt failed"), which
    # pops a modal dialog on the user's screen and wedges the endpoint. If
    # that comes back, the next tool call never answers and the gate hangs.
    client.tool("edit_editor", {"content": "#Requires AutoHotkey v2.0\n; 中文注释：你好，世界\n"})
    cjk, _ = client.tool("run_editor", {"timeout_ms": 6000})
    cell("a script with chinese text runs", cjk.get("rc") in (None, 0),
         f"rc={cjk.get('rc')} state={cjk.get('state')}")

    # --- 5.6 a write tool says what it changed (m101) ---------------------
    # "bytes" alone cannot tell a caller what moved; the prefix/suffix summary
    # is what lets it re-read one line instead of the whole script.
    base = "#Requires AutoHotkey v2.0\n; line 2\n; line 3\n"
    first, _ = client.tool("edit_editor", {"content": base})
    cell("edit_editor returns the change summary",
         first.get("changed") is True and first.get("lines_after") == 3,
         json.dumps(first, ensure_ascii=False)[:220])
    mod = base.replace("; line 2", "; line 2 changed")
    second, _ = client.tool("edit_editor", {"content": mod})
    cell("edit_editor reports the line that changed",
         second.get("changed") is True and second.get("lines_before") == 3
         and second.get("first_changed_line") == 2 and second.get("last_changed_line") == 2,
         json.dumps(second, ensure_ascii=False)[:220])
    again, _ = client.tool("edit_editor", {"content": mod})
    cell("an identical edit reports changed=false",
         again.get("changed") is False and again.get("first_changed_line") == 0
         and again.get("last_changed_line") == 0,
         json.dumps(again, ensure_ascii=False)[:220])

    # --- 5.7 reading a session has an explicit cut and an explicit size ---
    # The caller decides how much of the log it can afford; truncated tells it
    # whether there is older content it has not seen.
    if broken_session:
        small, _ = client.tool("get_session_output",
                               {"session": int(broken_session), "max_bytes": 128})
        cell("max_bytes caps the tail at 128 bytes",
             small.get("truncated") is True and small.get("bytes") == 128,
             f"truncated={small.get('truncated')} bytes={small.get('bytes')}")
        full, _ = client.tool("get_session_output",
                              {"session": int(broken_session), "max_bytes": 65536})
        cell("a full read reports truncated=false",
             full.get("truncated") is False and (full.get("bytes") or 0) > 0,
             f"truncated={full.get('truncated')} bytes={full.get('bytes')}")
        default, _ = client.tool("get_session_output", {"session": int(broken_session)})
        cell("the default read still returns the error text",
             default.get("truncated") is False
             and "space" in str(default.get("output", "")).lower(),
             str(default.get("output", ""))[:120].replace("\n", " | "))
    else:
        cell("get_session_output had a session to read", False, "run_editor returned no session")

    # --- 5.8 run tools take script arguments (m102) ----------------------
    # A_Args is the only place a script can see them, so the probe writes what
    # it received next to the temp script and the gate reads it back.
    args_script = (
        "#Requires AutoHotkey v2.0\n"
        'FileAppend(A_Args[1] "`n" A_Args.Length "`n", A_ScriptDir "\\mcp-gate-args.txt")\n'
    )
    client.tool("edit_editor", {"content": args_script})
    ran, _ = client.tool("run_editor",
                         {"timeout_ms": 6000, "args": ["hello", "two words"]})
    args_probe = Path(exe_dir) / "Scripts" / "mcp-gate-args.txt"
    got = ""
    if args_probe.exists():
        got = args_probe.read_text(encoding="utf-8", errors="replace").strip()
    cell("run_editor passes args through A_Args",
         ran.get("state") == "finished" and got == "hello\n2",
         f"state={ran.get('state')} probe={got!r}")
    bad, _ = client.tool("run_editor", {"args": "not-an-array"})
    cell("non-array args are refused",
         bad.get("ok") is False and "args" in str(bad.get("error", "")),
         json.dumps(bad, ensure_ascii=False)[:160])
    if args_probe.exists():
        args_probe.unlink()

    # --- 5.85 a script's own stdout reaches the caller (m061) -------------
    # A GUI host has no stdout handle; the kernel hands FileAppend's "*" target
    # to the deck as a JSON record instead. Without this the text was invisible.
    stdout_script = (
        "#Requires AutoHotkey v2.0\n"
        'FileAppend("gate-stdout`n", "*")\n'
    )
    client.tool("edit_editor", {"content": stdout_script})
    spoke, _ = client.tool("run_editor", {"timeout_ms": 6000})
    payload = str(spoke.get("output", ""))
    cell("FileAppend to stdout becomes a stdout record",
         spoke.get("verdict") == "pass" and '"kind":"stdout"' in payload
         and "gate-stdout" in payload,
         payload[:220].replace("\n", " | "))

    # --- 5.86 an offset cursor reads only what is new (m106) --------------
    # Tail-reading the same finished session twice sends the same bytes twice;
    # the cursor is the fix, and it must survive a log that shrank under it.
    spoke_id = int(spoke["session"])
    whole, _ = client.tool("get_session_output", {"session": spoke_id, "max_bytes": 65536})
    cell("an offset read reports where to continue",
         whole.get("from_offset") == 0
         and whole.get("next_offset") == whole.get("total_bytes")
         and int(whole.get("total_bytes", 0)) > 0,
         f"next={whole.get('next_offset')} total={whole.get('total_bytes')}")
    nothing, _ = client.tool("get_session_output", {
        "session": spoke_id, "after_offset": int(whole.get("next_offset", 0))})
    cell("a second cursor read has nothing new",
         nothing.get("bytes") == 0 and nothing.get("output") == ""
         and nothing.get("rewound") is False,
         f"bytes={nothing.get('bytes')} rewound={nothing.get('rewound')}")
    rewound, _ = client.tool("get_session_output", {
        "session": spoke_id, "after_offset": 100000000})
    cell("a cursor past the end rewinds instead of failing",
         rewound.get("rewound") is True and rewound.get("from_offset") == 0
         and str(rewound.get("output", "")) != "",
         f"rewound={rewound.get('rewound')} from={rewound.get('from_offset')}")

    # --- 5.87 a long-lived session rotates its log (m106) -----------------
    # The gate runs the child with a 64 KB cap (env). 1200 stdout records are
    # well past it, so the live file must have a .1 next to it.
    loud = "#Requires AutoHotkey v2.0\nLoop 1200\n{\n    FileAppend \"ab`n\", \"*\"\n}\n"
    client.tool("edit_editor", {"content": loud})
    loud_run, _ = client.tool("run_editor", {"timeout_ms": 20000})
    loud_out, _ = client.tool("get_session_output", {
        "session": int(loud_run["session"]), "max_bytes": 65536})
    cell("a full log is rotated to .1",
         loud_run.get("verdict") == "pass" and loud_out.get("log_rotated") is True,
         f"verdict={loud_run.get('verdict')} rotated={loud_out.get('log_rotated')} "
         f"total={loud_out.get('total_bytes')}")

    # --- 5.9 static check catches v1 syntax before a run (m102) -----------
    # The first section of this gate already proves a v1 command fails at run
    # time; check_editor is worth having only if it says so without running.
    v1_mixed = "#Requires AutoHotkey v2.0\n#NoEnv\nMsgBox, 123\nx := %A_ScriptDir%\n"
    client.tool("edit_editor", {"content": v1_mixed})
    lint, lint_err = client.tool("check_editor", {})
    findings = lint.get("findings") or []
    rules = [f.get("rule") for f in findings]
    lines = [f.get("line") for f in findings]
    cell("check_editor reports v1-only and v1-command",
         lint_err and lint.get("ok") is False
         and "v1-only" in rules and "v1-command" in rules
         and 2 in lines and 3 in lines,
         json.dumps(lint, ensure_ascii=False)[:220])
    cell("check_editor flags v1 %var% as a warning",
         "v1-percent" in rules and (lint.get("counts") or {}).get("warning", 0) >= 1,
         json.dumps(lint.get("counts"), ensure_ascii=False))
    clean_v2 = '#Requires AutoHotkey v2.0\nMsgBox("hello")\nx := 5 % 3\n'
    client.tool("edit_editor", {"content": clean_v2})
    clean, clean_err = client.tool("check_editor", {})
    cell("a clean v2 script has no findings",
         not clean_err and clean.get("ok") is True
         and (clean.get("counts") or {}).get("total") == 0,
         json.dumps(clean, ensure_ascii=False)[:200])

    # --- 5.10 targeted patches keep the rest of the script intact (m102)
    # The probe script reports `b` to a file, so the gate can tell an applied
    # patch from one that only claimed to be applied.
    patch_base = (
        "#Requires AutoHotkey v2.0\n"
        "b := 2\n"
        'FileAppend(b "`n", A_ScriptDir "\\mcp-gate-patch.txt")\n'
    )
    patch_probe = Path(exe_dir) / "Scripts" / "mcp-gate-patch.txt"
    if patch_probe.exists():
        patch_probe.unlink()
    client.tool("edit_editor", {"content": patch_base})
    patched, p_err = client.tool("patch_editor", {"edits": [
        {"op": "replace", "old_text": "b := 2", "new_text": "b := 22"}]})
    client.tool("run_editor", {"timeout_ms": 6000})
    value = patch_probe.read_text(encoding="utf-8", errors="replace").strip() \
        if patch_probe.exists() else ""
    cell("patch_editor applies a unique replace",
         not p_err and patched.get("applied") is True and patched.get("changed") is True
         and patched.get("first_changed_line") == 2 and value == "22",
         f"applied={patched.get('applied')} value={value!r}")

    if patch_probe.exists():
        patch_probe.unlink()
    previewed, _ = client.tool("patch_editor", {"preview": True, "edits": [
        {"op": "replace", "old_text": "b := 22", "new_text": "b := 999"}]})
    client.tool("run_editor", {"timeout_ms": 6000})
    value2 = patch_probe.read_text(encoding="utf-8", errors="replace").strip() \
        if patch_probe.exists() else ""
    cell("preview leaves the editor unchanged",
         previewed.get("applied") is False and previewed.get("changed") is True
         and value2 == "22",
         f"applied={previewed.get('applied')} value={value2!r}")

    ambiguous, amb_err = client.tool("patch_editor", {"edits": [
        {"op": "replace", "old_text": "b", "new_text": "c"}]})
    cell("an ambiguous old_text is refused",
         amb_err and "唯一" in str(ambiguous.get("error", "")),
         json.dumps(ambiguous, ensure_ascii=False)[:160])

    marker2 = Path(exe_dir) / "Scripts" / "mcp-gate-patch2.txt"
    if marker2.exists():
        marker2.unlink()
    inserted, i_err = client.tool("patch_editor", {"edits": [
        {"op": "insert_after", "line": 1,
         "new_text": 'FileAppend("inserted`n", A_ScriptDir "\\mcp-gate-patch2.txt")'}]})
    client.tool("run_editor", {"timeout_ms": 6000})
    cell("insert_after adds an executable line",
         not i_err and marker2.exists(), f"exists={marker2.exists()}")

    # --- 5.11 validate-only pass asks the kernel itself (m103) ------------
    # check_editor reads text; validate_editor runs the kernel's /validate mode.
    # The fixture would create a file if it ever executed, so "validate ran
    # nothing" is a fact about the file, not about the tool's wording.
    validated = Path(exe_dir) / "Scripts" / "mcp-gate-validated.txt"
    if validated.exists():
        validated.unlink()
    proof = (
        "#Requires AutoHotkey v2.0\n"
        'FileAppend("ran`n", A_ScriptDir "\\mcp-gate-validated.txt")\n'
    )
    client.tool("edit_editor", {"content": proof})
    good_v, good_v_err = client.tool("validate_editor", {})
    cell("validate_editor accepts a parseable script",
         not good_v_err and good_v.get("ok") is True and good_v.get("rc") == 0,
         json.dumps(good_v, ensure_ascii=False)[:220])
    cell("validate_editor executes nothing", not validated.exists(),
         f"marker exists={validated.exists()}")

    client.tool("edit_editor", {"content": BROKEN})
    bad_v, bad_v_err = client.tool("validate_editor", {})
    vf = bad_v.get("findings") or []
    vlines = [f.get("line") for f in vf]
    cell("validate_editor reports rc=2 and the failing line",
         bad_v_err and bad_v.get("ok") is False and bad_v.get("rc") == 2 and 2 in vlines,
         json.dumps(bad_v, ensure_ascii=False)[:260])
    cell("validate findings keep the kernel's own words",
         any("comma" in str(f.get("message", "")).lower()
             or "MsgBox, 123" in str(f.get("extra", "")) for f in vf),
         json.dumps(vf, ensure_ascii=False)[:240])
    # Rule 9 over this path: the report names a file, never a machine path.
    vfiles = [str(f.get("file", "")) for f in vf]
    cell("validate findings carry no machine path",
         all(":" not in f and "\\" not in f for f in vfiles),
         json.dumps(vfiles, ensure_ascii=False)[:200])

    # --- 6. the policy boundary still holds over the endpoint -------------
    denied, err = client.tool("write_script", {"name": "..\\evil.ahk", "content": FIXED})
    cell("'..\\evil.ahk' refused", err and bool(denied.get("error")),
         json.dumps(denied, ensure_ascii=False)[:160])

    # The success path of the same tool: a file inside Scripts is legal, and it
    # must carry the same change summary as edit_editor (m101). The probe file
    # and the backups it creates are removed by main().
    created, c_err = client.tool("write_script", {"name": WRITE_PROBE, "content": base})
    cell("write_script creates a file and reports changed=true",
         not c_err and created.get("ok") is True and created.get("changed") is True
         and created.get("lines_after") == 3,
         json.dumps(created, ensure_ascii=False)[:220])
    edited, e_err = client.tool("write_script", {"name": WRITE_PROBE, "content": mod})
    cell("write_script reports the line it changed",
         not e_err and edited.get("changed") is True
         and edited.get("first_changed_line") == 2 and edited.get("last_changed_line") == 2,
         json.dumps(edited, ensure_ascii=False)[:220])
    same, s_err = client.tool("write_script", {"name": WRITE_PROBE, "content": mod})
    cell("an identical write reports changed=false",
         not s_err and same.get("changed") is False and same.get("lines_before") == 3,
         json.dumps(same, ensure_ascii=False)[:220])

    # --- 7. a resident script must not look like a crash ------------------
    client.tool("edit_editor", {"content": RESIDENT})
    run3, _ = client.tool("run_editor", {"timeout_ms": 2000})
    cell("resident script returns state=running with no rc",
         run3.get("state") == "running" and run3.get("rc") is None,
         f"state={run3.get('state')} rc={run3.get('rc')} fatal={run3.get('fatal')}")
    cell("a resident script is a resident verdict, not a timeout",
         run3.get("verdict") == "resident", f"verdict={run3.get('verdict')}")
    # "running" is only worth something if the script actually executed, so read
    # the ticks it wrote while we were timing out.
    ticks = 0
    if resident_file.exists():
        try:
            ticks = len(resident_file.read_text(encoding="utf-8", errors="replace").splitlines())
        except OSError:
            ticks = 0
    cell("the resident script really ticked", ticks >= 1, f"{ticks} tick line(s)")
    session = run3.get("session")
    if session:
        stopped, err = client.tool("stop_script", {"session": int(session)})
        cell("stop_script stops it", not err and stopped.get("stopped") is True, str(stopped)[:120])
        after, _ = client.tool("get_session_output", {"session": int(session)})
        cell("session readable after stop", after.get("ok") is True, str(after)[:160])

    # --- 7.4 a re-run stops the previous copy of the same script (m062) ---
    # Two live copies of one script fight over the same hotkeys and window, so
    # run_editor has to end the old one before it starts the new one. The id
    # must change (a new thread) and the old entry must no longer be running.
    client.tool("edit_editor", {"content": RESIDENT})
    first, _ = client.tool("run_editor", {"timeout_ms": 1500})
    second, _ = client.tool("run_editor", {"timeout_ms": 1500})
    cell("a re-run uses a new session",
         first.get("session") is not None and first.get("session") != second.get("session"),
         f"first={first.get('session')} second={second.get('session')}")
    old_session = first.get("session")
    if old_session:
        old, _ = client.tool("get_session_output", {"session": int(old_session)})
        cell("a re-run stops the previous copy", old.get("state") != "running",
             f"old state={old.get('state')}")

    # --- 7.45 the session table and its one-stop button (m106) -----------
    # An AI that lost an id must be able to ask for the table instead of
    # guessing, and must be able to end everything in one call.
    listed, _ = client.tool("list_sessions", {})
    all_sessions = listed.get("sessions") or []
    live = [s for s in all_sessions if s.get("session") == second.get("session")]
    cell("list_sessions sees the live session",
         len(live) == 1 and live[0].get("state") == "running",
         f"count={len(all_sessions)} mine={live}")
    name = str(live[0].get("name", "")) if live else ""
    # The engine stores the display name (stem, no extension), so the cell only
    # pins rule 9: whatever the name is, it must not carry a path.
    cell("list_sessions names the script by leaf",
         name != "" and "\\" not in name and "/" not in name and ":" not in name,
         f"name={name!r}")
    stopped, _ = client.tool("stop_all", {})
    cell("stop_all ends every live session",
         int(stopped.get("stopped", 0)) >= 1 and stopped.get("still_running") == 0,
         f"stopped={stopped.get('stopped')} still={stopped.get('still_running')}")
    after, _ = client.tool("get_session_output",
                           {"session": int(second.get("session", 0) or 0)})
    cell("stop_all leaves the entry finished", after.get("state") == "finished",
         f"state={after.get('state')}")

    # --- 7.5 cancellation (m081) -------------------------------------------
    # The client gives up on an in-flight run. The endpoint must answer
    # promptly instead of serving the whole timeout, and the NEXT run must
    # still wait its full budget - a cancel must not latch.
    client.next_id += 1
    cancel_id = client.next_id
    cancel_sent_at = time.time()
    client._send({"jsonrpc": "2.0", "id": cancel_id, "method": "tools/call",
                  "params": {"name": "run_editor", "arguments": {"timeout_ms": 20000}}})
    time.sleep(1.0)
    client.notify("notifications/cancelled", {"requestId": cancel_id})
    cancelled_reply = json.loads(client._read_line())
    cancelled_elapsed = time.time() - cancel_sent_at
    cblocks = ((cancelled_reply.get("result") or {}).get("content") or [])
    ctext = "".join(b.get("text", "") for b in cblocks if b.get("type") == "text")
    try:
        cpayload = json.loads(ctext)
    except json.JSONDecodeError:
        cpayload = {}
    cell("cancelled run answers before its timeout", cancelled_elapsed < 8.0,
         f"{cancelled_elapsed:.1f}s")
    cell("cancelled run reports state=running", cpayload.get("state") == "running",
         str(cpayload)[:140])
    if cpayload.get("session"):
        client.tool("stop_script", {"session": int(cpayload["session"])})

    # The cancel must not latch onto later runs. RESIDENT lives ~8 s, so a
    # 12 s budget only returns early if the wait loop is broken.
    latch_sent_at = time.time()
    run5, _ = client.tool("run_editor", {"timeout_ms": 12000})
    latch_elapsed = time.time() - latch_sent_at
    cell("cancel does not latch onto later runs", latch_elapsed >= 6.0,
         f"{latch_elapsed:.1f}s state={run5.get('state')} rc={run5.get('rc')}")
    if run5.get("session"):
        client.tool("stop_script", {"session": int(run5["session"])})

    # --- 7.6 a script that cannot answer is a timeout (m062) --------------
    # Resident means "still running and answering Windows messages". Measured
    # fact: AHK v2's own Loop{} pumps messages, so a tight loop stays resident
    # (the first fixture here asserted the opposite and was wrong). Only a
    # thread stuck inside a blocking call cannot answer, so the fixture blocks
    # in DllCall("Sleep") past the run budget.
    blocked = (
        "#Requires AutoHotkey v2.0\n"
        'DllCall("Sleep", "UInt", 2500)\n'
    )
    client.tool("edit_editor", {"content": blocked})
    stuck, _ = client.tool("run_editor", {"timeout_ms": 1500})
    cell("a blocked script is a timeout verdict", stuck.get("verdict") == "timeout",
         f"verdict={stuck.get('verdict')} state={stuck.get('state')}")
    if stuck.get("session"):
        client.tool("stop_script", {"session": int(stuck["session"])})


    # --- 8. the OTHER half of the redaction -------------------------------
    # The load error above only proves the "file" field collapses to a name.
    # A path inside the kernel's free text ("#Include file ... not found") has
    # to lose the exe prefix instead. Half-enforced redaction reads like full
    # redaction, and Qoder's follow-up run called that gap out on its own.
    deep = (
        "#Requires AutoHotkey v2.0\n"
        "#Include " + str(exe_dir) + "\\missing-mcp-include.ahk\n"
    )
    client.tool("edit_editor", {"content": deep})
    run4, _ = client.tool("run_editor", {"timeout_ms": 6000})
    out4 = str(run4.get("output", ""))
    cell("include failure reaches the caller", "include" in out4.lower(),
         out4[:220].replace("\n", " | "))
    cell("exe prefix folded to <exe>", "<exe>" in out4, out4[:220].replace("\n", " | "))
    leaks4 = [m.group(0) for m in re.finditer(r"[A-Za-z]:[\\/][^\"\\s]{0,60}", out4)]
    cell("no drive-letter path in include error", not leaks4, str(leaks4)[:200])

    # --- 9. tool edits must not ride the timed save (m082) -----------------
    # edit_editor promises "not saved". The timed save used to break that
    # promise behind the user's back. The deck runs with AHKDECK_AUTOSAVE_MS=600
    # and a script already open (AHKDECK_OPEN_SCRIPT), because the timed save
    # only applies to an editor that owns a disk file. Several edits land during
    # the earlier sections, so a regression shows up here within ~2 s.
    if autosave_probe is not None:
        client.tool("edit_editor", {"content": "; autosave probe v2\n"})
        time.sleep(1.8)
        try:
            disk = autosave_probe.read_text(encoding="utf-8", errors="replace")
        except OSError:
            disk = ""
        cell("tool edits are not autosaved", "v1" in disk and "v2" not in disk,
             disk[:80].replace("\n", " | "))


def report() -> int:
    failures = [r for r in RESULTS if r[1] != "PASS"]
    for name, status, detail in RESULTS:
        print(f"{status:4}  {name}" + (f"  [{detail}]" if status != "PASS" else ""))
    print(f"mcp gate: {len(RESULTS) - len(failures)}/{len(RESULTS)} cells passed")
    return len(failures)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    work = Path(sys.argv[-1])
    work.mkdir(parents=True, exist_ok=True)
    selftest = sys.argv[1] == "--selftest"

    if selftest:
        fake = Path(__file__).with_name("fake_mcp_server.py")
        client = McpStdio([sys.executable, str(fake), "--mcp-stdio"], work)
        resident = work / "selftest-resident.txt"
        exe_dir = work
        autosave_probe = None
        probe_ahk = None
        write_probe = None
    else:
        # The redaction cell below builds an absolute #Include path; a relative
        # exe argument would make that fixture relative too and the cell would
        # measure nothing. Resolve once, here.
        exe = str(Path(sys.argv[1]).resolve())
        # m082: the deck must run with a fast timed save and a script already
        # open in the editor, or the autosave cell below has nothing to watch.
        probe_ahk = Path(exe).parent / "Scripts" / "mcp-autosave-probe.ahk"
        probe_ahk.parent.mkdir(parents=True, exist_ok=True)
        probe_ahk.write_text("; autosave probe v1\r\n", encoding="utf-8")
        write_probe = Path(exe).parent / "Scripts" / WRITE_PROBE
        if write_probe.exists():
            write_probe.unlink()
        env = dict(os.environ)
        env["AHKDECK_AUTOSAVE_MS"] = "600"
        env["AHKDECK_OPEN_SCRIPT"] = "mcp-autosave-probe.ahk"
        # m106: 64 KB is the smallest cap the engine accepts; the rotation cell
        # writes far past it, so the .1 sidecar is created for real.
        env["AHKDECK_LOG_CAP"] = "65536"
        client = McpStdio([exe, "--mcp-stdio"], work, env=env)
        autosave_probe = probe_ahk
        # m062: run_editor now runs from a copy in the script's own folder
        # (real file, or Scripts\AHKDeck-preview.ahk), so A_ScriptDir is the
        # directory the user saves into. Probes therefore land next to that
        # copy, in Scripts, not in .temp.
        exe_dir = Path(exe).parent
        probe_dir = exe_dir / "Scripts"
        resident = probe_dir / "mcp-gate-resident.txt"
        for stale in (resident, probe_dir / "mcp-gate.txt",
                      probe_dir / "mcp-gate-patch.txt",
                      probe_dir / "mcp-gate-patch2.txt",
                      probe_dir / "mcp-gate-args.txt",
                      probe_dir / "mcp-gate-validated.txt",
                      probe_dir / "AHKDeck-preview.ahk",
                      exe_dir / ".temp" / "validate-editor.ahk"):
            if stale.exists():
                stale.unlink()
        for stale in (exe_dir / ".temp").glob("validate-*.out"):
            stale.unlink()

    try:
        probe(client, resident, exe_dir, autosave_probe)
    except Exception as exc:  # noqa: BLE001 - a gate reports, it does not traceback
        cell("probe completed", False, f"{type(exc).__name__}: {exc}")
    finally:
        client.close()
        # Two byproducts, both under <exe dir>\.temp: the resident tick file and
        # the file the FIXED script appends to A_ScriptDir. A gate that leaves
        # files behind is a gate that will be blamed for "the deck made a mess".
        for leftover in (resident, Path(exe_dir) / "Scripts" / "mcp-gate.txt",
                         Path(exe_dir) / "Scripts" / "mcp-gate-patch.txt",
                         Path(exe_dir) / "Scripts" / "mcp-gate-patch2.txt",
                         Path(exe_dir) / "Scripts" / "mcp-gate-args.txt",
                         Path(exe_dir) / "Scripts" / "mcp-gate-validated.txt",
                         Path(exe_dir) / "Scripts" / "AHKDeck-preview.ahk",
                         Path(exe_dir) / ".temp" / "validate-editor.ahk"):
            for _attempt in range(20):
                try:
                    if leftover.exists():
                        leftover.unlink()
                    break
                except OSError:
                    time.sleep(0.1)
        # m082: the autosave probe lives in the shipped Scripts folder and must
        # not outlive the gate either.
        if probe_ahk is not None:
            for _attempt in range(20):
                try:
                    if probe_ahk.exists():
                        probe_ahk.unlink()
                    break
                except OSError:
                    time.sleep(0.1)
        # m101: the write_script success probe, plus the backups each overwrite
        # forced. Only this probe's own leaf is removed; real user backups stay.
        if write_probe is not None:
            for _attempt in range(20):
                try:
                    if write_probe.exists():
                        write_probe.unlink()
                    break
                except OSError:
                    time.sleep(0.1)
            backups = write_probe.parent.parent / "Backups"
            try:
                for hit in backups.glob("*/" + WRITE_PROBE):
                    hit.unlink()
                    try:
                        hit.parent.rmdir()
                    except OSError:
                        pass
            except OSError:
                pass
        if not selftest:
            args_left = Path(exe_dir) / ".temp" / "mcp-gate-args.txt"
            try:
                if args_left.exists():
                    args_left.unlink()
            except OSError:
                pass
            cell("gate leaves no temp file behind",
                 not resident.exists()
                 and not (Path(exe_dir) / ".temp" / "mcp-gate.txt").exists()
                 and not (Path(exe_dir) / ".temp" / "mcp-gate-patch.txt").exists()
                 and not (Path(exe_dir) / ".temp" / "mcp-gate-patch2.txt").exists()
                 and not (Path(exe_dir) / ".temp" / "mcp-gate-validated.txt").exists()
                 and not (Path(exe_dir) / ".temp" / "validate-editor.ahk").exists()
                 and not any((Path(exe_dir) / ".temp").glob("validate-*.out"))
                 and not args_left.exists(),
                 str(resident))
            cell("the write probe left no file behind", not write_probe.exists(),
                 str(write_probe))
        try:
            log = client.stderr_path.read_bytes().decode("utf-8", "replace")
            cell("endpoint logged its lifecycle to stderr", "MCP endpoint ready" in log,
                 log[:120].replace("\n", " | "))
        except OSError as exc:
            cell("endpoint stderr captured", False, str(exc))
        # m084: FLTK's own errors must never appear. They used to open a modal
        # dialog (class "FLTK", so the #32770 counter never saw them) - one per
        # console line, and the error message was appended to the very console
        # that caused it, so the deck looped until it died. The error channel
        # writes them to stderr with the "[fltk]" tag; a line here means the
        # library tripped a consistency check again.
        try:
            log = client.stderr_path.read_bytes().decode("utf-8", "replace")
            fltk_lines = [ln for ln in log.splitlines() if ln.startswith("[fltk]")]
            cell("no FLTK error in the endpoint log", not fltk_lines,
                 f"{len(fltk_lines)}: {fltk_lines[:2]}")
        except OSError as exc:
            cell("FLTK error log read", False, str(exc))

    failures = report()
    if selftest:
        # The red-proof: the fake server breaks the version, the tool count and
        # the error code, so a client that caught none of them is blind.
        if len(RESULTS) < 5:
            print(f"selftest: only {len(RESULTS)} cells ran - the harness never talked")
            return 1
        if failures == 0:
            print("selftest: the fake server passed every cell, so this client proves nothing")
            return 1
        print(f"selftest: bad server caught in {failures} cell(s) - the gate can go red")
        return 0
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
