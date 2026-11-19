#!/usr/bin/env python3
"""Lazy shader restore: every place that reads the device's shaders back (Get*Shader, a state block capture) settles a
pending restore first (D3D9Hooks::SettleDeferredShaders within the lines before it), and both UP draw hooks settle.
Usage: python tools/deferred_shader_sites_check.py [--self-check]"""
import pathlib
import re
import sys

READ = re.compile(r"(?:->|\.)\s*(?:GetPixelShader|GetVertexShader)\s*\(\s*&|->\s*Capture\s*\(\s*\)|CreateStateBlock\s*\(")
SETTLE = "SettleDeferredShaders("
WINDOW = 8  # lines before a read in which the settle must appear
# Reads that need no settle, with the reason
ALLOWED = {
    "framework/d3d9_hooks.cpp": "the restore machinery itself",
    "features/picture.cpp:m_checkPasses": "reads back its own shader, set through the hook just before (clears the pending restore)",
    "features/f10_study.h": "development study: writes the shader to a file, never binds it again",
}
UP_HOOKS = ("HookedDrawPrimitiveUP(", "HookedDrawIndexedPrimitiveUP(")


def allowed(rel, lines, i):
    if ALLOWED.get(rel):
        return True
    near = "\n".join(lines[max(0, i - WINDOW):i + 1])
    return any(rel == k.split(":", 1)[0] and ":" in k and k.split(":", 1)[1] in near for k in ALLOWED)


def missing_sites(files):
    """files: iterable of (relative path, text). Returns 'path:line' of reads without a settle before them."""
    out = []
    for rel, text in files:
        lines = text.splitlines()
        for i, line in enumerate(lines):
            if line.lstrip().startswith("//") or not READ.search(line):
                continue
            if any(SETTLE in l for l in lines[max(0, i - WINDOW):i + 1]) or allowed(rel, lines, i):
                continue
            out.append(f"{rel}:{i + 1}")
    return out


def up_hooks_settle(text):
    ok = True
    for name in UP_HOOKS:
        at = text.find("__stdcall " + name)
        body = text[at:text.find("\n}", at)] if at >= 0 else ""
        ok &= SETTLE in body
    return ok


def self_check():
    bad = ("features/x.cpp", "void F(IDirect3DDevice9* dev) {\n    dev->GetPixelShader(&ps);\n}\n")
    good = ("features/y.cpp", "void F(IDirect3DDevice9* dev) {\n    D3D9Hooks::SettleDeferredShaders(dev);\n    dev->GetPixelShader(&ps);\n    g_sb->Capture();\n}\n")
    comment = ("features/z.cpp", "    // dev->GetVertexShader(&vs);\n")
    far = ("features/w.cpp", "D3D9Hooks::SettleDeferredShaders(dev);\n" + "x;\n" * 9 + "dev->CreateStateBlock(D3DSBT_ALL, &sb);\n")
    assert missing_sites([bad]) == ["features/x.cpp:2"], missing_sites([bad])
    assert missing_sites([good, comment]) == []
    assert missing_sites([far]) == ["features/w.cpp:11"]
    assert up_hooks_settle("HRESULT __stdcall HookedDrawPrimitiveUP(a) {\n    D3D9Hooks::SettleDeferredShaders(dev);\n}\n"
                           "HRESULT __stdcall HookedDrawIndexedPrimitiveUP(b) {\n    D3D9Hooks::SettleDeferredShaders(dev);\n}\n")
    assert not up_hooks_settle("HRESULT __stdcall HookedDrawPrimitiveUP(a) {\n    obs();\n}\n")
    print("self-check ok")


def main():
    if "--self-check" in sys.argv:
        self_check()
        return 0
    repo = pathlib.Path(__file__).resolve().parents[1]
    files = [(p.relative_to(repo).as_posix(), p.read_text(encoding="utf-8", errors="replace"))
             for d in ("features", "patches", "framework") for p in sorted((repo / d).rglob("*")) if p.suffix in (".cpp", ".h")]
    missing = missing_sites(files)
    up_ok = up_hooks_settle((repo / "framework/d3d9_extra_hooks.cpp").read_text(encoding="utf-8"))
    for m in missing:
        print(f"FAIL {m}: shader read-back without SettleDeferredShaders before it")
    if not up_ok:
        print("FAIL framework/d3d9_extra_hooks.cpp: an UP draw hook does not settle the lazy restore")
    if missing or not up_ok:
        return 1
    print(f"deferred shader sites: ok ({sum(len(READ.findall(t)) for _, t in files)} reads checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
