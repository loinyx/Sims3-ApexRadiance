"""Self-check of flash_scan.py: a one-frame flash and a short blink are found, a steady ramp (a camera pan) and a camera cut
are not."""
import csv
import os
import subprocess
import sys
import tempfile

d = tempfile.mkdtemp()
with open(os.path.join(d, "luma.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["qpcMs", "clock", "frame", "c0", "c1", "c2", "c3", "c4"])
    for i in range(60):
        flash = 90 if i == 10 else 30          # c0: one bright frame
        blink = 5 if i in (20, 21) else 40     # c1: two dark frames
        ramp = 20 + 3 * i                      # c2: gradual brightening, never back
        cut = 20 if i < 45 else 90                 # c3, c4: a camera cut at 45 (changes and stays)
        w.writerow([i * 125, "00:00:%02d.000" % i, i, flash, blink, ramp, cut, cut])
out = subprocess.run([sys.executable, "-I", os.path.join(os.path.dirname(__file__), "flash_scan.py"), d],
                     capture_output=True, text=True, check=True).stdout
rows = list(csv.DictReader(open(os.path.join(d, "flashes.csv"))))
frames = sorted(int(r["frame"]) for r in rows)
assert frames == [10, 20], (frames, out)
assert any(r["frame"] == "10" and float(r["largest"]) > 0 for r in rows)
assert any(r["frame"] == "20" and float(r["largest"]) < 0 for r in rows)
print("flash_scan self-check passed")
