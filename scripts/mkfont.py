#!/usr/bin/env python3
"""Build src/screen/font.hex from the Shinonome BDF fonts (public domain).

    scripts/mkfont.py path/to/shnm8x16r.bdf path/to/shnmk16.bdf > src/screen/font.hex

Output: one glyph per line, `XXXX:HEX` (Unicode code point, then 16 rows of
either 1 byte (8 px wide) or 2 bytes (16 px wide) as hex), the format GNU
Unifont uses. The half-width font is JIS X 0201 (ASCII plus half-width
katakana), the full-width one JIS X 0208 (kana, kanji, symbols); both are
mapped to Unicode here so the screen program needs no tables of its own.
"""
import sys


def read_bdf(path):
    glyphs = {}
    enc = None
    rows = None
    with open(path, encoding="latin-1") as f:
        for line in f:
            line = line.strip()
            if line.startswith("ENCODING "):
                enc = int(line.split()[1])
            elif line == "BITMAP":
                rows = []
            elif line == "ENDCHAR":
                if enc is not None and rows is not None:
                    glyphs[enc] = rows
                enc, rows = None, None
            elif rows is not None:
                rows.append(line)
    return glyphs


def main():
    half, full = sys.argv[1], sys.argv[2]
    out = {}
    for code, rows in read_bdf(half).items():
        if 0x20 <= code <= 0x7E:
            cp = code                      # JIS X 0201 roman; 0x5C/0x7E kept as \ and ~
        elif 0xA1 <= code <= 0xDF:
            cp = 0xFF61 + (code - 0xA1)    # half-width katakana
        else:
            continue
        out[cp] = "".join(r[:2] for r in rows)
    for code, rows in read_bdf(full).items():
        b = bytes([(code >> 8) | 0x80, (code & 0xFF) | 0x80])
        try:
            ch = b.decode("euc_jp")
        except UnicodeDecodeError:
            continue
        if len(ch) != 1:
            continue
        out[ord(ch)] = "".join(r[:4] for r in rows)
    # 0x5C in JIS X 0201 is the yen sign; keep a backslash there and give
    # U+00A5 the same glyph so both show something sensible.
    if 0x5C in out and 0xA5 not in out:
        out[0xA5] = out[0x5C]
    print("# AlterEgOS screen font: Shinonome 16 (Public Domain, /efont/ 2001,")
    print("# http://openlab.ring.gr.jp/efont/), converted by scripts/mkfont.py.")
    print("# code point:16 rows, 1 or 2 bytes per row, MSB first")
    for cp in sorted(out):
        print("%04X:%s" % (cp, out[cp]))


main()
