"""Story seams from a recording's Wall seams.csv: a point on the line between two stories is listed twice, as the top of
the story below and the bottom of the story above (same x, y, z and normal). The difference of their light
(after_curve_sum) is the seam a wall shows at the floor line. The two stories are sampled in different frames: each
point of one story is paired with the same point of the other story's nearest sample within 250 ms.

Usage: python seam_report.py <recording folder or Wall seams.csv> [...]
Prints: point pairs compared, median and 90th percentile of |below - above| / max(below, above), the share of pairs over
25 %, and the worst second of the recording with its own median.
"""
import csv
import os
import sys
from collections import defaultdict


def report(path):
    if os.path.isdir(path):
        path = os.path.join(path, "Wall seams.csv")
    samples = defaultdict(list)  # (lot, x, y, z, normal) -> [(elapsed, story, light)]
    for r in csv.DictReader(open(path, encoding="utf-8")):
        key = (r["lot"], round(float(r["x"]), 2), round(float(r["y"]), 2), round(float(r["z"]), 2),
               round(float(r["nx"]), 2), round(float(r["nz"]), 2))
        samples[key].append((int(r["elapsed_ms"]), int(r["story"]), float(r["after_curve_sum"])))
    rel, by_time = [], defaultdict(list)
    for pts in samples.values():
        stories = sorted(set(s for _, s, _ in pts))
        for a, b in zip(stories, stories[1:]):
            lower = [p for p in pts if p[1] == a]
            upper = [p for p in pts if p[1] == b]
            for t, _, lo in lower:
                near = min(upper, key=lambda p: abs(p[0] - t))
                if abs(near[0] - t) > 250:
                    continue
                top = max(lo, near[2])
                if top > 0.02:  # (a seam between two dark texels is invisible)
                    x = abs(lo - near[2]) / top
                    rel.append(x)
                    by_time[t // 1000].append(x)
    worst = max(((sorted(v)[len(v) // 2], k) for k, v in by_time.items()), default=(0.0, None))
    if not rel:
        print("%s: no lit point on a story line" % path)
        return
    rel.sort()
    n = len(rel)
    print("%s: %d point pairs on story lines: median %.0f %%, p90 %.0f %%, over 25 %%: %.0f %% of points; worst second %s (median %.0f %%)"
          % (os.path.basename(os.path.dirname(path)) or path, n, 100 * rel[n // 2], 100 * rel[int(n * 0.9)],
             100.0 * sum(1 for x in rel if x > 0.25) / n, worst[1], 100 * worst[0]))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for p in sys.argv[1:]:
        report(p)
