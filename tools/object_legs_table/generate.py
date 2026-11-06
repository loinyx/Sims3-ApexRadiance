#!/usr/bin/env python3
"""Bakes the legs of tables into features/object_legs_data.inc.

The game's 3D blockers come from each model's LITE resource (type 0x03B4C61D, reached through its VPXY), and a table whose
LITE lists only a top casts no leg. Its real legs are in the model's shadow LOD: this slices that mesh from the floor up to the
underside of the top and keeps each leg as a chain of capsules, in the model's space. Only models with no upright occluder
qualify: the others already have a body. The game's packages are only read; a Delta package overrides a Full one. Standard
library only.

    python tools/object_legs_table/generate.py "<game folder>" [--check]

--check prints the counts and writes nothing.
"""
import argparse
import collections
import glob
import importlib.util
import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("opening_mask_generate", os.path.join(HERE, "..", "opening_mask_table", "generate.py"))
_masks = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_masks)
package_index, refpack = _masks.package_index, _masks.refpack

MODL, LITE, VPXY = 0x01661233, 0x03B4C61D, 0x736884F1
SHADOW_LODS = (0x10001, 0x10000)  # shadow low, shadow high: the low one is enough at a floor texel of 12.5 cm
OPT_SCALE, OPT_OFFSET = 0x487648E5, 0x790EBF2C  # the shadow mesh's scale-offset material parameters
STEP = 0.05          # slices apart (m)
MATCH = 0.08         # a leg's section moves at most this between slices (m)
JOIN = 0.02          # section outline ends this close are one section (m)
BEND = 0.025         # a chain follows its leg to this (m): tighter adds segments, not shade, under a 12.5 cm texel
MIN_SECTIONS = 3     # a detail shorter than 15 cm is no leg
TOP_MIN, TOP_MAX = 0.2, 1.3  # a table's underside: lower is a rug, higher an awning or a shelf
FOOT = 0.15          # a model keeps its chains only with one reaching this close to the floor
OUT = os.path.join(HERE, "..", "..", "features", "object_legs_data.inc")


def rcol(d):
    """([(type, group, instance, bytes)] of its chunks, public count, [external (type, group, instance)]) of an RCOL resource."""
    _ver, pub, _unused, ne, ni = struct.unpack_from("<5I", d, 0)
    o, its, ext, chunks = 20, [], [], []
    for _ in range(ni):
        inst, t, g = struct.unpack_from("<QII", d, o)
        o += 16
        its.append((t, g, inst))
    for _ in range(ne):
        inst, t, g = struct.unpack_from("<QII", d, o)
        o += 16
        ext.append((t, g, inst))
    for t, g, inst in its:
        p, s = struct.unpack_from("<II", d, o)
        o += 8
        chunks.append((t, g, inst, d[p:p + s]))
    return chunks, pub, ext


def ref(v):
    return (v >> 28, (v & 0x0FFFFFFF) - 1) if v else None


def vpxy_links(b):
    off = struct.unpack_from("<I", b, 8)[0]
    p = 12 + off
    links = []
    for i in range(struct.unpack_from("<I", b, p)[0]):
        t, g, lo, hi = struct.unpack_from("<4I", b, p + 4 + 16 * i)
        links.append((t, g, hi << 32 | lo))
    return links


def lite_occluders(b):
    """[(type, 13 floats)]: origin, normal, then the shape's own."""
    if b[:4] != b"LITE":
        raise ValueError("no LITE")
    o = 16 + 128 * b[12]
    out = []
    for _ in range(b[13]):
        out.append((struct.unpack_from("<I", b, o)[0], struct.unpack_from("<13f", b, o + 4)))
        o += 56
    return out


def model_lods(b):
    """[(reference, id)] of a MODL chunk: its LOD entries are its last 20 bytes each."""
    n = struct.unpack_from("<I", b, 8)[0]
    o = len(b) - 20 * n
    return [(struct.unpack_from("<I", b, o + 20 * i)[0], struct.unpack_from("<I", b, o + 20 * i + 8)[0]) for i in range(n)]


def mlod_meshes(b):
    if b[:4] != b"MLOD":
        raise ValueError("no MLOD")
    n = struct.unpack_from("<I", b, 8)[0]
    o, meshes = 12, []
    for _ in range(n):
        size = struct.unpack_from("<I", b, o)[0]
        p = o + 4
        _name, _mat, vr, vb, ib, _val, soff, sv, si, _mv, vc, pc = struct.unpack_from("<12I", b, p)
        p += 72
        p += 8 + 4 * struct.unpack_from("<I", b, p + 4)[0]
        so = struct.unpack_from("<I", b, p)[0]
        meshes.append(dict(vrtf=ref(vr), vbuf=ref(vb), ibuf=ref(ib), so=ref(so), soff=soff, sv=sv, si=si, vc=vc, pc=pc))
        o += 4 + size
    return meshes


def chunk(chunks, pub, r):
    return chunks[r[1] + (pub if r[0] == 1 else 0)][3]


def indices(b):
    _ver, fl, _dl = struct.unpack_from("<III", b, 4)
    n = (len(b) - 16) // (4 if fl & 2 else 2)
    v = struct.unpack_from(f"<{n}{'i' if fl & 2 else 'h'}", b, 16)
    if not fl & 1:
        return list(v)
    out, cur = [], 0
    for x in v:
        cur += x
        out.append(cur)
    return out


def material_floats(b):
    p = b.find(b"MTNF")
    if p < 0:
        return {}
    o, out = p + 16, {}
    for _ in range(struct.unpack_from("<I", b, p + 12)[0]):
        h, ty, c, off = struct.unpack_from("<4I", b, o)
        o += 16
        if ty == 1:
            out[h] = struct.unpack_from(f"<{min(c, 4)}f", b, p + off)
    return out


def shadow_mesh(chunks, pub, m):
    """(positions, triangles) of a shadow mesh: 8 B a vertex, four int16 through its material's scale and offset."""
    mat = material_floats(chunk(chunks, pub, m["so"]))
    sc, of = mat[OPT_SCALE], mat[OPT_OFFSET]
    vb = chunk(chunks, pub, m["vbuf"])
    pos = []
    for i in range(m["vc"]):
        x, y, z, _w = struct.unpack_from("<4h", vb, 16 + (m["sv"] + i) * 8 + m["soff"])
        pos.append((x * sc[0] + of[0], y * sc[1] + of[1], z * sc[2] + of[2]))
    idx = indices(chunk(chunks, pub, m["ibuf"]))[m["si"]:m["si"] + m["pc"] * 3]
    return pos, [tuple(idx[i:i + 3]) for i in range(0, len(idx) - 2, 3)]


def sections(p, t, y):
    """The mesh cut at height y: its outline pieces joined into sections, each section's (x0, x1, z0, z1)."""
    seg = []
    for tri in t:
        v = [p[i] for i in tri]
        pts = []
        for i in range(3):
            u, w = v[i], v[(i + 1) % 3]
            if (u[1] - y) * (w[1] - y) < 0:
                s = (y - u[1]) / (w[1] - u[1])
                pts.append((u[0] + s * (w[0] - u[0]), u[2] + s * (w[2] - u[2])))
        if len(pts) == 2:
            seg.append(pts)
    par = list(range(len(seg)))

    def find(x):
        while par[x] != x:
            par[x] = par[par[x]]
            x = par[x]
        return x

    for i in range(len(seg)):
        for j in range(i):
            if any(math.dist(a, b) < JOIN for a in seg[i] for b in seg[j]):
                par[find(i)] = find(j)
    groups = collections.defaultdict(list)
    for i, s in enumerate(seg):
        groups[find(i)] += s
    return sorted((min(q[0] for q in g), max(q[0] for q in g), min(q[1] for q in g), max(q[1] for q in g)) for g in groups.values())


def chain(track):
    """A leg's sections (x, y, z, r) from the bottom up as capsules (x0, y0, z0, x1, y1, z1, r): split where the chord leaves it most."""
    pts = [track[0], track[-1]]
    while len(pts) < 4:
        worst = None
        for q in track:
            for a, b in zip(pts, pts[1:]):
                if a[1] <= q[1] <= b[1]:
                    s = (q[1] - a[1]) / max(b[1] - a[1], 1e-6)
                    d = math.hypot(q[0] - (a[0] + s * (b[0] - a[0])), q[2] - (a[2] + s * (b[2] - a[2])))
                    if worst is None or d > worst[0]:
                        worst = (d, q)
        if worst is None or worst[0] < BEND:
            break
        pts.append(worst[1])
        pts.sort(key=lambda v: v[1])
    r = sum(q[3] for q in track) / len(track)
    return [a[:3] + b[:3] + (r,) for a, b in zip(pts, pts[1:])]


def legs(p, t, top):
    """The capsules of a mesh under its top at height `top`: each slice's sections follow the nearest of the slice below."""
    open_tracks, done = [], []
    y = 0.03
    while y < top - 0.03:
        cs = [((a + b) / 2, y, (c + d) / 2, (b - a + d - c) / 4) for a, b, c, d in sections(p, t, y)]
        y += STEP
        nxt, used = [], set()
        for k in open_tracks:
            last, best = k[-1], None
            for i, c in enumerate(cs):
                d = math.hypot(c[0] - last[0], c[2] - last[2])
                if i not in used and d < MATCH and (best is None or d < best[0]):
                    best = (d, i)
            if best:
                used.add(best[1])
                k.append(cs[best[1]])
                nxt.append(k)
            else:
                done.append(k)
        nxt += [[c] for i, c in enumerate(cs) if i not in used]
        open_tracks = nxt
    out = []
    for k in done + open_tracks:
        if len(k) >= MIN_SECTIONS:
            out += chain(k)
    return out if any(s[1] < FOOT for s in out) else []


def top_height(oc):
    """The height of a model's level tops, or None when it has an upright occluder (a body) or no top."""
    if not oc or any(abs(f[4]) < 0.7 for _, f in oc):
        return None
    tops = [f[1] for _, f in oc if abs(f[4]) > 0.95]
    return min(tops) if tops else None


class Packages:
    def __init__(self, game):
        packs = [p for p in glob.glob(os.path.join(game, "**", "*.package"), recursive=True) if os.path.basename(p).startswith(("FullBuild", "DeltaBuild"))]
        packs.sort(key=lambda p: (os.path.basename(p).startswith("DeltaBuild"), p.lower()))  # (the last one read wins)
        self.index, self.files, self.count = {}, {}, len(packs)
        for path in packs:
            for t, g, inst, off, size, _mem, comp in package_index(path):
                if t in (MODL, LITE, VPXY) or t == 0x01D10F34:
                    self.index[(t, g, inst)] = (path, off, size, comp)

    def get(self, key):
        path, off, size, comp = self.index[key]
        f = self.files.get(path) or self.files.setdefault(path, open(path, "rb"))
        f.seek(off)
        d = f.read(size)
        return refpack(d) if comp else d


def model_legs(pk, modl, top):
    """The legs of a model from its lowest shadow LOD's largest mesh; [] when it has none."""
    chunks, pub, ext = rcol(pk.get(modl))
    lods = {i: r for r, i in model_lods(next(c[3] for c in chunks if c[3][:4] == b"MODL"))}
    want = next((lods[i] for i in SHADOW_LODS if i in lods), None)
    if want is None:
        return []
    r = ref(want)
    if r[0] >= 2:
        key = ext[r[1]]
        if key not in pk.index:
            return []
        chunks, pub, _ = rcol(pk.get(key))
    best = None
    for m in mlod_meshes(next(c[3] for c in chunks if c[3][:4] == b"MLOD")):
        if m["vrtf"] is None and m["so"] and (best is None or m["pc"] > best["pc"]):
            best = m
    return legs(*shadow_mesh(chunks, pub, best), top) if best else []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    pk = Packages(args.game)
    table, tally = {}, collections.Counter()
    for key in sorted(k for k in pk.index if k[0] == VPXY):
        try:
            chunks, _, _ = rcol(pk.get(key))
            links = vpxy_links(chunks[0][3])
        except (IndexError, struct.error):
            tally["VPXY without links"] += 1
            continue
        lite = next((x for x in links if x[0] == LITE and x in pk.index), None)
        modl = next((x for x in links if x[0] == MODL and x in pk.index), None)
        if not lite or not modl:
            continue
        try:
            top = top_height(lite_occluders(rcol(pk.get(lite))[0][0][3]))
            if top is None or not TOP_MIN <= top <= TOP_MAX:
                continue
            tally["top-only models"] += 1
            segs = model_legs(pk, modl, top)
        except (IndexError, KeyError, StopIteration, ValueError, struct.error, ZeroDivisionError):
            tally["unreadable model"] += 1
            continue
        if segs:
            table.setdefault(lite[2], segs)
    for f in pk.files.values():
        f.close()
    mm = lambda v: max(-32768, min(32767, round(v * 1000)))  # noqa: E731
    total = sum(len(s) for s in table.values())
    print(f"{pk.count} packages, {dict(tally)}, {len(table)} models with legs, {total} capsules, {12 * len(table) + 14 * total} bytes")
    if args.check:
        return
    with open(OUT, "w", newline="\n") as f:
        f.write("// Generated by tools/object_legs_table/generate.py. Do not edit.\n")
        f.write("// x0 y0 z0 x1 y1 z1 radius of each capsule, in mm of the model's space\n")
        f.write("inline constexpr Capsule kCapsules[] = {\n")
        first = {}
        at = 0
        for inst in sorted(table):
            first[inst] = at
            for s in table[inst]:
                f.write("{" + ",".join(str(mm(v)) for v in s) + "},\n")
                at += 1
        f.write("};\n// a model's LITE instance, its first capsule, how many\n")
        f.write("inline constexpr Model kModels[] = {\n")
        for inst in sorted(table):
            f.write(f"{{0x{inst:016X}ull,{first[inst]},{len(table[inst])}}},\n")
        f.write("};\n")


if __name__ == "__main__":
    main()
