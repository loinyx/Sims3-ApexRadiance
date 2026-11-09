"""Apex Radiance health report (docs/validation/dev-tools-scene-replay.md): reads ApexRadiance_LOG.txt and prints at most 25 lines on
the physical indoor lamps: edits taken or refused (with the top reasons), how fast an edit is shown, the GPU against the
game's solves, the frame cost and the map alternation that shows as flicker; then the change since the last report (the
summary JSON written beside the log as apex_health_last.json).

Usage: python tools/apex_health.py [log path] [--no-save] [--selftest]
"""
import json
import os
import re
import statistics
import sys
from collections import Counter

DEFAULT_LOG = os.path.join(os.path.expanduser("~"), "Documents", "Electronic Arts", "The Sims 3", "Apex Radiance", "ApexRadiance_LOG.txt")

R_SUMMARY = re.compile(r"physical lamp edits in the last 10 s: (\d+) taken, (\d+) left to the room solves")
R_REFUSED = re.compile(r"\[GpuRoomLamps\] physical [^:]+: lamp [0-9A-F]+ left to the room solves \((.*)\)$")
R_SHOWN = re.compile(r"\[GpuRoomLamps\] physical [^:]+: lamp [0-9A-F]+, .*relit in ([\d.]+) ms")
R_NOT_SHOWN = re.compile(r"its edit was not shown in every room within")
R_SELF = re.compile(r"\[GpuRoomLamps\] Story (-?\d+) room (\d+) class (\d+) (.+?) physical vs the game's last solve: (\d+) texels, ([\d.]+)% within 2/255")
R_RT_COST = re.compile(r"\[RtOutdoorLamps\] Cost over 10 s: ([\d.]+) ms a frame on average \(largest ([\d.]+) ms\).*the game wrote the map (\d+)")
R_DRAW_COST = re.compile(r"\[LotLightBridge\] Draw cost by fix over \d+ frames .*?: ([\d.]+) ms a frame in all")
R_HELD = re.compile(r"object parts kept their lights until their room showed the new light")
R_HEALTH = re.compile(r"\[Health\]")
R_SCENE = re.compile(r"\[SceneRecord\] Saved (\d+) room solves")
R_ORACLE = re.compile(r"\[GpuOracle\] (story -?\d+ room \d+ class \d+): within2 ([\d.]+)% of (\d+) lamp texels.*most misses (\w+)")
R_ROOM = re.compile(r"^story -?\d+ room \d+: ")


def pct(k, n):
    return 100.0 * k / n if n else 100.0


def quantile(v, q):
    if not v:
        return 0.0
    v = sorted(v)
    return v[min(len(v) - 1, int(q * (len(v) - 1) + 0.5))]


def analyse(lines):
    s = {"taken": 0, "refused": 0, "reasons": Counter(), "shown_ms": [], "not_shown": 0, "self": [], "rt_ms": [], "rt_peak": 0.0,
         "game_writes": 0, "draw_ms": [], "held": 0, "health": 0, "scenes": 0, "oracle": []}
    for line in lines:
        text = line.rstrip("\r\n")
        if m := R_SUMMARY.search(text):
            s["taken"] += int(m[1]); s["refused"] += int(m[2])
        elif m := R_REFUSED.search(text):
            s["reasons"][R_ROOM.sub("", m[1])] += 1
        elif m := R_SHOWN.search(text):
            s["shown_ms"].append(float(m[1]))
        elif R_NOT_SHOWN.search(text):
            s["not_shown"] += 1
        elif m := R_SELF.search(text):
            s["self"].append((f"story {m[1]} room {m[2]} class {m[3]} {m[4]}", int(m[5]), float(m[6])))
        elif m := R_RT_COST.search(text):
            s["rt_ms"].append(float(m[1])); s["rt_peak"] = max(s["rt_peak"], float(m[2])); s["game_writes"] += int(m[3])
        elif m := R_DRAW_COST.search(text):
            s["draw_ms"].append(float(m[1]))
        elif m := R_ORACLE.search(text):
            s["oracle"].append((m[1], int(m[3]), float(m[2]), m[4]))
        elif R_HELD.search(text):
            s["held"] += 1
        if R_HEALTH.search(text):
            s["health"] += 1
        if m := R_SCENE.search(text):
            s["scenes"] += 1
    return s


def summary(s):
    edits = s["taken"] + s["refused"]
    texels = sum(n for _, n, _ in s["self"])
    within = sum(n * p / 100.0 for _, n, p in s["self"])
    return {
        "refused_pct": round(pct(s["refused"], edits) if edits else 0.0, 2),
        "edits": edits,
        "shown_p50_ms": round(quantile(s["shown_ms"], 0.5), 1),
        "shown_p95_ms": round(quantile(s["shown_ms"], 0.95), 1),
        "gpu_vs_game_pct": round(pct(within, texels), 2) if texels else None,
        "rt_ms": round(statistics.mean(s["rt_ms"]), 3) if s["rt_ms"] else 0.0,
        "draw_ms": round(statistics.mean(s["draw_ms"]), 3) if s["draw_ms"] else 0.0,
        "game_writes": s["game_writes"],
        "held": s["held"],
    }


def report(s, last):
    out = []
    now = summary(s)
    ok = lambda b: "PASS" if b else "FAIL"
    edits = now["edits"]
    out.append(f"{ok(now['refused_pct'] <= 10)} edits: {s['taken']} taken on the GPU, {s['refused']} left to the game's solves ({now['refused_pct']:.1f}%)")
    for why, k in s["reasons"].most_common(3):
        out.append(f"      {k} x {why[:110]}")
    if s["shown_ms"]:
        out.append(f"{ok(now['shown_p95_ms'] <= 100 and not s['not_shown'])} edit shown: p50 {now['shown_p50_ms']} ms, p95 {now['shown_p95_ms']} ms, "
                   f"{s['not_shown']} not shown within 5 s")
    if s["self"]:
        out.append(f"{ok(now['gpu_vs_game_pct'] >= 95)} GPU vs the game's solves: {now['gpu_vs_game_pct']:.2f}% within 2/255 over {len(s['self'])} checks")
        for name, n, p in sorted(s["self"], key=lambda r: r[2])[:5]:
            if p < 99.0:
                out.append(f"      {p:.1f}% of {n}: {name}")
    else:
        out.append("---- GPU vs the game's solves: no check in this log (Developer: Compare indoor lamps with GPU)")
    if s["oracle"]:
        n = sum(r[1] for r in s["oracle"])
        w = sum(r[1] * r[2] / 100.0 for r in s["oracle"])
        out.append(f"{ok(pct(w, n) >= 95)} oracle (recording, no cube shadow): {pct(w, n):.2f}% within 2/255 of {n} lamp texels in {len(s['oracle'])} solves")
        for name, k, p, tag in sorted(s["oracle"], key=lambda r: r[2])[:3]:
            if p < 99.0:
                out.append(f"      {p:.1f}% of {k}: {name}, mostly {tag}")
    if s["rt_ms"] or s["draw_ms"]:
        out.append(f"{ok(now['rt_ms'] <= 1.0 and now['draw_ms'] <= 2.0)} frame cost: lamps {now['rt_ms']:.2f} ms a frame (peak {s['rt_peak']:.1f} ms), "
                   f"draw fixes {now['draw_ms']:.2f} ms a frame")
    out.append(f"{ok(now['game_writes'] <= 20 and now['held'] == 0)} flicker: {now['game_writes']} maps the game wrote over the GPU's, "
               f"{now['held']} object holds released late")
    if s["health"]:
        out.append(f"WARN {s['health']} health warnings in the log")
    if s["scenes"]:
        out.append(f"---- {s['scenes']} lighting scene recordings saved (run tools/scene_replay)")
    if last:
        diffs = []
        for k in ("refused_pct", "shown_p95_ms", "gpu_vs_game_pct", "rt_ms", "draw_ms", "game_writes", "held"):
            a, b = last.get(k), now.get(k)
            if a is not None and b is not None and a != b:
                diffs.append(f"{k} {a} -> {b}")
        out.append("since the last report: " + ("; ".join(diffs) if diffs else "no change"))
    return out[:25], now


def selftest():
    sample = [
        "00:00:01.000 [INFO] [GpuRoomLamps] physical lamp edits in the last 10 s: 8 taken, 2 left to the room solves; 0 room solves",
        "00:00:01.100 [INFO] [GpuRoomLamps] physical move: lamp 0000ABCD left to the room solves (story 0 room 13: no kept physical solve of the class it shows)",
        "00:00:01.200 [INFO] [GpuRoomLamps] physical move: lamp 0000ABCD left to the room solves (story 1 room 2: no kept physical solve of the class it shows)",
        "00:00:01.300 [INFO] [GpuRoomLamps] physical move: lamp 0000ABCD, no room solved, 47 maps relit in 9.7 ms (frames +1)",
        "00:00:01.400 [INFO] [GpuRoomLamps] Story 1 room 2 class 2 floor physical vs the game's last solve: 1000 texels, 98.00% within 2/255, mean 0.4 levels",
        "00:00:01.450 [INFO] [GpuOracle] story 1 room 2 class 2: within2 90.0% of 500 lamp texels, floor 90.0%; max err 0.100 (lamp 0000ABCD texel 3), most misses shadow3D",
        "00:00:01.500 [INFO] [RtOutdoorLamps] Cost over 10 s: 0.500 ms a frame on average (largest 3.00 ms) on the render thread; 1 relights, maps relit because: first 0, the game wrote the map 3, x",
    ]
    s = analyse(sample)
    lines, now = report(s, {"refused_pct": 50.0})
    assert s["taken"] == 8 and s["refused"] == 2, s
    assert s["reasons"]["no kept physical solve of the class it shows"] == 2, s["reasons"]
    assert now["refused_pct"] == 20.0 and now["gpu_vs_game_pct"] == 98.0 and now["game_writes"] == 3, now
    assert lines[0].startswith("FAIL edits") and any("refused_pct 50.0 -> 20.0" in l for l in lines), lines
    assert s["oracle"] == [("story 1 room 2 class 2", 500, 90.0, "shadow3D")] and any("mostly shadow3D" in l for l in lines), lines
    assert len(lines) <= 25
    print("apex_health self-test: ok")


def main(argv):
    if "--selftest" in argv:
        selftest()
        return 0
    paths = [a for a in argv if not a.startswith("--")]
    log = paths[0] if paths else DEFAULT_LOG
    try:
        with open(log, encoding="utf-8", errors="replace") as f:
            s = analyse(f)
    except OSError as e:
        print(f"no log: {e}")
        return 2
    state = os.path.join(os.path.dirname(os.path.abspath(log)), "apex_health_last.json")
    last = None
    try:
        with open(state, encoding="utf-8") as f:
            last = json.load(f)
    except (OSError, ValueError):
        pass
    lines, now = report(s, last)
    print("\n".join(lines))
    if "--no-save" not in argv:
        try:
            with open(state, "w", encoding="utf-8") as f:
                json.dump(now, f)
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
