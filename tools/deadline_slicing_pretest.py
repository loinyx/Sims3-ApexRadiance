"""Desk pre-test for Frame deadline while moving (docs/features/performance/deadline-slicing.md).

Reads an ApexRadiance_FrameTimeline.csv of a camera sweep and estimates the ceiling of the feature: on moving frames
>= 25 ms that also ran one item > 15 ms, the deferrable time (main-thread jobs, scene pending nodes, lot load stages)
that ran beside it. Gate from the research note: >= 2 ms on >= 20% of moving hitch frames.

    python tools/deadline_slicing_pretest.py ApexRadiance_FrameTimeline.csv
    python tools/deadline_slicing_pretest.py --self-check
"""
import csv
import sys

DEFERRABLE = ("Jobs (self)", "Scene pending nodes", "Lot load stages")
# shortcut: an "item" is one category column's self time in the frame, not one call; upgrade if the hitch blocks are joined
INDIVISIBLE = ("DXT encode", "RefPack compress", "Lot room solve", "Terrain update", "Services (self)", "Texture create",
               "Wall AO pass", "Lot impostor pump", "Resource lookup", "File read", "RefPack read")


def num(row, key):
    try:
        return float(row.get(key) or 0)
    except ValueError:
        return 0.0


def analyse(rows):
    moving_hitch = hit = 0
    total = 0.0
    for r in rows:
        if num(r, "apex_moving") < 1 or num(r, "frame_ms") < 25:
            continue
        moving_hitch += 1
        defer = sum(num(r, k) for k in DEFERRABLE)
        if max(num(r, k) for k in INDIVISIBLE) > 15 and defer > 2:
            hit += 1
            total += defer
    share = hit / moving_hitch if moving_hitch else 0.0
    return {"moving_hitch_frames": moving_hitch, "frames_with_stacking": hit, "share": share,
            "deferrable_ms_sum": total, "mean_ms": total / hit if hit else 0.0, "go": share >= 0.2}


def self_check():
    rows = [
        {"apex_moving": "1", "frame_ms": "40", "DXT encode": "20", "Jobs (self)": "3", "Scene pending nodes": "1"},
        {"apex_moving": "1", "frame_ms": "30", "DXT encode": "5", "Jobs (self)": "3"},   # no long item
        {"apex_moving": "0", "frame_ms": "60", "DXT encode": "40", "Jobs (self)": "5"},  # camera still
        {"apex_moving": "1", "frame_ms": "20", "DXT encode": "16", "Jobs (self)": "3"},  # not a hitch
        {"apex_moving": "1", "frame_ms": "50", "Lot room solve": "30", "Lot load stages": "1"},  # deferrable <= 2
    ]
    r = analyse(rows)
    assert r["moving_hitch_frames"] == 3 and r["frames_with_stacking"] == 1, r
    assert abs(r["deferrable_ms_sum"] - 4.0) < 1e-9 and r["go"], r
    print("self-check ok")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    if sys.argv[1] == "--self-check":
        self_check()
    else:
        with open(sys.argv[1], newline="", encoding="utf-8") as f:
            res = analyse(csv.DictReader(f))
        print(", ".join(f"{k} {v:.3f}" if isinstance(v, float) else f"{k} {v}" for k, v in res.items()))
