"""Generates the GShade looks shader table for Apex Radiance from the GShade .fx files (read only).

For each shader in SPECS it:
  - copies the .fx, drops #include "ReShade.fxh", texture/sampler declarations and techniques,
  - turns every `uniform T name <...> = def;` into `T name : register(cN);` (int/bool become float),
  - maps samplers to s registers, applies the spec's few textual fixes (listed per shader, maths untouched),
  - appends ps_3_0 entry wrappers (VPOS + 0.5 = SV_Position pixel centre, float4 COLOR0 output),
and writes:
  features/gshade/hlsl/<Name>.hlsl   (readable result, compiled with fxc by this script as a check)
  features/gshade/gs_generated.h      (HLSL pieces + parameter tables for C++)

Adding a shader = add one SPECS entry here + one run function in gs_runner.cpp. Run:
  python -I tools/gen_gshade_shaders.py
"""
import os, re, subprocess, sys

SHADERS = os.environ.get("GSHADE_SHADERS", r"C:\Program Files\GShade\gshade-shaders\Shaders")
APEX = os.environ.get("APEX_SRC", os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
OUT_DIR = os.path.join(APEX, "features", "gshade")
FXC = os.environ.get("FXC", r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x64\fxc.exe")
FIRST_PARAM_REG = 8

WRAP = {
    "rgb": "return float4({call}, 1.0);",
    "r":   "float v = {call}; return float4(v, v, v, 1.0);",
    "rg":  "return float4({call}, 0.0, 1.0);",
    "rgba": "float4 c = {call}; return float4(c.rgb, 1.0);",  # passes that return float4 (alpha unused: RGB-only copy)
}

SPECS = [
    dict(name="FilmicPass", technique="FilmicPass", file="FilmicPass.fx", entries=[("FilmPass", "rgb")]),
    dict(name="Levels", technique="Levels", file="Levels.fx", entries=[("LevelsPass", "rgb")]),
    dict(name="Technicolor2", technique="Technicolor2", file="Technicolor2.fx", entries=[("TechnicolorPass", "rgb")]),
    dict(name="RetroTint", technique="RetroTint", file="RetroTint.fx", entries=[("RetroTintPS", "rgb")]),
    dict(name="Clarity", technique="Clarity", file="Clarity.fx",
         samplers={"ClaritySampler": 1, "ClaritySampler2": 2, "ClaritySampler3": 3},
         # ps_3_0 cannot index the local offset/weight arrays in a real loop; unrolling gives the same sums
         fixes=[(r"\[loop\]", "[unroll]")],
         entries=[("Clarity1", "r"), ("Clarity2", "r"), ("Clarity3", "r"), ("ClarityFinal", "rgb")]),
    dict(name="DPX", technique="DPX", file="DPX.fx", entries=[("DPXPass", "rgb")]),
    dict(name="AdaptiveSharpen", technique="AdaptiveSharpen", file="AdaptiveSharpen.fx",
         samplers={"AS_Pass0Sampler": 1},
         entries=[("AdaptiveSharpenP0", "rg"), ("AdaptiveSharpenP1", "rgb")]),
    # ---- one-pass colour and sharpen shaders ----
    dict(name="Vibrance", technique="Vibrance", file="Vibrance.fx", entries=[("VibrancePass", "rgb")]),
    dict(name="Tint", technique="Tint", file="Sepia.fx", entries=[("TintPass", "rgb")]),
    dict(name="Tonemap", technique="Tonemap", file="Tonemap.fx", entries=[("TonemapPass", "rgb")]),
    dict(name="Colourfulness", technique="Colourfulness", file="Colourfulness.fx", entries=[("Colourfulness", "rgb")]),
    dict(name="LumaSharpen", technique="LumaSharpen", file="LumaSharpen.fx", entries=[("LumaSharpenPass", "rgb")]),
    dict(name="HighPassSharp", technique="HighPassSharp", file="HighPassSharpen.fx", entries=[("SharpBlurFinal", "rgb")]),
    dict(name="CAS", technique="ContrastAdaptiveSharpen", file="CAS.fx", entries=[("CASPass", "rgb")],
         aliases={"sTexCASColor": "BackBuffer"}, srgb=True),  # sRGB sampler on COLOR + SRGBWriteEnable, as in the .fx
    dict(name="Curves", technique="Curves", file="Curves.fx", entries=[("CurvesPass", "rgba")]),
    dict(name="Vignette", technique="Vignette", file="Vignette.fx", entries=[("VignettePass", "rgba")]),
    dict(name="Technicolor", technique="Technicolor", file="Technicolor.fx", entries=[("TechnicolorPass", "rgb")]),
    dict(name="FakeHDR", technique="FakeHDR", file="FakeHDR.fx", entries=[("fHDRPass", "rgb")]),
    dict(name="PD80SelectiveColor", technique="prod80_04_SelectiveColor", file="PD80_04_Selective_Color.fx",
         entries=[("pd80_selectivecolor::PS_SelectiveColor", "rgba")]),
    dict(name="LiftGammaGain", technique="LiftGammaGain", file="LiftGammaGain.fx", entries=[("LiftGammaGainPass", "rgb")]),
]

UNIFORM_RE = re.compile(r"uniform\s+(\w+)\s+(\w+)\s*(<.*?>)?\s*(?:=\s*(.*?))?;", re.S)


def strip_block(src, start_re):
    """Removes `start { ... }` blocks (brace matched)."""
    while True:
        m = re.search(start_re, src)
        if not m:
            return src
        i = src.index("{", m.end() - 1)
        depth = 0
        for j in range(i, len(src)):
            if src[j] == "{":
                depth += 1
            elif src[j] == "}":
                depth -= 1
                if depth == 0:
                    break
        src = src[:m.start()] + src[j + 1:]


def ann(a, key):
    m = re.search(key + r"\s*=\s*(\"(?:[^\"\\]|\\.)*\"|[^;]+);", a or "")
    return m.group(1).strip() if m else None


def parse_default(text, comps):
    t = (text or "0").strip()
    if t in ("true", "false"):
        return [1.0 if t == "true" else 0.0]
    nums = re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", t)
    vals = [float(n) for n in nums]
    if re.match(r"float\d\(", t):
        vals = vals[1:]  # skip the N of floatN
    if len(vals) == 1 and comps > 1:
        vals = vals * comps
    return vals[:comps]


LIB_INCLUDES = ("ReShade.fxh", "qUINT_common.fxh", "ReShadeUI.fxh", "TriDither.fxh")


def inline_includes(src, folder, seen):
    """Inlines a shader's own headers (e.g. PD80_00_Base_Effects.fxh); library headers are dropped
    (their pieces live in gs_common.hlsl; TriDither is off: GShade.ini UseDithering=0)."""
    def inc(m):
        name = m.group(1)
        if os.path.basename(name) in LIB_INCLUDES:
            return ""
        path = os.path.join(folder, name)
        if name in seen:
            return ""
        seen.add(name)
        text = open(path, encoding="utf-8", errors="replace").read().replace(chr(13) + chr(10), chr(10))
        return "// ---- inlined %s ----\n%s\n" % (name, inline_includes(text, os.path.dirname(path), seen))
    return re.sub(r'^[ \t]*#include\s+"([^"]+)"', inc, src, flags=re.M)


def convert(spec):
    src = open(os.path.join(SHADERS, spec["file"]), encoding="utf-8", errors="replace").read()
    src = src.replace(chr(13) + chr(10), chr(10))
    src = inline_includes(src, SHADERS, set())
    src = strip_block(src, r"\btechnique\s+\w+[^{]*\{")
    # a // comment ending in a backslash would swallow the next line in fxc (not in ReShade's preprocessor)
    src = re.sub(r"(//[^\n]*?)[\\]+[ \t]*$", r"\1", src, flags=re.M)
    # Apex's public-text check reads two-digit a/b numbers in comments as dates (e.g. 37/15): space them out
    src = re.sub(r"//[^\n]*", lambda m: re.sub(r"\b([0-3]\d)/([01]\d)\b", r"\1 / \2", m.group(0)), src)
    src = re.sub(r"^[ \t]*(texture2?D?)\s+\w+[^{\n]*\{[^}]*\}\s*;", "", src, flags=re.M)
    src = re.sub(r"\bin sampler (\w+)", r"in sampler2D \1", src)  # ReShade 'sampler' parameter = sampler2D
    samplers = spec.get("samplers", {})
    def samp(m):
        name = m.group(2)
        if name in samplers:
            return "sampler2D %s : register(s%d);" % (name, samplers[name])
        return ""
    src = re.sub(r"^[ \t]*(sampler2?D?)\s+(\w+)\s*\{[^}]*\}\s*;", samp, src, flags=re.M)
    for k, v in spec.get("aliases", {}).items():
        src = re.sub(r"\b%s\b" % k, v, src)
    src = src.replace("ReShade::BackBuffer", "BackBuffer").replace("qUINT::", "qUINT_")
    params = []
    reg = [FIRST_PARAM_REG]
    def uni(m):
        typ, name, a, dflt = m.group(1), m.group(2), m.group(3), m.group(4)
        source = ann(a, "source")
        if source:  # filled by the runner every frame (timer, framecount, frametime, random)
            source = source.strip('"')
            if source not in ("timer", "framecount", "frametime", "random"):
                raise SystemExit("%s: uniform %s has source %s, not supported yet" % (spec["name"], name, source))
            p = dict(key=name, comps=1, reg=reg[0], kind="Source", default=[0.0], min=ann(a, "min"),
                     max=ann(a, "max"), label='"%s"' % name, items='"%s"' % source)
            params.append(p)
            reg[0] += 1
            return "float %s : register(c%d);" % (name, p["reg"])
        comps = int(typ[-1]) if typ[-1].isdigit() else 1
        hl = "float%s" % (comps if comps > 1 else "")
        kind = "Bool" if typ == "bool" else "Int" if typ == "int" else "Float"
        uitype = (ann(a, "ui_type") or "").strip('"')
        items = ann(a, "ui_items")
        if items:
            kind = "Combo"
        elif uitype == "color":
            kind = "Color"
        p = dict(key=name, comps=comps, reg=reg[0], kind=kind, default=parse_default(dflt, comps),
                 min=ann(a, "ui_min"), max=ann(a, "ui_max"), label=(ann(a, "ui_label") or '"%s"' % name),
                 items=items)
        params.append(p)
        reg[0] += 1
        return "%s %s : register(c%d);" % (hl, name, p["reg"])
    src = UNIFORM_RE.sub(uni, src)
    for pat, rep in spec.get("fixes", []):
        new = re.sub(pat, rep, src, flags=re.S)
        if new == src:
            raise SystemExit("%s: fix %r matched nothing" % (spec["name"], pat))
        src = new
    wrappers = []
    entries = []
    for fn, kind in spec["entries"]:
        call = "%s(float4(vp + 0.5, 0.0, 1.0), uv)" % fn
        wname = "gs_" + fn.replace("::", "_")  # namespaced entries (PD80) get a plain wrapper name
        wrappers.append("float4 %s(float2 vp : VPOS, float2 uv : TEXCOORD0) : COLOR0 { %s }"
                        % (wname, WRAP[kind].format(call=call)))
        entries.append(wname)
    if spec.get("custom"):
        wrappers.append(spec["custom"])
        entries += spec["custom_entries"]
    body = src.strip() + "\n\n// ---- ps_3_0 entry points (GShade looks) ----\n" + "\n".join(wrappers) + "\n"
    return body, params, entries


def cpp_str_pieces(text, size=12000):
    out, i = [], 0
    while i < len(text):
        piece = text[i:i + size]
        out.append('R"GSHLSL(%s)GSHLSL"' % piece)
        i += size
    return out


def cnum(v):
    s = repr(float(str(v).strip().rstrip("fF")))
    return s + "f" if "e" in s or "." in s else s + ".0f"


def main():
    common = open(os.path.join(OUT_DIR, "gs_common.hlsl"), encoding="utf-8").read()
    os.makedirs(os.path.join(OUT_DIR, "hlsl"), exist_ok=True)
    h = ["// Generated by tools/gen_gshade_shaders.py from the GShade .fx files. Do not edit by hand.",
         "#pragma once", '#include "gs_types.h"', "", "namespace GShadeGen {", ""]
    h.append("static const char* const kCommon[] = { %s, nullptr };" % ", ".join(cpp_str_pieces(common)))
    table = []
    ok = True
    for spec in SPECS:
        body, params, entries = convert(spec)
        full = common + body
        path = os.path.join(OUT_DIR, "hlsl", spec["name"] + ".hlsl")
        open(path, "w", encoding="utf-8", newline="\n").write(
            "// Generated from %s by tools/gen_gshade_shaders.py\n%s" % (spec["file"], full))
        for e in entries:
            r = subprocess.run([FXC, "/nologo", "/T", "ps_3_0", "/E", e, "/O3", "/Fo", os.devnull, path],
                               capture_output=True, text=True)
            status = "ok" if r.returncode == 0 else "FAILED"
            print("%-16s %-24s %s" % (spec["name"], e, status))
            if r.returncode:
                ok = False
                print(r.stderr[-3000:])
        n = spec["name"]
        h.append("static const char* const k%sSrc[] = { %s, nullptr };" % (n, ", ".join(cpp_str_pieces(body))))
        h.append("static const char* const k%sEntries[] = { %s, nullptr };" % (n, ", ".join('"%s"' % e for e in entries)))
        rows = []
        for p in params:
            d = (p["default"] + [0.0] * 4)[:4]
            mn = cnum(p["min"]) if p["min"] else "0.0f"
            mx = cnum(p["max"]) if p["max"] else ("1.0f" if p["kind"] in ("Bool", "Color") else "1.0f")
            items = "nullptr"
            if p["kind"] == "Source":
                items = p["items"]
            elif p["items"]:
                items = re.sub(r'[\\](?![0n"\\])', '', p["items"])  # drop stray escapes like "\Soft Light"
                count = len([x for x in items.strip('"').split(chr(92) + "0") if x])
                mn, mx = "0.0f", cnum(count - 1)
            rows.append('    { "%s", %s, %d, %d, GsKind::%s, { %s }, %s, %s, %s },' % (
                p["key"], p["label"], p["comps"], p["reg"], p["kind"], ", ".join(cnum(x) for x in d), mn, mx, items))
        h.append("static const GsParamDesc k%sParams[] = {\n%s\n};" % (n, "\n".join(rows) if rows else "    {}"))
        table.append('    { "%s", "%s", k%sSrc, k%sEntries, k%sParams, %d, %s, %s },' % (
            spec["technique"], spec["file"], n, n, n, len(params), "true" if spec.get("depth") else "false",
            "true" if spec.get("srgb") else "false"))
    h.append("")
    h.append("static const GsShaderDesc kShaders[] = {\n%s\n};" % "\n".join(table))
    h.append("static const int kShaderCount = %d;" % len(SPECS))
    h.append("} // namespace GShadeGen")
    open(os.path.join(OUT_DIR, "gs_generated.h"), "w", encoding="utf-8", newline="\n").write("\n".join(h) + "\n")
    print("ok" if ok else "SOME SHADERS FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
