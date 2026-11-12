"""Light gaps of the real-time outdoor lamps from ApexRadiance_LOG.txt (developer mode), the times the game's own maps are
shown on the active lot instead of the relit ones:

- switch: after an active lot switch ("Active lot tracker X") until every story of the new lot is relit ("Active: ..."). The
  lot just left lost its relit maps at the switch (DropAll), so each switch is a light change on both lots. A switch that
  another switch follows first never got its relit maps ("never").
- refresh: the same lot falls back to "Inactive: waiting for kept room-0 solves" without a switch (the game made its maps
  anew, as when its detail rises once the camera stops) until "Active: ..." again: the whole lot shows the game's maps.

Usage: python lot_switch_gap.py <ApexRadiance_LOG.txt> [...]
"""
import re
import sys


def sec(t):
    h, m, x = t.split(":")
    return int(h) * 3600 + int(m) * 60 + float(x)


def stats(xs):
    xs = sorted(xs)
    return "n %d, median %.2f s, worst %.2f s, total %.1f s" % (len(xs), xs[len(xs) // 2], xs[-1], sum(xs)) if xs else "n 0"


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for path in sys.argv[1:]:
        switch_at = refresh_at = None
        active = False
        switches, refreshes, never = [], [], 0
        for line in open(path, encoding="utf-8", errors="replace"):
            m = re.match(r"(\d\d:\d\d:\d\d\.\d+) ", line)
            if not m or "[RtOutdoorLamps]" not in line:
                continue
            t = sec(m.group(1))
            if re.search(r"\[RtOutdoorLamps\] Active lot tracker [0-9A-F]+( \(|$)", line.rstrip()):
                if switch_at is not None:
                    never += 1
                switch_at, refresh_at, active = t, None, False
            elif "[RtOutdoorLamps] Active: outdoor lamps" in line:
                if switch_at is not None:
                    switches.append(t - switch_at)
                elif refresh_at is not None:
                    refreshes.append(t - refresh_at)
                switch_at = refresh_at = None
                active = True
            elif "[RtOutdoorLamps] Inactive: waiting for kept room-0 solves" in line and active and switch_at is None:
                refresh_at, active = t, False
        print("%s\n  switch:  %s, never relit %d\n  refresh: %s" % (path, stats(switches), never, stats(refreshes)))


if __name__ == "__main__":
    main()
