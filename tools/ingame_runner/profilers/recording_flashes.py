"""Flashes in the mod's own recordings (Captures\\<date> <HH-MM-SS> Recording\\Screen pixels.csv): the screen points it reads
every frame, the column around the mouse and the 6 x 4 grid (the ground points that move with a lamp are left out), turned
into flash_scan.py's luma.csv with the wall clock (the folder's start time plus elapsed_ms), so the flashes line up with
ApexRadiance_LOG.txt.

Usage: python recording_flashes.py <recording folder> [...]   (writes <folder>\\scan\\luma.csv, flashes.csv, settle.csv)
"""
import csv
import datetime
import os
import re
import subprocess
import sys


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    here = os.path.dirname(os.path.abspath(__file__))
    for folder in sys.argv[1:]:
        m = re.search(r"(\d\d)-(\d\d)-(\d\d) Recording", folder)
        if not m:
            print("%s: not a recording folder name" % folder)
            continue
        start = datetime.datetime(2000, 1, 1, int(m.group(1)), int(m.group(2)), int(m.group(3)))
        rows = list(csv.reader(open(os.path.join(folder, "Screen pixels.csv"), encoding="utf-8")))
        head = rows[0]
        grid = [i for i, c in enumerate(head) if c.startswith("luma") and (c.startswith("luma_x") or c[4:5] in "+-")]
        out = os.path.join(folder, "scan")
        os.makedirs(out, exist_ok=True)
        with open(os.path.join(out, "luma.csv"), "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["qpcMs", "clock", "frame"] + ["c%d" % k for k in range(len(grid))])
            for n, r in enumerate(rows[1:]):
                if any(r[i] == "" for i in grid):
                    continue
                t = start + datetime.timedelta(milliseconds=float(r[0]))
                w.writerow([r[0], t.strftime("%H:%M:%S.%f")[:-3], n] + [r[i] for i in grid])
        res = subprocess.run([sys.executable, "-I", os.path.join(here, "flash_scan.py"), out], capture_output=True, text=True)
        print("== %s\n%s" % (os.path.basename(folder.rstrip("\\/")), res.stdout.strip()))


if __name__ == "__main__":
    main()
