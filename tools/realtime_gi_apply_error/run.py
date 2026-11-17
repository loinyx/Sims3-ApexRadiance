#!/usr/bin/env python3
"""Error of a multiplicative post-scene apply behind the game's tone curves (real-time GI, detail layers).

A detail layer runs after the scene and scales the finished pixel: colour *= 1 + s, with s = dE / E the known change of
the light at the pixel (s > 0 bounce detail, s < 0 contact, capsule and rig shadows). That is exact where the pixel is
linear in its light (walls, objects). Two curves of the game are not:

  floor  the room-lit floor shaders end with c = y tanh(K m) / m on y = light x albedo, m = mean(y), K = 4.2 ln 2 / 2
  solve  the game's room solve writes map = x tanh(m / 2) / m, m = mean(x) (only maps the game still solves)

Both scale the colour by a function of its mean, so the true pixel after the light change follows from the pixel
itself: c' = c (1 + s) tanh(atanh(u) q / u) / q, u = mean(c), q = mean(c (1 + s)). The constant K cancels.

    python tools/realtime_gi_apply_error/run.py                  the tables
    python tools/realtime_gi_apply_error/run.py --selftest
    python tools/realtime_gi_apply_error/run.py --frames <png/jpg or folder> [--stride 7]
    python tools/realtime_gi_apply_error/run.py --write          tables into results.md

--frames takes every sampled pixel as a floor pixel (the worst case; other pixels have no error) and needs Pillow.
"""
import argparse
import math
import sys
from pathlib import Path

K_FLOOR = 4.2 * math.log(2) / 2
U_MAX = 1 - 1 / 510  # the curve never reaches 1; an 8-bit 255 is taken as half a level below
TERMS = (-1.0, -0.5, -0.25, -0.1, -0.05, 0.05, 0.1, 0.25, 0.5, 1.0)


def sat(v):
    return min(max(v, 0.0), 1.0)


def truth(c, s):
    """The pixel c (after a mean curve) once its light is scaled by 1 + s a channel. Exact; also the corrected apply."""
    u = sum(c) / 3
    lit = [c[i] * (1 + s[i]) for i in range(3)]
    q = sum(lit) / 3
    if u < 1e-6 or q <= 0:
        return [sat(v) for v in lit]
    g = math.tanh(math.atanh(min(u, U_MAX)) * q / u) / q
    return [sat(v * g) for v in lit]


def plain(c, s):
    """The multiplicative apply as designed."""
    return [sat(c[i] * (1 + s[i])) for i in range(3)]


def err_levels(c, s):
    """Largest channel error of the plain apply, in 8-bit levels, signed (positive: too bright)."""
    t, p = truth(c, s), plain(c, s)
    return max(((p[i] - t[i]) * 255 for i in range(3)), key=abs)


def first_order(u):
    """Relative error per unit of s for a small term: 1 - atanh(u) (1 - u^2) / u."""
    return 1 - math.atanh(u) * (1 - u * u) / u


def floor_after_solve(u_out, albedo, s):
    """Grey floor pixel of mean u_out whose map went through the solve curve too; returns the true mean, or None when
    that albedo cannot give that pixel."""
    y = math.atanh(min(u_out, U_MAX)) / K_FLOOR  # light x albedo
    m = y / albedo  # the map's mean
    if m >= U_MAX:
        return None
    m2 = math.tanh((1 + s) * math.atanh(m))
    return math.tanh(K_FLOOR * albedo * m2)


def tables():
    out = []
    greys = (0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9)
    out.append('### 1. Floor pixels: error of `colour *= 1 + s` in 8-bit levels (grey pixel, grey term)\n')
    out.append('| pixel mean (0-255) | error per unit s, small s | ' + ' | '.join(f's = {s:+g}' for s in TERMS) + ' |')
    out.append('|---|---|' + '---|' * len(TERMS))
    for u in greys:
        row = ' | '.join(f'{err_levels([u] * 3, [s] * 3):+.1f}' for s in TERMS)
        out.append(f'| {u:.1f} ({round(u * 255)}) | {first_order(u) * 100:.1f}% | {row} |')
    out.append('')
    out.append('### 2. Largest floor error over all grey pixel values up to 229 (map 1 x albedo 1), and the pixel mean where it falls\n')
    out.append('| s | largest error (levels) | at pixel mean | brightest pixel mean with at most 1 / 2 / 4 levels |')
    out.append('|---|---|---|---|')
    # an 8-bit map holds at most 1 and an albedo is at most 1, so a floor pixel's mean is at most tanh(K): 229
    grid = [i / 510 for i in range(1, int(math.tanh(K_FLOOR) * 510) + 1)]
    for s in TERMS:
        errs = [(abs(err_levels([u] * 3, [s] * 3)), u) for u in grid]
        worst = max(errs)
        lim = []
        for cap in (1, 2, 4):
            ok = 0.0
            for e, u in errs:
                if e > cap:
                    break
                ok = u
            lim.append(f'{round(ok * 255)}')
        out.append(f'| {s:+g} | {worst[0]:.1f} | {round(worst[1] * 255)} | {" / ".join(lim)} |')
    out.append('')
    out.append('### 3. Coloured cases (floor pixel; error of the worst channel, levels)\n')
    out.append('| pixel (0-255) | term s (r, g, b) | plain apply | truth | error |')
    out.append('|---|---|---|---|---|')
    cases = [((0.6, 0.6, 0.6), (0.3, 0.0, 0.0)), ((0.6, 0.6, 0.6), (0.3, 0.15, 0.0)), ((0.8, 0.3, 0.2), (0.25,) * 3),
             ((0.8, 0.3, 0.2), (-0.5,) * 3), ((0.2, 0.3, 0.8), (0.5, 0.1, 0.0)), ((0.7, 0.7, 0.6), (-0.25,) * 3)]
    for c, s in cases:
        p, t = plain(c, s), truth(c, s)
        fmt = lambda v: ', '.join(str(round(x * 255)) for x in v)
        out.append(f'| {fmt(c)} | {", ".join(f"{x:+g}" for x in s)} | {fmt(p)} | {fmt(t)} | {err_levels(c, s):+.1f} |')
    out.append('')
    out.append('### 4. Maps the game still solves: the solve curve alone (walls, objects) and before the floor curve\n')
    out.append('Relative error of the plain apply, percent of the true pixel. Walls: by the map mean. Floors: by the '
               'pixel mean, albedo 0.65 and 1.\n')
    ss = (-0.5, -0.1, 0.1, 0.5)
    out.append('| surface | mean | ' + ' | '.join(f's = {s:+g}' for s in ss) + ' |')
    out.append('|---|---|' + '---|' * len(ss))
    for u in (0.2, 0.4, 0.6, 0.8):
        row = ' | '.join(f'{((1 + s) * u / math.tanh((1 + s) * math.atanh(u)) - 1) * 100:+.1f}%' for s in ss)
        out.append(f'| wall, map mean | {u:.1f} | {row} |')
    for a in (0.65, 1.0):
        for u in (0.2, 0.4, 0.6, 0.8):
            cells = []
            for s in ss:
                t = floor_after_solve(u, a, s)
                cells.append('-' if t is None else f'{(min((1 + s) * u, 1) / t - 1) * 100:+.1f}%')
            out.append(f'| floor, albedo {a:g}, pixel mean | {u:.1f} | {" | ".join(cells)} |')
    out.append('')
    return '\n'.join(out)


def selftest():
    # a linear pixel model has no error: the plain apply is the definition; the floor model must reduce to it in the dark
    assert abs(err_levels([0.004] * 3, [0.5] * 3)) < 0.01
    # the closed form against the curve applied forward, for two values of K (K must cancel)
    for k in (K_FLOOR, 2.1):
        for y, s in (((0.5, 0.3, 0.1), (0.4, -0.2, 0.0)), ((0.9, 0.9, 0.9), (-0.5,) * 3), ((0.05, 0.2, 0.1), (1.0,) * 3)):
            fwd = lambda v: [x * math.tanh(k * sum(v) / 3) / (sum(v) / 3) for x in v]
            c, want = fwd(y), fwd([y[i] * (1 + s[i]) for i in range(3)])
            got = truth(c, s)
            assert all(abs(got[i] - sat(want[i])) < 1e-9 for i in range(3)), (k, y, s, got, want)
    # the small-term slope against a finite difference
    for u in (0.2, 0.5, 0.8):
        fd = err_levels([u] * 3, [1e-4] * 3) / 255 / u / 1e-4
        assert abs(fd - first_order(u)) < 1e-3, (u, fd, first_order(u))
    # sign: the plain apply overshoots both ways (too bright when brightening, too dark when darkening)
    assert err_levels([0.6] * 3, [0.25] * 3) > 0 > err_levels([0.6] * 3, [-0.25] * 3)
    # full shadow and no term
    assert truth([0.6] * 3, [-1.0] * 3) == [0.0] * 3 and abs(err_levels([0.6, 0.4, 0.2], [0.0] * 3)) < 1e-9
    # the solve chain: no term gives the pixel back; an albedo too dark for the pixel is refused
    assert abs(floor_after_solve(0.4, 0.65, 0.0) - 0.4) < 1e-9 and floor_after_solve(0.9, 0.3, 0.1) is None
    print('selftest: pass')


def frames(paths, stride):
    try:
        from PIL import Image
    except ImportError:
        sys.exit('--frames needs Pillow (pip install pillow)')
    files = []
    for p in map(Path, paths):
        files += sorted(f for f in p.rglob('*') if f.suffix.lower() in ('.png', '.jpg', '.jpeg', '.bmp')) if p.is_dir() else [p]
    if not files:
        sys.exit('no frames')
    # the error depends on the pixel's colour only: count colours, then evaluate each once
    seen = {}
    for f in files:
        px = Image.open(f).convert('RGB').tobytes()
        for i in range(0, len(px) - 2, 3 * stride):
            key = px[i:i + 3]
            seen[key] = seen.get(key, 0) + 1
    n = sum(seen.values())
    print(f'{len(files)} frame(s), {n} sampled pixels (every {stride}th), {len(seen)} colours; every pixel taken as a floor\n')
    print('| s | mean abs error (levels) | 95th pct | 99th pct | max | pixels over 1 level | over 2 | over 4 |')
    print('|---|---|---|---|---|---|---|---|')
    for s in TERMS:
        hist = sorted((abs(err_levels([b / 255 for b in key], [s] * 3)), w) for key, w in seen.items())
        acc, p95, p99, mean = 0, None, None, 0.0
        over = [0, 0, 0]
        for e, w in hist:
            acc += w
            mean += e * w
            if p95 is None and acc >= 0.95 * n:
                p95 = e
            if p99 is None and acc >= 0.99 * n:
                p99 = e
            for j, cap in enumerate((1, 2, 4)):
                over[j] += w if e > cap else 0
        print(f'| {s:+g} | {mean / n:.2f} | {p95:.1f} | {p99:.1f} | {hist[-1][0]:.1f} | '
              + ' | '.join(f'{100 * o / n:.1f}%' for o in over) + ' |')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--frames', nargs='+')
    ap.add_argument('--stride', type=int, default=7)
    ap.add_argument('--write', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.frames:
        return frames(a.frames, max(a.stride, 1))
    text = tables()
    if a.write:
        Path(__file__).with_name('results.md').write_text(
            '# Multiplicative apply error: tables\n\nWritten by `run.py --write`. Levels are 8-bit levels of the '
            'finished pixel; positive means the plain apply is too bright.\n\n' + text, encoding='utf-8', newline='\n')
    print(text)


if __name__ == '__main__':
    main()
