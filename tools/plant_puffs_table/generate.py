#!/usr/bin/env python3
"""Bakes the leaf volumes of potted plants into features/plant_puffs_data.inc.

The game's 3D blockers of a plant (its LITE, reached through its VPXY) are two flat sheets crossed in an X: a lamp past a
plant casts a wedge with straight edges and no leaves. The plant's shadow LOD holds the real shape (pot, stem, leaf
cards). This fits that mesh with a few soft balls ("puffs"): the mesh's surface, sampled by area, split by k-means; each
ball's radius covers its samples and its density is the leaf area it holds over its cross-section, so a lamp's light
through it is exp(-tau * (1 - (d / r)^2)^1.5) at a distance d from its centre (rt_lamp_blockers.h PuffT). Models are
picked by their blockers (two crossed upright sheets) or their resource name (the packages' name maps). The game's packages are only read; a Delta package overrides a
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
# A model is baked when its LITE is the game's two vertical sheets crossed in an X (the rough blocker of plants and of other
# objects of that build; a ball of its real mesh shades better than the wedge for every one of them, and custom content
# without names keeps that shape), or when its name holds one of these words and none of the next (food, paintings, shelves
# and signs with a plant in their name)
WORDS = ("plant", "palm", "fern", "ficus", "bonsai", "flower", "topiary", "cactus", "bamboo", "lotus", "papyrus", "hosta",
         "lillies", "philodendron", "perennials", "planter", "coleus", "ivy", "bush", "shrub", "orchid", "daffodil", "tulip",
         "grass", "reed", "cattail", "succulent", "leafy", "tree")
NOT = ("food", "painting", "bookshelf", "sign", "yearbook", "decal", "frames", "holographic")
SAMPLE = 0.004   # m^2 of mesh surface per sample
MAX_K = 10
AREA_PER_PUFF = 0.06  # m^2 of mesh surface a puff holds at most, before MAX_K
RADIUS = 1.29    # a ball of uniform samples has an rms distance of 0.775 r
TAU_MIN, TAU_MAX = 0.1, 2.5
DENSITY = 0.6    # shadow LOD leaf cards overlap and many are two-sided: their raw area made black balls (previews of 18/11)
SOLID_M = 0.05   # a mesh piece this thick (3 x volume / area) is a pot, a box or a table, not leaves (leaves 0 to 0.8 cm, ficus
                 # leaf clumps 3 to 5 cm, pots and planters 6 to 21 cm in the base game's meshes)
TAU_SOLID = 6.0  # its balls: 0.25% of the light through the centre, as a solid blocker
MAX_SOLID_K = 3


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


def crossed_sheets(occluders):
    """Two occluders, both upright (normal's y under 0.1), at right angles (|dot| under 0.2)."""
    n = [o[1][3:6] for o in occluders if abs(o[1][4]) < 0.1]
    return len(occluders) == 2 and len(n) == 2 and abs(n[0][0] * n[1][0] + n[0][1] * n[1][1] + n[0][2] * n[1][2]) < 0.2


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


def plain_sum(values):
    """Left-to-right sum (Python 3.12's sum() compensates float rounding; plant_scan.cpp adds plainly, and a symmetric model's
    k-means tie went the other way)."""
    t = 0.0
    for v in values:
        t += v
    return t


def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def norm(a): return math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])
def d2(a, b): return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])


def solid_pieces(tris):
    """For each triangle, whether its connected piece (vertices welded at 1 mm) is at least SOLID_M thick."""
    key = lambda q: (round(q[0] * 1000), round(q[1] * 1000), round(q[2] * 1000))  # noqa: E731
    parent = {}

    def find(x):
        while parent.setdefault(x, x) != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    for a, b, c in tris:
        ra = find(key(a))
        parent[find(key(b))] = ra
        parent[find(key(c))] = ra
    area, vol = {}, {}
    for a, b, c in tris:
        r = find(key(a))
        area[r] = area.get(r, 0.0) + 0.5 * norm(cross(sub(b, a), sub(c, a)))
        vol[r] = vol.get(r, 0.0) + (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0
    return [3.0 * abs(vol[find(key(a))]) / max(area[find(key(a))], 1e-9) >= SOLID_M for a, _, _ in tris]


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
    cx = plain_sum(p[0] * w for p, w in pts) / plain_sum(w for _, w in pts), plain_sum(p[1] * w for p, w in pts) / plain_sum(w for _, w in pts), \
        plain_sum(p[2] * w for p, w in pts) / plain_sum(w for _, w in pts)
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
            sw = plain_sum(w for _, w in g)
            new.append((plain_sum(p[0] * w for p, w in g) / sw, plain_sum(p[1] * w for p, w in g) / sw, plain_sum(p[2] * w for p, w in g) / sw))
        if new == centres:
            break
        centres = new
    return centres, groups


def balls(tris, solid, max_k):
    pts, total = samples(tris)
    if not pts:
        return []
    k = max(1, min(max_k, len(pts), int(round(total / AREA_PER_PUFF))))
    centres, groups = kmeans(pts, k)
    out = []
    for c, g in zip(centres, groups):
        if not g:
            continue
        area = plain_sum(w for _, w in g)
        rms = math.sqrt(plain_sum(d2(p, c) * w for p, w in g) / area)
        r = max(0.03, RADIUS * rms)
        # leaves face every way: a ray sees half their area; spread over the ball's cross-section, the mean depth, and the
        # centre's 1.5 times it (the mean of sqrt(1 - rho^2) over a disc is 2/3). A solid piece blocks like the game's blockers
        tau = TAU_SOLID if solid else max(TAU_MIN, min(TAU_MAX, DENSITY * 1.5 * (area / 2.0) / (math.pi * r * r)))
        out.append((c, r, tau))
    return out


def puffs_of(tris):
    """The solid pieces' balls (at most MAX_SOLID_K), then the leaves' (the rest of MAX_K), clustered apart."""
    if not tris:
        return []
    flags = solid_pieces(tris)
    hard = [t for t, f in zip(tris, flags) if f]
    soft = [t for t, f in zip(tris, flags) if not f]
    out = balls(hard, True, MAX_SOLID_K)
    return out + balls(soft, False, MAX_K - len(out))


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
        try:
            chunks, _, _ = legs.rcol(pk.get(key))
            links = legs.vpxy_links(chunks[0][3])
            lite = next((x for x in links if x[0] == legs.LITE and x in pk.index), None)
            modl = next((x for x in links if x[0] == legs.MODL and x in pk.index), None)
            if not lite or not modl:
                continue
            occluders = legs.lite_occluders(legs.rcol(pk.get(lite))[0][0][3])
            if not occluders or not (crossed_sheets(occluders) or is_plant(name)):
                continue  # no blocker in the game (nothing to replace), or not a plant's
            puffs = puffs_of(shadow_triangles(pk, modl))
        except (IndexError, KeyError, StopIteration, ValueError, struct.error, ZeroDivisionError):
            continue
        if puffs and lite[2] not in table:
            table[lite[2]] = (name or f"unnamed {key[2]:016X}", puffs)
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
