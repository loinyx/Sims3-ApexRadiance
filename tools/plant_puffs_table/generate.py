#!/usr/bin/env python3
"""Bakes the leaf volumes of potted plants into features/plant_puffs_data.inc.

The game's 3D blockers of a plant (its LITE, reached through its VPXY) are two flat sheets crossed in an X: a lamp past a
plant casts a wedge with straight edges and no leaves. The plant's shadow LOD holds the real shape (pot, stem, leaf
cards). This fits that mesh with a few soft balls ("puffs"): the mesh's surface, sampled by area, split by k-means; each
ball's radius covers its samples and its density is the leaf area it holds over its cross-section, so a lamp's light
through it is exp(-tau * (1 - (d / r)^2)^1.5) at a distance d from its centre (rt_lamp_blockers.h PuffT). Models are
picked by their resource name (the packages' name maps). The game's packages are only read; a Delta package overrides a
Full one. Standard library only.

    python tools/plant_puffs_table/generate.py "<game folder>" [--check] [--list]

--check prints the counts and writes nothing; --list prints each model's puffs.
"""
import argparse
import glob
import importlib.util
import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("object_legs_generate", os.path.join(HERE, "..", "object_legs_table", "generate.py"))
legs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(legs)

NMAP = 0x0166038C
OUT = os.path.join(HERE, "..", "..", "features", "plant_puffs_data.inc")
# A plant's model name holds one of these words; the rest (food, paintings, shelves, Sims' clothes, wall art) are left out
WORDS = ("plant", "palm", "fern", "ficus", "bonsai", "flower", "topiary", "cactus", "bamboo", "lotus", "papyrus", "hosta",
         "lillies", "philodendron", "perennials", "planter", "coleus", "ivy", "bush", "shrub")
NOT = ("food", "painting", "bookshelf", "sign", "yearbook", "decal", "frames", "holographic", "body", "hair", "shoes",
       "beard", "top", "bottom", "dress", "wallflower", "ceiling")
SAMPLE = 0.004   # m^2 of mesh surface per sample
MIN_K, MAX_K = 3, 10
AREA_PER_PUFF = 0.06  # m^2 of mesh surface a puff holds at most, before MAX_K
RADIUS = 1.29    # a ball of uniform samples has an rms distance of 0.775 r
TAU_MIN, TAU_MAX = 0.1, 2.5
DENSITY = 0.6    # shadow LOD leaf cards overlap and many are two-sided: their raw area made black balls (previews of 18/11)


def names_of(game):
    names = {}
    for path in glob.glob(os.path.join(game, "**", "*.package"), recursive=True):
        if not os.path.basename(path).startswith(("FullBuild", "DeltaBuild")):
            continue
        for t, _g, _inst, off, size, _m, comp in legs.package_index(path):
            if t != NMAP:
                continue
            with open(path, "rb") as f:
                f.seek(off)
                d = f.read(size)
            d = legs.refpack(d) if comp else d
            try:
                n, o = struct.unpack_from("<I", d, 4)[0], 8
                for _ in range(n):
                    i, ln = struct.unpack_from("<QI", d, o)
                    o += 12
                    names[i] = d[o:o + ln].decode("latin-1")
                    o += ln
            except struct.error:
                pass
    return names


def is_plant(name):
    low = name.lower()
    # a table or a column that holds a plant keeps the game's blockers: its body would become puffs
    return any(w in low for w in WORDS) and not any(w in low for w in NOT) and not low.startswith(("table", "column"))


def shadow_triangles(pk, modl):
    """Every shadow mesh of the model's shadow LOD (low first) as triangles of 3 points; [] when it has none."""
    chunks, pub, ext = legs.rcol(pk.get(modl))
    lods = {i: r for r, i in legs.model_lods(next(c[3] for c in chunks if c[3][:4] == b"MODL"))}
    want = next((lods[i] for i in legs.SHADOW_LODS if i in lods), None)
    if want is None:
        return []
    r = legs.ref(want)
    if r[0] >= 2:
        key = ext[r[1]]
        if key not in pk.index:
            return []
        chunks, pub, _ = legs.rcol(pk.get(key))
    tris = []
    for m in legs.mlod_meshes(next(c[3] for c in chunks if c[3][:4] == b"MLOD")):
        if m["vrtf"] is None and m["so"]:
            p, t = legs.shadow_mesh(chunks, pub, m)
            tris += [(p[a], p[b], p[c]) for a, b, c in t]
    return tris


def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def norm(a): return math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])
def d2(a, b): return (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2


def samples(tris):
    """Points spread over the surface, each standing for the same area (a fixed barycentric grid: deterministic)."""
    out, total = [], 0.0
    for a, b, c in tris:
        area = 0.5 * norm(cross(sub(b, a), sub(c, a)))
        if area <= 0.0:
            continue
        total += area
        n = max(1, int(math.ceil(math.sqrt(area / SAMPLE))))
        w = area / (n * (n + 1) / 2)
        for i in range(n):
            for j in range(n - i):
                u, v = (i + 1 / 3) / n, (j + 1 / 3) / n
                out.append(((a[0] + (b[0] - a[0]) * u + (c[0] - a[0]) * v, a[1] + (b[1] - a[1]) * u + (c[1] - a[1]) * v,
                             a[2] + (b[2] - a[2]) * u + (c[2] - a[2]) * v), w))
    return out, total


def kmeans(pts, k):
    """Weighted k-means, farthest-point start from the heaviest-weighted centroid: the same input gives the same balls."""
    cx = sum(p[0] * w for p, w in pts) / sum(w for _, w in pts), sum(p[1] * w for p, w in pts) / sum(w for _, w in pts), \
        sum(p[2] * w for p, w in pts) / sum(w for _, w in pts)
    centres = [min(pts, key=lambda q: d2(q[0], cx))[0]]
    while len(centres) < k:
        centres.append(max(pts, key=lambda q: min(d2(q[0], c) for c in centres))[0])
    for _ in range(40):
        groups = [[] for _ in centres]
        for p, w in pts:
            groups[min(range(len(centres)), key=lambda i: d2(p, centres[i]))].append((p, w))
        new = []
        for g, c in zip(groups, centres):
            if not g:
                new.append(c)
                continue
            sw = sum(w for _, w in g)
            new.append((sum(p[0] * w for p, w in g) / sw, sum(p[1] * w for p, w in g) / sw, sum(p[2] * w for p, w in g) / sw))
        if new == centres:
            break
        centres = new
    return centres, groups


def puffs_of(tris):
    pts, total = samples(tris)
    if len(pts) < MIN_K:
        return []
    k = max(MIN_K, min(MAX_K, int(round(total / AREA_PER_PUFF))))
    centres, groups = kmeans(pts, k)
    out = []
    for c, g in zip(centres, groups):
        if not g:
            continue
        area = sum(w for _, w in g)
        rms = math.sqrt(sum(d2(p, c) * w for p, w in g) / area)
        r = max(0.03, RADIUS * rms)
        # leaves face every way: a ray sees half their area; spread over the ball's cross-section, the mean depth, and the
        # centre's 1.5 times it (the mean of sqrt(1 - rho^2) over a disc is 2/3)
        tau = max(TAU_MIN, min(TAU_MAX, DENSITY * 1.5 * (area / 2.0) / (math.pi * r * r)))
        out.append((c, r, tau))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()
    names = names_of(args.game)
    pk = legs.Packages(args.game)
    table = {}
    for key in sorted(k for k in pk.index if k[0] == legs.VPXY):
        name = names.get(key[2], "")
        if not is_plant(name):
            continue
        try:
            chunks, _, _ = legs.rcol(pk.get(key))
            links = legs.vpxy_links(chunks[0][3])
            lite = next((x for x in links if x[0] == legs.LITE and x in pk.index), None)
            modl = next((x for x in links if x[0] == legs.MODL and x in pk.index), None)
            if not lite or not modl or not legs.lite_occluders(legs.rcol(pk.get(lite))[0][0][3]):
                continue  # no blocker in the game: nothing to replace
            puffs = puffs_of(shadow_triangles(pk, modl))
        except (IndexError, KeyError, StopIteration, ValueError, struct.error, ZeroDivisionError):
            continue
        if puffs and lite[2] not in table:
            table[lite[2]] = (name, puffs)
            if args.list:
                print(name, [(tuple(round(v, 2) for v in c), round(r, 2), round(t, 2)) for c, r, t in puffs])
    for f in pk.files.values():
        f.close()
    total = sum(len(p) for _, p in table.values())
    print(f"{pk.count} packages, {len(names)} names, {len(table)} plant models, {total} puffs, {12 * len(table) + 10 * total} bytes")
    if args.check:
        return
    mm = lambda v: max(-32768, min(32767, round(v * 1000)))  # noqa: E731
    with open(OUT, "w", newline="\n") as f:
        f.write("// Generated by tools/plant_puffs_table/generate.py. Do not edit.\n")
        f.write("// x y z radius of each puff in mm of the model's space, its centre's optical depth in thousandths\n")
        f.write("inline constexpr Puff kPuffs[] = {\n")
        first, at = {}, 0
        for inst in sorted(table):
            first[inst] = at
            f.write(f"// {table[inst][0]}\n")
            for c, r, t in table[inst][1]:
                f.write("{" + ",".join(str(mm(v)) for v in (*c, r)) + f",{round(t * 1000)}}},\n")
                at += 1
        f.write("};\n// a model's LITE instance, its first puff, how many\n")
        f.write("inline constexpr PlantModel kPlantModels[] = {\n")
        for inst in sorted(table):
            f.write(f"{{0x{inst:016X}ull,{first[inst]},{len(table[inst][1])}}},\n")
        f.write("};\n")


if __name__ == "__main__":
    main()
