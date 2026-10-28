#!/usr/bin/env python3
"""Generate the machine-readable SIRIL command catalog.

The catalog is parsed from src/core/command_list.h so the CLI, the control
server and the HarmonyOS UI always describe exactly the commands this build
understands.  Run from the repository root:

    python3 scripts/gen-command-catalog.py \
        -o harmonyos/entry/src/main/resources/rawfile/commands.json
"""

import argparse
import json
import re
import sys
from pathlib import Path

NAME_RE = re.compile(r'\{\s*"([a-z_0-9]+)"\s*,')


def split_fields(body: str):
    """Split a C initializer body on top-level commas."""
    fields, depth, quoted, escaped, cur = [], 0, False, False, []
    for ch in body:
        if quoted:
            cur.append(ch)
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                quoted = False
            continue
        if ch == '"':
            quoted = True
            cur.append(ch)
        elif ch in "([{":
            depth += 1
            cur.append(ch)
        elif ch in ")]}":
            depth -= 1
            cur.append(ch)
        elif ch == "," and depth == 0:
            fields.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if cur:
        fields.append("".join(cur).strip())
    return fields


def unquote(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        return value[1:-1]
    return value


def parse(text: str):
    commands, seen = [], set()
    for m in NAME_RE.finditer(text):
        # locate the closing brace of this initializer
        start = m.start()
        depth, quoted, escaped, end = 0, False, False, -1
        for i in range(start, len(text)):
            ch = text[i]
            if quoted:
                if escaped:
                    escaped = False
                elif ch == "\\":
                    escaped = True
                elif ch == '"':
                    quoted = False
                continue
            if ch == '"':
                quoted = True
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    end = i
                    break
        if end < 0:
            continue
        body = text[start + 1:end]
        fields = split_fields(body)
        if len(fields) < 4:
            continue
        name = unquote(fields[0])
        if name in seen:
            continue
        try:
            min_args = int(fields[1])
        except ValueError:
            continue
        usage = unquote(fields[2])

        scriptable = True
        req_raw = ""
        for idx, field in enumerate(fields):
            if field in ("TRUE", "FALSE"):
                scriptable = field == "TRUE"
                req_raw = " ".join(fields[idx + 1:])
                break
        requires = []
        for token in re.findall(r"REQ_CMD_[A-Z_0-9]+", req_raw):
            flag = token.replace("REQ_CMD_", "")
            if flag != "NONE" and flag not in requires:
                requires.append(flag)

        seen.add(name)
        commands.append(
            {
                "name": name,
                "min_args": min_args,
                "usage": usage,
                "scriptable": scriptable,
                "requires": requires,
            }
        )
    commands.sort(key=lambda c: c["name"])
    return commands


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-i", "--input", default="src/core/command_list.h")
    ap.add_argument("-o", "--output", default="-")
    args = ap.parse_args()

    commands = parse(Path(args.input).read_text(encoding="utf-8"))
    payload = json.dumps(commands, indent=1, ensure_ascii=False) + "\n"
    if args.output == "-":
        sys.stdout.write(payload)
    else:
        Path(args.output).parent.mkdir(parents=True, exist_ok=True)
        Path(args.output).write_text(payload, encoding="utf-8")
        print(f"wrote {len(commands)} commands to {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
