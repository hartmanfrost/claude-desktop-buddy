#!/usr/bin/env python3
"""Extract a single u8g2 font byte array from the upstream u8g2_fonts.c blob.

u8g2_fonts.c stores each font as one big C string-literal assignment. We
parse it directly with a small regex+state-machine instead of pulling in a
real C parser - the format is rigidly machine-generated so this is robust
enough.

Run with the path to the upstream source, the font symbol, and the output
header path. The generated header exposes a single
`extern const uint8_t <symbol>[];` array.

Usage:
    python extract_cyrillic.py /tmp/u8g2_fonts.c \\
        u8g2_font_6x12_t_cyrillic cyrillic_font.h
"""
from __future__ import annotations
import re
import sys
from pathlib import Path


def unescape_c_string(s: str) -> bytes:
    """Decode a C-source string literal payload (sans surrounding quotes) into bytes.

    Handles octal \\NNN (1-3 digits), hex \\xHH, and the standard short escapes.
    """
    out = bytearray()
    i = 0
    while i < len(s):
        c = s[i]
        if c != "\\":
            out.append(ord(c))
            i += 1
            continue
        i += 1
        nxt = s[i]
        if nxt == "x":
            j = i + 1
            while j < len(s) and j - i - 1 < 2 and s[j] in "0123456789abcdefABCDEF":
                j += 1
            out.append(int(s[i + 1 : j], 16))
            i = j
        elif nxt in "01234567":
            j = i
            while j < len(s) and j - i < 3 and s[j] in "01234567":
                j += 1
            out.append(int(s[i:j], 8))
            i = j
        else:
            short = {"n": 0x0A, "r": 0x0D, "t": 0x09, "0": 0x00,
                     "\\": 0x5C, "'": 0x27, '"': 0x22, "?": 0x3F,
                     "a": 0x07, "b": 0x08, "f": 0x0C, "v": 0x0B}
            out.append(short.get(nxt, ord(nxt)))
            i += 1
    return bytes(out)


def extract_font(src: str, symbol: str) -> bytes:
    """Pull the byte payload of `symbol` out of u8g2_fonts.c content.

    Can't end on the first `;` after `=` — `;` (0x3B) is a printable byte
    that u8g2 emits unescaped inside the C string literal, and a naïve
    `(.*?);` regex stops there. Walk forward instead, accumulating each
    "..." literal up to the first `;` that's *outside* a string.
    """
    decl = re.search(rf"const\s+uint8_t\s+{re.escape(symbol)}\s*\[", src)
    if not decl:
        raise SystemExit(f"symbol {symbol!r} not found")
    eq = src.index("=", decl.end())
    i = eq + 1
    chunks: list[str] = []
    n = len(src)
    while i < n:
        c = src[i]
        if c.isspace():
            i += 1
            continue
        if c == '"':
            # Walk to matching unescaped closing quote.
            j = i + 1
            while j < n:
                if src[j] == "\\" and j + 1 < n:
                    j += 2
                    continue
                if src[j] == '"':
                    break
                j += 1
            chunks.append(src[i + 1 : j])
            i = j + 1
            continue
        if c == ";":
            break
        # u8g2 sometimes prefixes the declaration with a macro call before
        # the literal — skip until we hit either `"` or `;`.
        i += 1
    return b"".join(unescape_c_string(c) for c in chunks)


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    src_path = Path(sys.argv[1])
    symbol = sys.argv[2]
    out_path = Path(sys.argv[3])

    src = src_path.read_text(encoding="latin-1")
    data = extract_font(src, symbol)

    rows = []
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        rows.append("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")

    header = (
        f"// Auto-generated from u8g2_fonts.c - do not edit by hand.\n"
        f"// Source: olikraus/u8g2 -> {symbol}\n"
        f"// Regenerate with extract_cyrillic.py if you swap fonts.\n"
        f"#pragma once\n"
        f"#include <stdint.h>\n\n"
        f"extern const uint8_t {symbol}[{len(data)}];\n"
    )
    impl = (
        f"// Auto-generated. See extract_cyrillic.py.\n"
        f"#include \"{out_path.name}\"\n\n"
        f"const uint8_t {symbol}[{len(data)}] = {{\n"
        + "\n".join(rows)
        + "\n};\n"
    )

    out_path.write_text(header, encoding="ascii")
    out_path.with_suffix(".c").write_text(impl, encoding="ascii")
    print(f"wrote {out_path} ({len(data)} bytes) and {out_path.with_suffix('.c')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
