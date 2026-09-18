#!/usr/bin/env python3
"""Filter preprocessed LVGL headers to exclude internal headers.

Usage: filter_lv_preprocessed.py <input.pp> <output.filtered> [pattern ...]

Each line starting with '#' is a preprocessor linemarker.
If the filename in the linemarker matches any of the given patterns,
all subsequent lines from that file are excluded until a new linemarker
switches to a non-excluded file.
"""

import sys
import re


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <input> <output> [pattern ...]", file=sys.stderr)
        sys.exit(1)

    input_path = sys.argv[1]
    output_path = sys.argv[2]
    patterns = sys.argv[3:]

    # Build a combined regex from the patterns
    if patterns:
        combined = "|".join(re.escape(p) for p in patterns)
        filter_re = re.compile(combined)
    else:
        filter_re = None

    # Preprocessor linemarker: # <lineno> "<filename>" [flags]
    linemarker_re = re.compile(r'^#\s+\d+\s+"([^"]+)"')

    excluded = False
    with open(input_path, "r") as fin, open(output_path, "w") as fout:
        for line in fin:
            m = linemarker_re.match(line)
            if m:
                filename = m.group(1)
                if filter_re and filter_re.search(filename):
                    excluded = True
                else:
                    excluded = False
                continue  # skip linemarker lines
            if not excluded:
                fout.write(line)


if __name__ == "__main__":
    main()
