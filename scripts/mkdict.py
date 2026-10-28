#!/usr/bin/env python3
"""Turn an SKK dictionary (EUC-JP, "よみ /候補1/候補2;注釈/") into the screen's
dict.txt: UTF-8, one entry per line, "よみ<TAB>候補1/候補2". Keeps the
okuri-nasi entries and the okuri-ari ones (reading + one romaji letter for
the inflection, e.g. "たべr<TAB>食べ"), drops annotations, Lisp candidates
and keys that are not plain hiragana.

    scripts/mkdict.py SKK-JISYO.L src/screen/dict.txt
"""
import sys

MAX_CANDS = 16

def plain_key(key):
    body, tail = key, ""
    if body and "a" <= body[-1] <= "z":
        body, tail = body[:-1], body[-1]
    if not body:
        return False
    for ch in body:
        if not (0x3041 <= ord(ch) <= 0x309F or ch == "ー"):
            return False
    return True

def main():
    src, dst = sys.argv[1], sys.argv[2]
    entries = {}
    with open(src, "rb") as f:
        raw = f.read()
    text = raw.decode("euc_jis_2004", errors="replace")
    for line in text.splitlines():
        if not line or line.startswith(";"):
            continue
        try:
            key, rest = line.split(" ", 1)
        except ValueError:
            continue
        if not plain_key(key):
            continue
        rest = rest.strip()
        if not (rest.startswith("/") and rest.endswith("/")):
            continue
        cands = []
        for c in rest[1:-1].split("/"):
            if not c or "(concat" in c:
                continue
            c = c.split(";", 1)[0]
            if not c or "�" in c or c in cands:
                continue
            cands.append(c)
            if len(cands) >= MAX_CANDS:
                break
        if cands:
            entries.setdefault(key, [])
            for c in cands:
                if c not in entries[key] and len(entries[key]) < MAX_CANDS:
                    entries[key].append(c)
    with open(dst, "w", encoding="utf-8") as out:
        out.write("# SKK-JISYO converted by scripts/mkdict.py (GPL-2.0-or-later, see the SKK dictionary's header)\n")
        for key in sorted(entries):
            out.write(key + "\t" + "/".join(entries[key]) + "\n")
    print("%d entries" % len(entries))

if __name__ == "__main__":
    main()
