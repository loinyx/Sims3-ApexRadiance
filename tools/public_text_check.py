#!/usr/bin/env python3
"""Checks public text against docs/PUBLIC-TEXT-POLICY.md: changed files, commit messages, PR titles and bodies.

Exit code 1 when a rule matches outside the exceptions in tools/public_text_allow.txt. Standard library only.
Source files (SOURCE_EXT) also take the date and stamp rules (SOURCE_RULES). tools/public_text_legacy.txt lists lines
that matched those rules or the unaccented Portuguese tokens when they were added; a listed line is accepted as long
as it stays unchanged. That list is temporary: it only shrinks, as the lines are rewritten.
Personal names are not stored in the repository: set PUBLIC_TEXT_PERSONAL to a comma-separated list of terms (a
repository secret in CI, an environment variable locally).
"""
import argparse
import fnmatch
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

RULES = [
    ("local-path", re.compile(r"[A-Za-z]:\\\\?Users\\\\?|/Users/[A-Za-z]|/home/[a-z]|%USERPROFILE%\\Desktop|\\Desktop\\|Backups Sims 3|\bscratchpad\b", re.I)),
    ("personal", re.compile("|".join([r"@gmail\.com"] + [re.escape(t.strip()) for t in
                                     os.environ.get("PUBLIC_TEXT_PERSONAL", "").split(",") if t.strip()]), re.I)),
    ("conversation", re.compile(
        r"\b(the )?(user|maintainer)('s)? (asked|said|says|chose|approved|confirmed|preferred|prefers|requested|wants|wanted|reported|feedback|rule|choice|decision)\b"
        r"|\buser-approved\b|\bat the user's request\b|\(user[,)]|\buser rule\b|\breply in portuguese\b", re.I)),
    ("assistant", re.compile(r"(?<![.\w/-])(claude|codex|chatgpt|copilot)\b(?!\.md|\.local)|\bsubagent\b|\bagent handoff\b|claude\.ai/|Claude-Session:|Co-Authored-By: Claude", re.I)),
    ("clock-time", re.compile(r"\b(at|around|about|by) [0-2]?[0-9]:[0-5][0-9]\b", re.I)),
    ("security-product", re.compile(r"\b(kaspersky|bitdefender|ransomware)\b", re.I)),
    ("portuguese", re.compile(r"\b(ficou|lindo|ótimo|otimo|não|nao|usuário|obrigado|comodo|etapa|nenhum|andar)\b", re.I)),
]
# Source comments state the fact, not when it was seen: no day/month dates and no hh-mm-ss capture stamps
SOURCE_RULES = [
    ("date", re.compile(r"\b[0-3]\d/[01]\d\b")),
    ("stamp", re.compile(r"\b[0-2]\d-[0-5]\d-[0-5]\d\b")),
]
SOURCE_EXT = {".cpp", ".h", ".hpp", ".inl", ".c", ".hlsl", ".fx", ".fxh"}
TITLE = re.compile(r"^[a-z0-9-]+: [^\s].{0,70}$")
TEXT_EXT = {".cpp", ".h", ".hpp", ".inl", ".c", ".md", ".txt", ".ps1", ".pl", ".py", ".mjs", ".js", ".yml", ".yaml",
            ".hlsl", ".fx", ".fxh", ".rc", ".toml", ".json", ".xml", ".vcxproj", ".filters", ".props", ".sh"}


def load_allow():
    allow = []
    path = os.path.join(ROOT, "tools", "public_text_allow.txt")
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            line = line.rstrip("\n")
            if not line or line.startswith("#") or "\t" not in line:
                continue
            glob, rule = line.split("\t", 1)
            allow.append((glob.strip(), rule.strip()))
    return allow


def load_legacy():
    """The legacy lines (path<TAB>rule<TAB>line, the line stripped) as a set of (path, rule, line)."""
    legacy = set()
    path = os.path.join(ROOT, "tools", "public_text_legacy.txt")
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            line = line.rstrip("\r\n")
            if not line or line.startswith("#") or line.count("\t") < 2:
                continue
            p, rule, text = line.split("\t", 2)
            legacy.add((p, rule, text.strip()))
    return legacy


def allowed(allow, path, rule):
    path = path.replace("\\", "/")
    for glob, r in allow:
        if (r == "*" or r == rule) and (fnmatch.fnmatch(path, glob) or (glob.endswith("/**") and path.startswith(glob[:-2]))):
            return True
    return False


def scan(label, text, allow, path=None, legacy=frozenset()):
    problems = []
    rules = RULES
    if path is not None and os.path.splitext(path)[1].lower() in SOURCE_EXT:
        rules = RULES + SOURCE_RULES
    for n, line in enumerate(text.splitlines(), 1):
        for rule, rx in rules:
            if path is not None and allowed(allow, path, rule):
                continue
            m = rx.search(line)
            if m and (path is None or (path.replace("\\", "/"), rule, line.strip()) not in legacy):
                problems.append(f"{label}:{n}: [{rule}] {m.group(0)!r}: {line.strip()[:120]}")
    return problems


def check_title(title, label="title"):
    title = title.strip()
    problems = []
    if not TITLE.match(title):
        problems.append(f"{label}: [title] expected 'area: imperative summary' (lower-case area, at most 72 characters): {title!r}")
    if title.endswith(".") or "(" in title:
        problems.append(f"{label}: [title] no trailing period or parentheses: {title!r}")
    return problems + scan(label, title, [])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--files", nargs="*", default=[])
    ap.add_argument("--commit-msg")
    ap.add_argument("--title")
    ap.add_argument("--text-file", action="append", default=[])
    a = ap.parse_args()
    allow = load_allow()
    legacy = load_legacy()
    problems = []
    for f in a.files:
        rel = os.path.relpath(os.path.abspath(f), ROOT).replace("\\", "/")
        if os.path.splitext(f)[1].lower() not in TEXT_EXT or not os.path.isfile(f):
            continue
        with open(f, encoding="utf-8", errors="replace") as fh:
            problems += scan(rel, fh.read(), allow, rel, legacy)
    if a.commit_msg:
        lines = [l for l in open(a.commit_msg, encoding="utf-8").read().splitlines() if not l.startswith("#")]
        if lines and not lines[0].startswith(("Merge ", "Revert ", "fixup! ", "squash! ")):
            problems += check_title(lines[0], "commit subject")
        problems += scan("commit message", "\n".join(lines[1:]), [])
    if a.title is not None:
        problems += check_title(a.title)
    for f in a.text_file:
        problems += scan(os.path.basename(f), open(f, encoding="utf-8").read(), [])
    for p in problems:
        print(p)
    if problems:
        print(f"\n{len(problems)} public-text problem(s); see docs/PUBLIC-TEXT-POLICY.md")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
