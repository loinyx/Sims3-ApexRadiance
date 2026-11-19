#!/usr/bin/env python3
"""Furniture caster meshes out of the game's packages for the mesh caster check (mesh_check.cpp, -Mesh in run.ps1).

Reads only; writes one file, <out> (the game's mesh data: keep it out of the repository, run.ps1 puts it in its -OutDir).
  python casters.py <game folder> <out>     a chair and a table, each as its LOD 0 mesh and its shadow LOD mesh
  python casters.py <game folder> --survey  the triangle counts of every model's LOD 0 and shadow LOD (the cache's budget)
  python casters.py --self-check            the LOD 0 reader on a synthetic MLOD
LOD 0 positions are four int16 each, divided by the fourth: checked against each mesh's own bounds (no other source).
The shadow LOD is read by tools/object_legs_table (its material's scale and offset). A mesh flagged as a drop shadow
(the floor decal, MLOD flag 0x08) is left out. Output, little endian: "MCST", count; per model: instance (u64), role
(8 bytes), then for LOD 0 and the shadow LOD: vertices (u32), triangles (u32), float x y z each, u32 a b c each.
"""
import importlib.util
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("object_legs_generate", os.path.join(HERE, "..", "object_legs_table", "generate.py"))
_legs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_legs)
rcol, ref, chunk, mlod_meshes, model_lods, indices, shadow_mesh, sections = (
    _legs.rcol, _legs.ref, _legs.chunk, _legs.mlod_meshes, _legs.model_lods, _legs.indices, _legs.shadow_mesh, _legs.sections)
MODL = _legs.MODL
DROP_SHADOW = 0x08 << 8  # MLOD mesh flags sit above the primitive type byte


def vrtf_position(b):
    """(stride, offset) of a VRTF chunk's position element; None unless it is the four int16 kind."""
    if b[:4] != b"VRTF":
        return None
    _ver, stride, n, ext = struct.unpack_from("<4I", b, 4)
    o = 20
    for _ in range(n):
        use, _ui, fmt, off = struct.unpack_from("<4I", b, o) if ext else tuple(b[o:o + 4])
        o += 16 if ext else 4
        if use == 0:
            return (stride, off) if fmt == 7 else None
    return None


def mlod_flags(b):
    """Each mesh's primitive and flags word and its bounds (MLOD: count, then per mesh its size and body)."""
    out, o = [], 12
    for _ in range(struct.unpack_from("<I", b, 8)[0]):
        size = struct.unpack_from("<I", b, o)[0]
        out.append((struct.unpack_from("<I", b, o + 24)[0], struct.unpack_from("<6f", b, o + 4 + 48)))
        o += 4 + size
    return out


def lod0_mesh(chunks, pub, mlod):
    """(positions, triangles, worst distance of the decoded bounds from the stored ones) of an MLOD chunk's meshes."""
    pos, tris, worst = [], [], 0.0
    for m, (flags, bounds) in zip(mlod_meshes(mlod), mlod_flags(mlod)):
        if flags & 0xFF != 3 or flags & DROP_SHADOW or not m["vrtf"] or not m["vbuf"] or not m["ibuf"]:
            continue  # (3: a triangle list)
        layout = vrtf_position(chunk(chunks, pub, m["vrtf"]))
        if layout is None:
            continue
        stride, off = layout
        vb, base = chunk(chunks, pub, m["vbuf"]), len(pos)
        lo, hi = [1e30] * 3, [-1e30] * 3
        for i in range(m["vc"]):
            x, y, z, w = struct.unpack_from("<4h", vb, 16 + m["soff"] + (m["sv"] + i) * stride + off)
            p = (x / (w or 32767), y / (w or 32767), z / (w or 32767))
            pos.append(p)
            lo, hi = [min(a, c) for a, c in zip(lo, p)], [max(a, c) for a, c in zip(hi, p)]
        worst = max([worst] + [abs(a - c) for a, c in zip(lo + hi, bounds)])
        idx = indices(chunk(chunks, pub, m["ibuf"]))[m["si"]:m["si"] + m["pc"] * 3]
        tris += [(base + idx[i], base + idx[i + 1], base + idx[i + 2]) for i in range(0, len(idx) - 2, 3) if max(idx[i:i + 3]) < m["vc"]]
    return pos, tris, worst


def lod(pk, chunks, pub, ext, r):
    """(chunks, public count, MLOD chunk) a MODL's LOD reference names, inside the MODL or in its own resource; None if missing."""
    if r[0] < 2:
        return chunks, pub, chunk(chunks, pub, r)
    if ext[r[1]] not in pk.index:
        return None
    chunks, pub, _ = rcol(pk.get(ext[r[1]]))
    return chunks, pub, next(c[3] for c in chunks if c[3][:4] == b"MLOD")


def model(pk, key):
    """(LOD 0 positions, triangles, bounds error) or None, (shadow positions, triangles) or None, of a MODL."""
    chunks, pub, ext = rcol(pk.get(key))
    lods = {i: ref(r) for r, i in model_lods(next(c[3] for c in chunks if c[3][:4] == b"MODL"))}
    at = lod(pk, chunks, pub, ext, lods[0]) if 0 in lods else None
    lod0 = lod0_mesh(*at) if at else None
    want = next((lods[i] for i in _legs.SHADOW_LODS if i in lods), None)
    at = lod(pk, chunks, pub, ext, want) if want else None
    if not at:
        return lod0, None
    p, t = [], []
    for m in mlod_meshes(at[2]):
        if m["vrtf"] is None and m["so"]:
            mp, mt = shadow_mesh(at[0], at[1], m)
            t += [(a + len(p), b + len(p), c + len(p)) for a, b, c in mt]
            p += mp
    return lod0, (p, t) if t else None


def extent(p):
    return [min(v[k] for v in p) for k in range(3)], [max(v[k] for v in p) for k in range(3)]


def role(p, t):
    """'chair', 'table' or None by shape: size, standing on the floor, legs at a low cut."""
    lo, hi = extent(p)
    w, h, d = (hi[k] - lo[k] for k in range(3))
    if lo[1] < -0.02 or lo[1] > 0.02 or not 200 <= len(t) <= 6000:
        return None
    if 0.38 <= w <= 0.7 and 0.38 <= d <= 0.7 and 0.8 <= h <= 1.15 and len(sections(p, t, 0.2)) >= 4:
        return "chair"
    if 1.0 <= max(w, d) <= 2.2 and 0.6 <= min(w, d) <= 1.2 and 0.7 <= h <= 0.82 and len(sections(p, t, 0.3)) >= 4:
        return "table"
    return None


def write(out, picked):
    with open(out, "wb") as f:
        f.write(b"MCST" + struct.pack("<I", len(picked)))
        for inst, name, meshes in picked:
            f.write(struct.pack("<Q", inst) + name.encode().ljust(8, b"\0"))
            for p, t in meshes:
                f.write(struct.pack("<II", len(p), len(t)))
                f.write(struct.pack(f"<{3 * len(p)}f", *[c for v in p for c in v]))
                f.write(struct.pack(f"<{3 * len(t)}I", *[c for v in t for c in v]))


def self_check():
    # one mesh: VRTF (position int16 x4 at 0, stride 8), VBUF of 3 vertices, differenced IBUF; chunks 0 MLOD 1 VRTF 2 VBUF 3 IBUF
    vrtf = b"VRTF" + struct.pack("<4I", 2, 8, 1, 0) + bytes([0, 0, 7, 0])
    verts = [(0, 0, 0, 16383), (16383, 0, 0, 16383), (0, 32767, 0, 32767)]
    vbuf = b"VBUF" + struct.pack("<3I", 1, 0, 0) + b"".join(struct.pack("<4h", *v) for v in verts)
    ibuf = b"IBUF" + struct.pack("<3I", 1, 1, 0) + struct.pack("<3h", 0, 1, 1)  # differenced: 0, 1, 2
    body = struct.pack("<I", 0) + struct.pack("<4I", 0, 0x10000002, 0x10000003, 0x10000004) + struct.pack("<7I", 0x4003, 0, 0, 0, 0, 3, 1)
    body += struct.pack("<6f", 0, 0, 0, 1, 1, 0) + struct.pack("<II", 0, 0) + struct.pack("<I", 0) + bytes(20)
    mlod = b"MLOD" + struct.pack("<II", 0x203, 1) + struct.pack("<I", len(body)) + body
    chunks = [(0, 0, 0, c) for c in (mlod, vrtf, vbuf, ibuf)]
    p, t, worst = lod0_mesh(chunks, 0, mlod)
    assert p == [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)] and t == [(0, 1, 2)] and worst == 0.0, (p, t, worst)
    flagged = mlod.replace(struct.pack("<I", 0x4003), struct.pack("<I", 0x4803))
    assert lod0_mesh([(0, 0, 0, flagged)] + chunks[1:], 0, flagged)[1] == [], "a drop shadow mesh is left out"
    print("self-check passed")


def main():
    if sys.argv[1:] == ["--self-check"]:
        return self_check()
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    pk = _legs.Packages(sys.argv[1])
    survey = sys.argv[2] == "--survey"
    picked, counts, worst, unread = {}, [], 0.0, 0
    for key in sorted(k for k in pk.index if k[0] == MODL):
        try:
            lod0, shadow = model(pk, key)
        except (IndexError, KeyError, StopIteration, ValueError, struct.error, ZeroDivisionError):
            unread += 1
            continue
        if not lod0 or not lod0[1]:
            continue
        worst = max(worst, lod0[2])
        counts.append((len(lod0[1]), len(shadow[1]) if shadow else 0))
        if survey or not shadow:
            continue
        r = role(lod0[0], lod0[1])
        if r and r not in picked:
            picked[r] = (key[2], r, [lod0[:2], shadow])
            if len(picked) == 2:
                break
    print(f"{len(counts)} models read ({unread} unreadable); LOD 0 bounds within {worst * 1e3:.2f} mm of the stored ones")
    if survey:
        lod0 = sorted(c[0] for c in counts)
        shad = sorted(c[1] for c in counts if c[1])
        q = lambda v, f: v[min(len(v) - 1, int(f * len(v)))]  # noqa: E731
        print(f"LOD 0 triangles: median {q(lod0, 0.5)}, 90 % {q(lod0, 0.9)}, 99 % {q(lod0, 0.99)}")
        print(f"shadow LOD on {len(shad)} of {len(counts)}: median {q(shad, 0.5)}, 90 % {q(shad, 0.9)}, 99 % {q(shad, 0.99)} triangles;"
              f" shadow / LOD 0 median {sorted(c[1] / c[0] for c in counts if c[1])[len(shad) // 2]:.2f}")
        return
    for inst, r, m in picked.values():
        print(f"{r}: MODL {inst:016X}, LOD 0 {len(m[0][1])} triangles, shadow LOD {len(m[1][1])} triangles")
    if len(picked) < 2:
        sys.exit("no chair and table found")
    write(sys.argv[2], list(picked.values()))
    for f in pk.files.values():
        f.close()


if __name__ == "__main__":
    main()
