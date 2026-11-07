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

EXPECTED_TOOLS = {
    "list_scripts",
    "read_script",
    "edit_editor",
    "run_editor",
    "run_script",
    "get_session_output",
    "stop_script",
}

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
    cell("tools/list count == 7", len(tools) == len(EXPECTED_TOOLS),
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
    cell("the failing file is still named", "ai-editor.ahk" in out1, out1[:200].replace("\n", " | "))

    client.tool("edit_editor", {"content": FIXED})
    run2, _ = client.tool("run_editor", {"timeout_ms": 8000})
    cell("fixed script passes", run2.get("rc") == 0 and not run2.get("fatal"),
         f"state={run2.get('state')} rc={run2.get('rc')} output={str(run2.get('output'))[:120]}")

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

    # --- 6. the policy boundary still holds over the endpoint -------------
    denied, err = client.tool("write_script", {"name": "..\\evil.ahk", "content": FIXED})
    cell("'..\\evil.ahk' refused", err and bool(denied.get("error")),
         json.dumps(denied, ensure_ascii=False)[:160])

    # --- 7. a resident script must not look like a crash ------------------
    client.tool("edit_editor", {"content": RESIDENT})
    run3, _ = client.tool("run_editor", {"timeout_ms": 2000})
    cell("resident script returns state=running with no rc",
         run3.get("state") == "running" and run3.get("rc") is None,
         f"state={run3.get('state')} rc={run3.get('rc')} fatal={run3.get('fatal')}")
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
    else:
        exe = sys.argv[1]
        # m082: the deck must run with a fast timed save and a script already
        # open in the editor, or the autosave cell below has nothing to watch.
        probe_ahk = Path(exe).parent / "Scripts" / "mcp-autosave-probe.ahk"
        probe_ahk.parent.mkdir(parents=True, exist_ok=True)
        probe_ahk.write_text("; autosave probe v1\r\n", encoding="utf-8")
        env = dict(os.environ)
        env["AHKDECK_AUTOSAVE_MS"] = "600"
        env["AHKDECK_OPEN_SCRIPT"] = "mcp-autosave-probe.ahk"
        client = McpStdio([exe, "--mcp-stdio"], work, env=env)
        autosave_probe = probe_ahk
        # run_editor executes the editing area from <exe dir>\.temp\ai-editor.ahk
        # (m062 wants that changed: A_ScriptDir differs from where the user saves).
        exe_dir = Path(exe).parent
        resident = exe_dir / ".temp" / "mcp-gate-resident.txt"
        for stale in (resident, exe_dir / ".temp" / "mcp-gate.txt"):
            if stale.exists():
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
        for leftover in (resident, Path(exe_dir) / ".temp" / "mcp-gate.txt"):
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
        if not selftest:
            cell("gate leaves no temp file behind",
                 not resident.exists() and not (Path(exe_dir) / ".temp" / "mcp-gate.txt").exists(),
                 str(resident))
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
