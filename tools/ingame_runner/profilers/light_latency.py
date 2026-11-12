"""Light latency from ApexRadiance_LOG.txt: how long each part of a lighting check took to show its final light.

Usage: python light_latency.py <ApexRadiance_LOG.txt> [...]   (several logs: one column each, e.g. before and after)

Prints, per log:
  lot switches   each "Active lot tracker X" to the next "Active: ... every story" (the lot fully on the GPU's light)
  lamp edits     "Lamp edit: its rooms settled after N ms" (median, p90, max, count) and the game's own solve time
                 a frame while they settled (the frame drops of an edit)
  drags          "AtriumHold ... (drag, from its release) ... shown N ms after" (the light of a dragged lamp after release)
  frames         frames per 10 s from the "Draw cost by fix over N frames" lines (min and median FPS)
"""
import re
import statistics
import sys

TS = re.compile(r"^(\d\d):(\d\d):(\d\d)\.(\d\d\d) ")


def sec(line):
    m = TS.match(line)
    return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 1000 if m else None


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))] if v else 0


def analyse(path):
    switches, edits, solve_ms, drags, fps = [], [], [], [], []
    pending = None
    for line in open(path, encoding="utf-8", errors="replace"):
        t = sec(line)
        if t is None:
            continue
        m = re.search(r"\[RtOutdoorLamps\] Active lot tracker ([0-9A-F]+)$", line.strip())
        if m:
            pending = (t, m.group(1))
            continue
        if pending and "[RtOutdoorLamps] Active: outdoor lamps of the active lot are drawn on the GPU" in line:
            switches.append((pending[1], t - pending[0]))
            pending = None
            continue
        m = re.search(r"\[RoomLightQueue\] Lamp edit: its rooms settled after (\d+) ms .*?the game solved rooms for (\d+) ms \(([\d.]+) ms a frame\)", line)
        if m:
            edits.append(int(m.group(1)))
            solve_ms.append(float(m.group(3)))
            continue
        m = re.search(r"\[AtriumHold\] Lamp edit \(drag, from its release\).*shown (\d+) ms after", line)
        if m:
            drags.append(int(m.group(1)))
            continue
        m = re.search(r"\[LotLightBridge\] Draw cost by fix over (\d+) frames", line)
        if m:
            fps.append(int(m.group(1)) / 10.0)
    return switches, edits, solve_ms, drags, fps


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for path in sys.argv[1:]:
        sw, ed, so, dr, fps = analyse(path)
        print("== %s" % path)
        print("  lot switches to the GPU's light on every story: %s" % (", ".join("%s %.1f s" % (k[-4:], v) for k, v in sw) or "none"))
        if ed:
            print("  lamp edits: %d, settled median %d ms, p90 %d ms, max %d ms; the game's solves while settling: median %.1f ms a frame, max %.1f"
                  % (len(ed), statistics.median(ed), pct(ed, 0.9), max(ed), statistics.median(so), max(so)))
        if dr:
            print("  drags: %d, light shown after release median %d ms, max %d ms" % (len(dr), statistics.median(dr), max(dr)))
        if fps:
            print("  frames: median %.0f FPS, lowest 10 s %.0f FPS" % (statistics.median(fps), min(fps)))


if __name__ == "__main__":
    main()
