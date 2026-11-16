"""Compile the real-time GI shader snippets as ps_3_0 with fxc and print a cost table.

    python tools/realtime_gi_shader_cost/run.py [--fxc <fxc.exe>] [--write]

--write also stores the table in results.md next to this file. Exit code 1 when a compile or the slot self-check fails.
"""
import glob
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SLOT_LIMIT = 512  # guaranteed ps_3_0 minimum; a device may report more in MaxPixelShader30InstructionSlots
TEMP_LIMIT = 32
RUN_LIMIT = 65535  # instructions executed, the lowest value ps_3_0 hardware may report
# Host shader sizes from research note 02 section 2 (slots): typical room-lit, largest room-lit, SimSkin, roof.
HOSTS = (59, 170, 158, 343)

CELLS = {"SOURCE": 1}
# name, file, defines, attached to a game shader (True) or a pass of its own (False)
VARIANTS = [
    ("host stand-in (albedo x ambient)", "lamps.hlsl", {}, True),
    ("1 lamp, constants", "lamps.hlsl", {"LAMPS": 1}, True),
    ("1 lamp + cone", "lamps.hlsl", {"LAMPS": 1, "CONE": 1}, True),
    ("1 lamp + shadow, 1 compare", "lamps.hlsl", {"LAMPS": 1, "SHADOW": 1}, True),
    ("1 lamp + shadow, 4 taps averaged", "lamps.hlsl", {"LAMPS": 1, "SHADOW": 4}, True),
    ("1 lamp + shadow, 4 taps bilinear", "lamps.hlsl", {"LAMPS": 1, "SHADOW": 5}, True),
    ("1 lamp + cone + shadow, 4 taps bilinear", "lamps.hlsl", {"LAMPS": 1, "CONE": 1, "SHADOW": 5}, True),
    ("index cell read, 4 lamps x 2 texldl, no light", "lamps.hlsl", {**CELLS, "LAMPS": 4, "CELLONLY": 1}, True),
    ("index cells, 4 lamps", "lamps.hlsl", {**CELLS, "LAMPS": 4}, True),
    ("index cells, 4 lamps + 1 compare", "lamps.hlsl", {**CELLS, "LAMPS": 4, "SHADOW": 1}, True),
    ("index cells, 4 lamps + 4 taps bilinear", "lamps.hlsl", {**CELLS, "LAMPS": 4, "SHADOW": 5}, True),
    ("index cells, 4 lamps + cone (3 texldl) + 4 taps bilinear", "lamps.hlsl",
     {**CELLS, "LAMPS": 4, "CONE": 1, "SHADOW": 5}, True),
    ("index cells, 8 lamps + 1 compare", "lamps.hlsl", {**CELLS, "LAMPS": 8, "SHADOW": 1}, True),
    ("index cells, 8 lamps + 4 taps bilinear", "lamps.hlsl", {**CELLS, "LAMPS": 8, "SHADOW": 5}, True),
    ("index cells, 8 lamps + cone + 4 taps bilinear", "lamps.hlsl",
     {**CELLS, "LAMPS": 8, "CONE": 1, "SHADOW": 5}, True),
    ("index cells, 8 lamps + cone + 4 taps bilinear, loop", "lamps.hlsl",
     {**CELLS, "LAMPS": 8, "CONE": 1, "SHADOW": 5, "LOOPED": 1}, True),
    ("index cells, 16 lamps + cone + 4 taps bilinear, loop", "lamps.hlsl",
     {**CELLS, "LAMPS": 16, "CONE": 1, "SHADOW": 5, "LOOPED": 1}, True),
    ("compose, 16 layers, plain bilinear", "passes.hlsl", {"PASS": 1}, False),
    ("compose, 16 layers, wall-aware", "passes.hlsl", {"PASS": 1, "WALL": 1}, False),
    ("compose, 8 layers, wall-aware", "passes.hlsl", {"PASS": 1, "WALL": 1, "LAMPS": 8}, False),
    ("plan sphere trace, 8 steps, unrolled", "passes.hlsl", {"PASS": 3, "STEPS": 8}, False),
    ("plan sphere trace, 16 steps, unrolled", "passes.hlsl", {"PASS": 3, "STEPS": 16}, False),
    ("plan sphere trace, 16 steps, loop", "passes.hlsl", {"PASS": 3, "STEPS": 16, "LOOP": 1}, False),
    ("gather, 16 virtual lights, no visibility", "passes.hlsl", {"PASS": 2}, False),
    ("gather, 16 virtual lights, room table", "passes.hlsl", {"PASS": 2, "VIS": 1}, False),
    ("gather, 16 x trace 8, unrolled", "passes.hlsl", {"PASS": 2, "VIS": 2, "STEPS": 8}, False),
    ("gather, 4 x trace 8, unrolled", "passes.hlsl", {"PASS": 2, "VIS": 2, "STEPS": 8, "VPLS": 4}, False),
    ("gather, 16 x trace 8, loops", "passes.hlsl", {"PASS": 2, "VIS": 2, "STEPS": 8, "LOOP": 1}, False),
    ("gather, 16 x trace 16, loops", "passes.hlsl", {"PASS": 2, "VIS": 2, "STEPS": 16, "LOOP": 1}, False),
    ("gather, 16 x trace 8, loops, 4 directional targets", "passes.hlsl",
     {"PASS": 2, "VIS": 2, "STEPS": 8, "LOOP": 1, "DIRS": 1}, False),
    ("gather, 64 x trace 16, loops", "passes.hlsl", {"PASS": 2, "VIS": 2, "STEPS": 16, "LOOP": 1, "VPLS": 64}, False),
]

# Slots an instruction takes when it is not 1 (Direct3D 9 ps_3_0 instruction reference). fxc's own summary counts
# lrp as 1, so the table uses the count made here, which is never below fxc's.
SLOTS = {"nrm": 3, "pow": 3, "crs": 2, "sincos": 8, "lrp": 2, "dp2add": 2, "texldl": 2, "texldd": 3, "sgn": 3,
         "rep": 3, "endrep": 2, "loop": 3, "endloop": 2, "if": 3, "call": 2, "callnz": 3, "breakp": 3,
         "m4x4": 4, "m4x3": 3, "m3x4": 4, "m3x3": 3, "m3x2": 2, "label": 0}
NO_SLOT = ("ps_3_0", "def", "dcl")


def find_fxc():
    hits = sorted(glob.glob(r"C:\Program Files (x86)\Windows Kits\10\bin\*\x64\fxc.exe"),
                  key=lambda p: [int(x) for x in re.findall(r"\d+", p.split("bin")[-1])])
    return hits[-1] if hits else "fxc"


def measure(asm):
    """Counts from an fxc ps_3_0 listing."""
    m = re.search(r"approximately (\d+) instruction slots? used(?: \((\d+) texture, (\d+) arithmetic\))?", asm)
    fxc_slots, tex = int(m.group(1)), int(m.group(2) or 0)
    code = [l.strip() for l in asm.splitlines() if l.strip() and not l.strip().startswith("//")]
    reps = {mm.group(1): int(mm.group(2)) for mm in (re.match(r"defi (i\d+), (-?\d+)", l) for l in code) if mm}
    own = run = 0
    mult = [1]
    for l in code:
        op = re.match(r"[a-z0-9_]+", l).group(0)
        if op.startswith(NO_SLOT):
            continue
        base = op.split("_")[0]
        s = 3 if base in ("if", "break") and "_" in op else SLOTS.get(base, 1)
        own += s
        run += s * mult[-1]
        if base in ("rep", "loop"):
            mult.append(mult[-1] * reps[re.search(r"\bi\d+", l).group(0)])
        elif base in ("endrep", "endloop"):
            mult.pop()
    body = "\n".join(l for l in code if not l.startswith(("def", "dcl")))
    return {
        "slots": fxc_slots, "own": own, "run": run, "tex": tex,
        "temps": len(set(re.findall(r"\br(\d+)\b", body))),
        "consts": len(set(re.findall(r"\bc(\d+)\b", "\n".join(code)))),
        "samplers": len(set(re.findall(r"\bs(\d+)\b", body))),
        "inputs": sum(1 for l in code if re.match(r"dcl_\w+ v\d+", l)),
        "loops": len(reps),
    }


def self_check():
    asm = """// approximately 13 instruction slots used (1 texture, 12 arithmetic)
    ps_3_0
    def c1, 1, 0, 0, 0
    defi i0, 4, 0, 0, 0
    dcl_texcoord v0.xy
    dcl_2d s2
    mov r0, c1.x
    rep i0
      texldl r1, v0.xyyy, s2
      add r0, r0, r1
    endrep
    nrm r2.xyz, r0
    mov oC0, r2.xyzz
    """
    r = measure(asm)
    assert r["own"] == r["slots"] == 13, r
    assert r["run"] == 1 + 3 + 4 * (2 + 1 + 2) + 3 + 1, r
    assert (r["temps"], r["consts"], r["samplers"], r["inputs"], r["loops"]) == (3, 1, 1, 1, 1), r


def main():
    self_check()
    fxc = sys.argv[sys.argv.index("--fxc") + 1] if "--fxc" in sys.argv else find_fxc()
    rows, bad = [], 0
    base = None
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "o.asm")
        for name, src, defs, attached in VARIANTS:
            cmd = [fxc, "/nologo", "/T", "ps_3_0", "/E", "main", "/O3", "/Fc", out]
            for k, v in defs.items():
                cmd += ["/D", f"{k}={v}"]
            p = subprocess.run(cmd + [os.path.join(HERE, src)], capture_output=True, text=True, timeout=120)
            if p.returncode:
                err = [l for l in (p.stderr + p.stdout).splitlines() if "error" in l]
                rows.append(f"| {name} | compile failed: {err[0].split('error ', 1)[-1] if err else '?'} |||||||||")
                bad += 1
                continue
            r = measure(open(out).read())
            if r["own"] >= r["slots"]:
                r["slots"] = r["own"]
            else:
                bad += 1
                print(f"slot self-check: {name}: counted {r['own']}, fxc says {r['slots']}", file=sys.stderr)
            if base is None:
                base = r["slots"]
            ok = r["temps"] <= TEMP_LIMIT and r["run"] <= RUN_LIMIT
            if attached:
                add = r["slots"] - base
                fits = [h for h in HOSTS if h + add <= SLOT_LIMIT]
                verdict = ("fits every host" if len(fits) == len(HOSTS) else
                           "fits hosts up to %d slots" % max(fits) if fits else "too large for any host")
                delta = f"+{add}"
            else:
                verdict = "fits" if r["slots"] <= SLOT_LIMIT else "over 512 slots"
                delta = "-"
            if not ok:
                verdict = "over the temporaries" if r["temps"] > TEMP_LIMIT else "over the run limit"
            rows.append(f"| {name} | {r['slots']} | {delta} | {r['run']} | {r['tex']} | {r['temps']} | {r['consts']} "
                        f"| {r['samplers']} | {r['inputs']} | {verdict} |")
    ver = re.search(r"Shader Compiler ([\d.]+)",
                    subprocess.run([fxc, "/?"], capture_output=True, text=True, timeout=30).stdout)
    table = "\n".join([
        f"Compiler: fxc {ver.group(1) if ver else '?'}, `/T ps_3_0 /O3`. Limits: {SLOT_LIMIT} slots, {TEMP_LIMIT} "
        f"temporaries, 16 samplers, 224 constants, 10 inputs, {RUN_LIMIT} instructions run.",
        f"Hosts for the verdict of attached snippets: {', '.join(map(str, HOSTS))} slots.",
        "",
        "| Snippet | Slots | Added to host | Run | Texture reads | Temporaries | Constants | Samplers | Inputs | Verdict |",
        "|---|---|---|---|---|---|---|---|---|---|",
    ] + rows) + "\n"
    print(table)
    if "--write" in sys.argv:
        with open(os.path.join(HERE, "results.md"), "w", newline="\n") as f:
            f.write("# Real-time GI shader cost: results\n\nWritten by `run.py --write`.\n\n" + table)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
