"""Finds flashes in a recording made by grab.ps1: a screen cell that gets much brighter (or darker) for a moment and then
comes back, which a camera move alone does not do (a pan changes the picture gradually and does not return).

Usage: python flash_scan.py <grab dir> [--rise 22] [--back 0.35] [--max-frames 6]
Writes flashes.csv (clock, frame, cells, the largest jump, frames until it came back) and, for each flash, the frame
before, at and after it as flash-<n>-<frame>-{a,b,c}.jpg next to it; prints the strongest ones.
A cell flashes at frame i when its luma rises by at least --rise over the median of the 4 frames before, and within
--max-frames it is back within --back of that rise. Darkening blinks are found the same way with the sign reversed.
"""
import csv
import os
import shutil
import statistics
import sys


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    d = args[0]
    opt = {"--rise": 22.0, "--back": 0.35, "--max-frames": 6}
    for k in opt:
        if k in args:
            opt[k] = type(opt[k])(args[args.index(k) + 1])
    rows = list(csv.reader(open(os.path.join(d, "luma.csv"), encoding="utf-8")))
    head, rows = rows[0], rows[1:]
    ci = head.index("c0")
    clock = [r[head.index("clock")] if "clock" in head else r[0] for r in rows]
    frame = [int(r[head.index("frame")]) for r in rows]
    luma = [[float(x) for x in r[ci:]] for r in rows]
    n, cells = len(luma), len(luma[0]) if luma else 0
    events = []
    busy = [0] * cells  # a cell's flash lasts until it is back: its later frames are the same flash
    for i in range(4, n - 1):
        hit = []
        for c in range(cells):
            if i < busy[c]:
                continue
            base = statistics.median(luma[j][c] for j in range(i - 4, i))
            for sign in (1, -1):
                rise = sign * (luma[i][c] - base)
                if rise < opt["--rise"]:
                    continue
                for k in range(i + 1, min(n, i + 1 + opt["--max-frames"])):
                    if sign * (luma[k][c] - base) <= rise * opt["--back"]:
                        hit.append((c, sign * rise, k - i))
                        busy[c] = k
                        break
        if hit:
            events.append((i, hit))
    out = os.path.join(d, "flashes.csv")
    with open(out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["clock", "frame", "cells", "brighter", "largest", "framesBack", "cellList"])
        for i, hit in events:
            big = max(hit, key=lambda h: abs(h[1]))
            w.writerow([clock[i], frame[i], len(hit), sum(1 for h in hit if h[1] > 0), round(big[1], 1),
                        max(h[2] for h in hit), " ".join(str(h[0]) for h in hit)])
    ranked = sorted(events, key=lambda e: -sum(abs(h[1]) for h in e[1]))
    for rank, (i, hit) in enumerate(ranked[:12]):
        for tag, j in (("a", i - 1), ("b", i), ("c", min(n - 1, i + 2))):
            src = os.path.join(d, "%06d.jpg" % frame[j])
            if os.path.exists(src):
                shutil.copy(src, os.path.join(d, "flash-%02d-%06d-%s.jpg" % (rank, frame[i], tag)))
        big = max(hit, key=lambda h: abs(h[1]))
        print("%s frame %d: %d cells, largest %+.0f, back in %d frames" % (clock[i], frame[i], len(hit), big[1], max(h[2] for h in hit)))
    print("%d frames, %d flash frames -> %s" % (n, len(events), out))


if __name__ == "__main__":
    main()
