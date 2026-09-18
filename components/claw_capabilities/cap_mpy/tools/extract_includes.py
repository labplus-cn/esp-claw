#!/usr/bin/env python3
"""Extract include paths from compile_commands.json for QSTR preprocessing."""
import json
import re
import sys


def main():
    if len(sys.argv) < 2:
        print("Usage: extract_includes.py <compile_commands.json>", file=sys.stderr)
        sys.exit(1)

    cc_json_path = sys.argv[1]
    try:
        with open(cc_json_path) as f:
            data = json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        sys.exit(0)

    # Find the cap_mpy.c entry
    entry = None
    for e in data:
        if "cap_mpy/src/cap_mpy.c" in e.get("file", ""):
            entry = e
            break

    if not entry:
        sys.exit(0)

    command = entry.get("command", "")
    includes = re.findall(r"-I\s*(\S+)", command)
    for inc in includes:
        # Remove surrounding quotes if present
        inc = inc.strip('"').strip("'")
        print(inc)


if __name__ == "__main__":
    main()
