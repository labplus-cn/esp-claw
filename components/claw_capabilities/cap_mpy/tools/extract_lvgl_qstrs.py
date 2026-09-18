#!/usr/bin/env python3
"""Extract QSTR definitions from generated lv_mp.c for MicroPython QSTR pipeline.

Reads the generated lv_mp.c file and extracts all MP_QSTR_xxx references,
then writes them as Q(xxx) definitions to qstrdefsport.h.

This breaks the circular dependency: lv_mp.c generation (gen_mpy.py) does NOT
need qstrdefs.generated.h, only lv_mp.c *compilation* does.  By extracting
from the generated source instead of the preprocessed LVGL header we get the
exact set of QSTR names the bindings actually reference.
"""
import re
import sys


def extract_from_lv_mp_c(text):
    """Extract all MP_QSTR_xxx references from generated lv_mp.c."""
    pattern = re.compile(r'MP_QSTR_(\w+)')
    names = set()
    for match in pattern.finditer(text):
        names.add(match.group(1))
    return names


def extract_from_preprocessed(text):
    """Fallback: extract LVGL identifiers from preprocessed C code.

    This is less accurate but can be used if lv_mp.c is not yet available.
    """
    patterns = [
        r'\b(lv_[a-zA-Z_]\w*)\b',
        r'\b(LV_[A-Z_]\w*)\b',
    ]
    names = set()
    for pattern in patterns:
        for match in re.finditer(pattern, text):
            name = match.group(1)
            if len(name) > 3:
                names.add(name)
    return names


# Extra QSTR names emitted by gen_mpy.py that are NOT LVGL API names.
# These include custom exception types, special dunder methods, helper
# struct/class names, and field names from the C_Pointer union.
EXTRA_NAMES = {
    # Custom exception type
    'LvReferenceError',
    # Special dunder methods
    '__SIZE__', '__dereference__', '__cast__', '__cast_instance__',
    # Helper types emitted by gen_mpy.py
    'Blob', 'cast', 'dereference',
    'Struct', 'C_Array', 'C_Pointer',
    # C_Pointer union field names
    'ptr_val', 'str_val', 'int_val', 'uint_val',
}


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input_file> <output_qstrdefs>",
              file=sys.stderr)
        sys.exit(1)

    input_file = sys.argv[1]
    output_file = sys.argv[2]

    try:
        with open(input_file, 'r') as f:
            text = f.read()
    except FileNotFoundError:
        # Write empty placeholder so the build can continue
        with open(output_file, 'w') as f:
            f.write('/* LVGL QSTR definitions - pending generation */\n')
        return

    # Choose extraction strategy based on file content
    if 'MP_QSTR_' in text:
        names = extract_from_lv_mp_c(text)
    else:
        names = extract_from_preprocessed(text)

    all_names = names | EXTRA_NAMES

    with open(output_file, 'w') as f:
        f.write('/* Auto-generated LVGL QSTR definitions */\n')
        for name in sorted(all_names):
            f.write(f'Q({name})\n')

    print(f"Generated {len(all_names)} LVGL QSTR definitions in {output_file}",
          file=sys.stderr)


if __name__ == '__main__':
    main()
