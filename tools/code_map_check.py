#!/usr/bin/env python3
"""Checks that the code map (docs/code-map.md, docs/code-map/*.md) still matches the code: every file, symbol and
address it names exists.

References in backticks:
  `features/outdoor_delta.cpp`             the file exists (bare names such as `apex_main.cpp` are searched repo-wide)
  `features/outdoor_delta.cpp:ReadList`     the file exists and contains the identifier (`A::B` checks `B`)
  `patches/x.cpp@0x006C7160`                the file contains the address (leading zeros and case ignored)
Relative Markdown links must resolve. Exit code 1 on any stale reference. Standard library only.
Usage: python tools/code_map_check.py [map.md ...]   (default: the whole code map)
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CODE_EXT = r"(?:cpp|h|hpp|inc|hlsl|ps1|py|pl|mjs|md|tsv|yml|vcxproj)"
REF = re.compile(r"`([\w./-]+\." + CODE_EXT + r"|[\w./-]+/)(?:(:)([A-Za-z_][\w:~]*)|(@)(0x[0-9A-Fa-f]+))?`")
LINK = re.compile(r"\]\(([^)\s#]+)(?:#[^)]*)?\)")
SKIP_DIRS = {".git", "third_party", "vcpkg_installed", "Release", "Public", "outputs", "node_modules"}

_index = None


def find_bare(name):
    global _index
    if _index is None:
        _index = {}
        for d, dirs, files in os.walk(ROOT):
            dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
            for f in files:
                _index.setdefault(f, []).append(os.path.join(d, f))
    return _index.get(name, [])


def read(path, cache={}):
    if path not in cache:
        with open(path, encoding="utf-8", errors="replace") as f:
            cache[path] = f.read()
    return cache[path]


def resolve(token):
    path = os.path.join(ROOT, token)
    if os.path.exists(path):
        return [path]
    return find_bare(token) if "/" not in token else []


def check_text(text, base_dir):
    errors = []
    for m in REF.finditer(text):
        token, sym, addr = m.group(1), m.group(3), m.group(5)
        paths = resolve(token)
        if not paths:
            errors.append(f"missing file: {token}")
            continue
        if sym:
            name = sym.split("::")[-1]
            if not any(re.search(r"\b" + re.escape(name) + r"\b", read(p)) for p in paths if os.path.isfile(p)):
                errors.append(f"symbol not found: {token}:{sym}")
        if addr:
            digits = addr[2:].lstrip("0") or "0"
            pat = re.compile(r"0x0*" + digits + r"\b", re.I)
            if not any(pat.search(read(p)) for p in paths if os.path.isfile(p)):
                errors.append(f"address not found: {token}@{addr}")
    for m in LINK.finditer(text):
        target = m.group(1)
        if "://" in target or target.startswith("mailto:"):
            continue
        if not os.path.exists(os.path.normpath(os.path.join(base_dir, target))):
            errors.append(f"broken link: {target}")
    return errors


def self_test():
    ok = "`tools/code_map_check.py:check_text` `tools/code_map_check.py@0x0` [x](code_map_check.py)"
    assert check_text(ok, os.path.join(ROOT, "tools")) == [], check_text(ok, os.path.join(ROOT, "tools"))
    # split literals, so this file does not contain the names it must not find
    bad = "`tools/nope.cpp` `tools/code_map_check.py:NoSuch" + "Name` `tools/code_map_check.py@0xDEAD" + "1` [x](nope.md)"
    assert len(check_text(bad, os.path.join(ROOT, "tools"))) == 4


def main(argv):
    self_test()
    area_dir = os.path.join(ROOT, "docs", "code-map")
    maps = argv or [os.path.join(ROOT, "docs", "code-map.md")] + sorted(
        os.path.join(area_dir, f) for f in os.listdir(area_dir) if f.endswith(".md"))
    failed = 0
    for path in maps:
        errors = check_text(read(path), os.path.dirname(os.path.abspath(path)))
        refs = len(REF.findall(read(path)))
        for e in errors:
            print(f"{os.path.relpath(path, ROOT)}: {e}")
        print(f"{os.path.relpath(path, ROOT)}: {refs} code references, {len(errors)} stale")
        failed += len(errors)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
