#!/usr/bin/env python3
"""Bakes the cutout masks of the game's packages into features/opening_masks_table.inc.

A wall opening's hole lives in a DDS mask (type 0x00B2D882): one mask per wall cell, its rows spanning the wall's 3 m, cut
where min(G, A) < 128 (some masks hold the hole in RGB, others in alpha). The game's packages are only read. A Delta package
overrides a Full one. Standard library only.

    python tools/opening_mask_table/generate.py "<game folder>" [--check]

--check prints the counts and writes nothing.
"""
import argparse
import glob
import os
import struct
import sys

MASK_TYPE = 0x00B2D882
CUT_BELOW = 128
PROFILE = 5           # WallOpenings::kProfile
SILL_SNAP_ROWS = 3    # of 128: a hole this close to the wall's base stands on it (DXT blocks round the bottom edge)
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "features", "opening_masks_table.inc")


def package_index(path):
    """(type, group, instance, offset, size, memsize, compressed) of each resource of a DBPF 2.0 package."""
    with open(path, "rb") as f:
        head = f.read(96)
        if head[:4] != b"DBPF":
            return []
        count, size = struct.unpack_from("<I", head, 36)[0], struct.unpack_from("<I", head, 44)[0]
        pos = struct.unpack_from("<I", head, 64)[0] or struct.unpack_from("<I", head, 40)[0]
        f.seek(pos)
        data = f.read(size)
    flags, at, fixed = struct.unpack_from("<I", data, 0)[0], 4, {}
    for i in range(8):
        if flags >> i & 1:
            fixed[i] = struct.unpack_from("<I", data, at)[0]
            at += 4
    rows = []
    for _ in range(count):
        v = [0] * 8
        for i in range(8):
            if i in fixed:
                v[i] = fixed[i]
            else:
                v[i] = struct.unpack_from("<I", data, at)[0]
                at += 4
        rows.append((v[0], v[1], v[2] << 32 | v[3], v[4], v[5] & 0x7FFFFFFF, v[6], v[7] & 0xFFFF == 0xFFFF))
    return rows


def refpack(d):
    p = 6 if d[0] & 0x80 else 5
    out = bytearray()
    while p < len(d):
        c = d[p]
        if c < 0x80:
            b = d[p + 1]
            n, m, o = c & 3, ((c & 0x1C) >> 2) + 3, ((c & 0x60) << 3) + b + 1
            p += 2
        elif c < 0xC0:
            b1, b2 = d[p + 1], d[p + 2]
            n, m, o = b1 >> 6, (c & 0x3F) + 4, ((b1 & 0x3F) << 8) + b2 + 1
            p += 3
        elif c < 0xE0:
            b1, b2, b3 = d[p + 1], d[p + 2], d[p + 3]
            n, m, o = c & 3, ((c & 0x0C) << 6) + b3 + 5, ((c & 0x10) << 12) + (b1 << 8) + b2 + 1
            p += 4
        elif c < 0xFC:
            n, m, o = ((c & 0x1F) << 2) + 4, 0, 0
            p += 1
        else:
            n = c & 3
            out += d[p + 1:p + 1 + n]
            break
        out += d[p:p + n]
        p += n
        for _ in range(m):
            out.append(out[-o])
    return bytes(out)


def mask_values(dds):
    """(w, h, rows of min(G, A)) of a DXT1/3/5 DDS's top mip, or None."""
    if dds[:4] != b"DDS ":
        return None
    h, w = struct.unpack_from("<II", dds, 12)
    four = dds[84:88]
    if four not in (b"DXT1", b"DXT3", b"DXT5") or w % 4 or h % 4 or len(dds) < 128 + (w // 4) * (h // 4) * (8 if four == b"DXT1" else 16):
        return None
    img = [bytearray(w) for _ in range(h)]
    p = 128
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            alpha = None
            if four == b"DXT5":
                a0, a1 = dds[p], dds[p + 1]
                bits = int.from_bytes(dds[p + 2:p + 8], "little")
                t = [a0, a1] + ([((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)] if a0 > a1 else [((4 - i) * a0 + (i + 1) * a1) // 5 for i in range(4)] + [0, 255])
                alpha = [t[bits >> (3 * i) & 7] for i in range(16)]
                p += 8
            elif four == b"DXT3":
                bits = int.from_bytes(dds[p:p + 8], "little")
                alpha = [(bits >> (4 * i) & 15) * 17 for i in range(16)]
                p += 8
            c0, c1, bits = struct.unpack_from("<HHI", dds, p)
            p += 8
            g0, g1 = (c0 >> 5 & 63) * 255 // 63, (c1 >> 5 & 63) * 255 // 63
            if c0 > c1 or four != b"DXT1":
                t = [(g0, 255), (g1, 255), ((2 * g0 + g1) // 3, 255), ((g0 + 2 * g1) // 3, 255)]
            else:
                t = [(g0, 255), (g1, 255), ((g0 + g1) // 2, 255), (0, 0)]
            for i in range(16):
                g, a = t[bits >> (2 * i) & 3]
                if alpha:
                    a = alpha[i]
                img[by + i // 4][bx + i % 4] = min(g, a)
    return w, h, img


def hole_shape(w, h, img):
    """(u0, u1, sill, head x PROFILE) in 1/255 of the cell's width and of 3 m, or None when this is no cutout mask."""
    tops, bottoms = [-1] * w, [-1] * w
    cut = extreme = 0
    for y, row in enumerate(img):
        for x, v in enumerate(row):
            extreme += v < 32 or v > 223
            if v < CUT_BELOW:
                cut += 1
                if tops[x] < 0:
                    tops[x] = y
                bottoms[x] = y
    # a mask is a two-level image: its pixels are hole or wall, not the shades of a painted texture
    if not cut or extreme < 0.9 * w * h or cut > 0.95 * w * h:
        return None
    cols = [x for x in range(w) if tops[x] >= 0]
    first, last = cols[0], cols[-1]
    low = max(bottoms[x] for x in cols) + 1
    sill = 0 if h - low <= SILL_SNAP_ROWS * h // 128 else h - low
    head = []
    for k in range(PROFILE):
        at = round(first + (last - first) * k / (PROFILE - 1))
        near = min(cols, key=lambda x: abs(x - at))
        head.append(h - tops[near])
    return (round(first * 255 / w), round((last + 1) * 255 / w), round(sill * 255 / h)) + tuple(round(v * 255 / h) for v in head)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    packs = [p for p in glob.glob(os.path.join(args.game, "**", "*.package"), recursive=True) if os.path.basename(p).startswith(("FullBuild", "DeltaBuild"))]
    packs.sort(key=lambda p: (os.path.basename(p).startswith("DeltaBuild"), p.lower()))  # (the last one read wins)
    found = {}
    for path in packs:
        for t, g, inst, off, size, mem, comp in package_index(path):
            if t == MASK_TYPE and mem <= 48 * 1024:  # (the masks are 64 x 128, a few larger)
                found[(inst, g)] = (path, off, size, comp)
    shapes, skipped = {}, 0
    files = {}
    for (inst, g), (path, off, size, comp) in sorted(found.items(), key=lambda kv: kv[1][0]):
        f = files.get(path) or files.setdefault(path, open(path, "rb"))
        f.seek(off)
        data = f.read(size)
        try:
            m = mask_values(refpack(data) if comp else data)
        except (IndexError, struct.error):
            m = None
        s = hole_shape(*m) if m and m[1] in (m[0], 2 * m[0]) else None  # (64 x 128, one logged at 64 x 64)
        if s is None:
            skipped += 1
        elif shapes.setdefault(inst, s) != s:
            print(f"instance {inst:016X}: two shapes across groups, keeping the first", file=sys.stderr)
    for f in files.values():
        f.close()
    unique = sorted(set(shapes.values()))
    print(f"{len(packs)} packages, {len(found)} small textures, {skipped} not masks, {len(shapes)} masks, {len(unique)} shapes")
    if args.check:
        return
    at = {s: i for i, s in enumerate(unique)}
    with open(OUT, "w", newline="\n") as f:
        f.write("// Generated by tools/opening_mask_table/generate.py. Do not edit.\n")
        f.write("inline constexpr MaskRow kMaskShapes[] = {\n")
        for s in unique:
            f.write("{" + ",".join(map(str, s)) + "},\n")
        f.write("};\ninline constexpr MaskEntry kMasks[] = {\n")
        for inst in sorted(shapes):
            f.write(f"{{0x{inst:016X}ull,{at[shapes[inst]]}}},\n")
        f.write("};\n")


if __name__ == "__main__":
    main()
