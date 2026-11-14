#!/usr/bin/env python3
"""Build docs/help-index.json from the AutoHotkey v2 help file.

Why the .chm and not a web mirror: the index has to describe the engine that
actually runs the scripts. The vendored markdown mirror in .3rd/help tops out at
2.0.18; the chm next to AutoHotkey_2.0.30_setup.exe is the 2.0.30 text, which is
what the shipped AutoHotkey64.exe is. A wrong signature in this file teaches the
AI the wrong API, so the version match is the whole point.

Usage:
    python tools/make_help_index.py            # extract (once) + write the index
    python tools/make_help_index.py --show StrSplit    # dump one entry, to eyeball

The extraction goes to .temp/chm_extract and is reused when already present;
delete that folder to re-extract after swapping the chm.

No dependency beyond the standard library and the system hh.exe.
"""

from __future__ import annotations

import argparse
import html
import json
import os
import pathlib
import re
import subprocess
import sys

MAIN = pathlib.Path(__file__).resolve().parent.parent
CHM = MAIN.parent / ".3rd" / "AutoHotkey_2.0.30" / "AutoHotkey.chm"
EXTRACT = MAIN.parent / ".temp" / "chm_extract"
OUT = MAIN / "docs" / "help-index.json"

# Field caps. The reader is a model with a context budget, so every field is
# bounded: a 3 KB page is worse than useless when the caller only needs the
# parameter meanings. These numbers keep an average entry near 600 bytes.
SUMMARY_MAX = 220
DESC_MAX = 220
RETURNS_MAX = 200
EXAMPLE_MAX = 360


def clip(text: str, limit: int) -> str:
    text = text.strip()
    if len(text) <= limit:
        return text
    cut = text[:limit].rstrip()
    # Cut on a word boundary when there is one nearby, so a truncated sentence
    # does not end mid-token ("InitialValue" -> "InitialVal").
    space = cut.rfind(" ")
    if space > limit * 0.6:
        cut = cut[:space]
    return cut + "..."


def strip_tags(fragment: str, breaks: str = " ") -> str:
    """Flatten inline markup. `breaks` decides what a <br> becomes: a space for
    prose, a newline inside a <pre> where each line is a separate signature."""
    fragment = re.sub(r"<br\s*/?>", breaks, fragment, flags=re.I)
    fragment = re.sub(r"<[^>]+>", "", fragment)
    fragment = html.unescape(fragment)
    if breaks == "\n":
        return "\n".join(line.rstrip() for line in fragment.splitlines() if line.strip())
    return re.sub(r"\s+", " ", fragment).strip()


def first(pattern: str, text: str, flags: int = re.S | re.I) -> str:
    m = re.search(pattern, text, flags)
    return m.group(1) if m else ""


def section(text: str, anchor: str) -> str:
    """Body of the <h2 id="anchor"> section, up to the next h2 or the footer."""
    m = re.search(
        r'<h2 id="%s"[^>]*>.*?</h2>(.*?)(?=<h2[ >]|</body>|$)' % re.escape(anchor),
        text, re.S | re.I)
    return m.group(1) if m else ""


def parse_params(body: str) -> list[dict]:
    out = []
    for m in re.finditer(r"<dt[^>]*>(.*?)</dt>\s*<dd[^>]*>(.*?)</dd>", body, re.S | re.I):
        name = strip_tags(m.group(1))
        if not name:
            continue
        paragraphs = [strip_tags(p) for p in
                      re.findall(r"<p[^>]*>(.*?)</p>", m.group(2), re.S | re.I)]
        kind = ""
        desc = []
        for paragraph in paragraphs:
            if not paragraph:
                continue
            lower = paragraph.lower()
            if lower.startswith("type:") and not kind:
                kind = paragraph[5:].strip()
            else:
                desc.append(paragraph)
        out.append({"name": name, "type": kind,
                    "desc": clip(" ".join(desc), DESC_MAX)})
    return out


def parse_page(path: pathlib.Path) -> dict | None:
    try:
        raw = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None

    name = strip_tags(first(r"<h1[^>]*>(.*?)</h1>", raw))
    if not name:
        return None

    # A page often documents several spellings at once ("Send / SendText /
    # SendEvent / SendInput / SendPlay", "Run / RunWait") or a class ("Gui
    # Object"). Callers ask for one short name, so every spelling has to be
    # matchable - and that includes the first one, which is why the split keeps
    # it instead of dropping it.
    parts = [part.strip() for part in name.split("/") if part.strip()]
    aliases = parts if len(parts) > 1 else []
    if name.endswith(" Object"):
        aliases = aliases + [name[: -len(" Object")].strip()]

    # Some pages carry a second <pre class="Syntax"> that is a legend rather
    # than a signature (Format's "Flags Width .Precision ULT Type"). A legend is
    # several words with no call syntax at all, so the test is "looks like a
    # call" rather than "contains a parenthesis" - the command form ("Send
    # Keys") has no parentheses and is still a real signature. The test runs per
    # line: Send's block holds five signatures, and judging the block as a whole
    # would throw all five away because it is long.
    syntaxes = []
    for block in re.findall(r'<pre class="Syntax"[^>]*>(.*?)</pre>', raw, re.S | re.I):
        text = strip_tags(block, breaks="\n")
        keep = [line for line in text.splitlines()
                if any(ch in line for ch in "(),:=") or len(line.split()) <= 3]
        if keep:
            syntaxes.append("\n".join(keep))

    summary = strip_tags(first(r"</h1>\s*<p[^>]*>(.*?)</p>", raw))

    params = parse_params(section(raw, "Parameters"))
    returns = clip(strip_tags(first(r"<p[^>]*>(.*?)</p>", section(raw, "Return_Value"))),
                   RETURNS_MAX)
    # The return section opens with a "Type: X" line, which is already implied
    # by the field name; leaving it in makes the caller read past noise.
    if returns.lower().startswith("type:"):
        returns = returns[5:].strip()

    example = ""
    examples = section(raw, "Examples")
    if examples:
        block = first(r"<pre[^>]*>(.*?)</pre>", examples)
        example = clip(strip_tags(block), EXAMPLE_MAX)

    entry = {
        "name": name,
        "page": "docs/lib/" + path.name,
        "syntax": "\n".join(syntaxes),
        "summary": clip(summary, SUMMARY_MAX),
    }
    if aliases:
        entry["aliases"] = aliases
    if params:
        entry["params"] = params
    if returns:
        entry["returns"] = returns
    if example:
        entry["example"] = example
    return entry


def extract(force: bool) -> pathlib.Path:
    lib = EXTRACT / "docs" / "lib"
    if (lib.is_dir() and any(lib.glob("*.htm"))) and not force:
        return lib
    if not CHM.is_file():
        sys.exit(f"错误：找不到帮助文件 {CHM}")
    EXTRACT.mkdir(parents=True, exist_ok=True)
    # -decompile writes the chm's internal tree into the target folder. hh.exe
    # returns immediately on some builds, so wait on the process itself rather
    # than assuming it finished.
    result = subprocess.run(["hh.exe", "-decompile", str(EXTRACT), str(CHM)],
                            capture_output=True)
    if result.returncode != 0:
        sys.exit(f"错误：hh.exe -decompile 失败 rc={result.returncode} {result.stderr!r}")
    if not lib.is_dir():
        sys.exit(f"错误：解包后没有 {lib}，chm 结构可能变了")
    return lib


def main() -> int:
    ap = argparse.ArgumentParser(description="生成 AHK v2 函数索引")
    ap.add_argument("--force", action="store_true", help="强制重新解包 chm")
    ap.add_argument("--show", metavar="名称", help="打印一条索引，用于人工核对")
    ap.add_argument("--out", default=str(OUT), help="输出路径")
    args = ap.parse_args()

    lib = extract(args.force)

    if args.show:
        path = lib / (args.show + ".htm")
        if not path.is_file():
            sys.exit(f"错误：{path} 不存在")
        print(json.dumps(parse_page(path), ensure_ascii=False, indent=2))
        return 0

    entries = []
    skipped = []
    for path in sorted(lib.glob("*.htm")):
        entry = parse_page(path)
        if entry:
            entries.append(entry)
        else:
            skipped.append(path.name)

    if len(entries) < 300:
        sys.exit(f"错误：只解析出 {len(entries)} 条，chm 结构可能变了（预期 350+）")

    text = json.dumps({"engine": "2.0.30", "count": len(entries), "entries": entries},
                      ensure_ascii=False, separators=(",", ":"), indent=0)
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text + "\n", encoding="utf-8", newline="\n")

    size = len(text.encode("utf-8"))
    no_syntax = sum(1 for e in entries if not e["syntax"])
    no_params = sum(1 for e in entries if not e.get("params"))
    print(f"写入 {out}")
    print(f"  {len(entries)} 条 / {size} B / 平均 {size // len(entries)} B")
    print(f"  无签名 {no_syntax} 条，无参数 {no_params} 条" +
          (f"，跳过 {len(skipped)} 个文件" if skipped else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
