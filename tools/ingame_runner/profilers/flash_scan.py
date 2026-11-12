"""Finds flashes in a recording made by screen-luma.js (run.ps1 -Grab): a screen cell that gets much brighter (or darker) for a moment and then
comes back, which a camera move alone does not do (a pan changes the picture gradually and does not return).

Usage: python flash_scan.py <grab dir> [--rise 22] [--back 0.35] [--max-frames 6]
Writes flashes.csv (clock, frame, cells, the largest jump, frames until it came back) and, for each flash, the frame
before, at and after it as flash-<n>-<frame>-{a,b,c}.jpg next to it; prints the strongest ones.
settle.csv: per camera jump (a cut), the light changes while the camera then holds still and when they stopped.
Frames near a camera cut (most cells change at once and stay changed) are left out.
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
    black = sum(1 for row in luma if max(row) < 1.0)
    if n and black * 2 > n:
        print("%d of %d frames are black: the picture was not read" % (black, n))
    # a camera cut (a jump, not a pan): most cells change at once and stay changed; flashes next to one are its own
    cuts, jumps = set(), []
    for i in range(4, n - 8):
        moved = 0
        for c in range(cells):
            base = statistics.median(luma[j][c] for j in range(i - 4, i))
            if abs(luma[i][c] - base) >= opt["--rise"] and abs(luma[i + 8][c] - base) >= opt["--rise"] * 0.5:
                moved += 1
        if moved * 2 >= cells:
            if not jumps or i - jumps[-1] > 8:
                jumps.append(i)
            cuts.update(range(i - 8, i + 9))
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
        if hit and i not in cuts:
            events.append((i, hit))
    out = os.path.join(d, "flashes.csv")
    with open(out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["clock", "frame", "cells", "brighter", "largest", "framesBack", "cellList"])
        for i, hit in events:
            big = max(hit, key=lambda h: abs(h[1]))
            w.writerow([clock[i], frame[i], len(hit), sum(1 for h in hit if h[1] > 0), round(big[1], 1),
                        max(h[2] for h in hit), " ".join(str(h[0]) for h in hit)])
    # After a camera jump the camera holds still (camera_jumps dwellS): what still changes on screen then is the lot's light
    # arriving or changing (and Sims walking). Per jump: frames with 3 or more cells changing by 10 or more since the frame
    # before ("pops"), and the time from the jump to the last pop (settled).
    qpc = [float(r[head.index("qpcMs")]) for r in rows]
    # camera_jumps logs its jumps (jumps.txt next to the grab folder, "HH:MM:SS.mmm lot"): exact, where a dark night
    # jump may not look like a cut
    log = os.path.join(os.path.dirname(os.path.abspath(d)), "jumps.txt")
    if os.path.exists(log) and "clock" in head:
        def sec(t):
            h, m, x = t.split(":")
            return int(h) * 3600 + int(m) * 60 + float(x)
        fs = [sec(c) for c in clock]
        jumps = []
        for line in open(log, encoding="utf-8-sig"):
            if line.strip():
                t = sec(line.split()[0])
                i = min(range(n), key=lambda k: abs(fs[k] - t))
                if abs(fs[i] - t) < 1 and (not jumps or i > jumps[-1]):
                    jumps.append(i)
    settle = []
    for k, j in enumerate(jumps):
        end = jumps[k + 1] - 3 if k + 1 < len(jumps) else n
        pops = [f for f in range(j + 3, end)
                if sum(1 for c in range(cells) if abs(luma[f][c] - luma[f - 1][c]) >= 10) >= 3]
        settle.append((clock[j], frame[j], len(pops), round(qpc[pops[-1]] - qpc[j]) if pops else 0, round(qpc[end - 1] - qpc[j])))
    with open(os.path.join(d, "settle.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["clock", "frame", "pops", "settledMs", "stillMs"])
        w.writerows(settle)
    if settle:
        ms = sorted(x[3] for x in settle)
        print("%d camera jumps: %d pops after them, settled in median %d ms, worst %d ms -> settle.csv"
              % (len(settle), sum(x[2] for x in settle), ms[len(ms) // 2], ms[-1]))
    ranked = sorted(events, key=lambda e: -sum(abs(h[1]) for h in e[1]))
    for rank, (i, hit) in enumerate(ranked[:12]):
        for tag, j in (("a", i - 1), ("b", i), ("c", min(n - 1, i + 2))):
            for ext in ("bmp", "jpg"):  # screen-luma.js writes BMP
                src = os.path.join(d, "%06d.%s" % (frame[j], ext))
                if os.path.exists(src):
                    shutil.copy(src, os.path.join(d, "flash-%02d-%06d-%s.%s" % (rank, frame[i], tag, ext)))
        big = max(hit, key=lambda h: abs(h[1]))
        print("%s frame %d: %d cells, largest %+.0f, back in %d frames" % (clock[i], frame[i], len(hit), big[1], max(h[2] for h in hit)))
    print("%d frames, %d flash frames (%d frames near camera cuts left out) -> %s" % (n, len(events), len(cuts), out))


if __name__ == "__main__":
    main()
