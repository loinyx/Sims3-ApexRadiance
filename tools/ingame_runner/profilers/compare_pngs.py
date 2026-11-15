"""Compare two screenshots by their means: mean RGB, saturation and luminance of each, and whether they match.

Used after jobs/looks-world-only.json: the world moves between game starts, so pixels differ, but the same look on
the same paused view gives the same means. Each argument is a PNG or a folder (its newest PNG, searched recursively,
for example a load's captures folder). Exit 0 on a match, 1 on a difference, 2 on a bad argument.

    python -I tools/ingame_runner/profilers/compare_pngs.py <A.png or folder> <B.png or folder> [--lum 2] [--sat 2]
    python -I tools/ingame_runner/profilers/compare_pngs.py --selftest
"""
import argparse
import sys
from pathlib import Path

from PIL import Image, ImageStat


def newest_png(p):
    p = Path(p)
    if p.is_file():
        return p
    pngs = sorted(p.rglob('*.png'), key=lambda f: f.stat().st_mtime) if p.is_dir() else []
    if not pngs:
        sys.exit(f'no PNG at {p}')
    return pngs[-1]


def means(img):
    rgb = img.convert('RGB')
    r, g, b = ImageStat.Stat(rgb).mean
    sat = ImageStat.Stat(rgb.convert('HSV').getchannel('S')).mean[0] / 255 * 100  # HSV saturation, percent
    lum = 0.2126 * r + 0.7152 * g + 0.0722 * b  # Rec. 709 weights on the 0-255 values
    return (r, g, b), sat, lum


def compare(a, b, lum_tol, sat_tol):
    (ra, sa, la), (rb, sb, lb) = means(a), means(b)
    return (ra, sa, la), (rb, sb, lb), abs(la - lb) <= lum_tol and abs(sa - sb) <= sat_tol


def selftest():
    base = Image.new('RGB', (64, 64), (120, 80, 40))
    near = Image.new('RGB', (64, 64), (121, 81, 41))
    far = Image.new('RGB', (64, 64), (160, 80, 40))
    assert compare(base, near, 2, 2)[2], 'one level apart should match'
    assert not compare(base, far, 2, 2)[2], 'a warmer image should not match'
    (_, s, l), _, _ = compare(Image.new('RGB', (4, 4), (255, 255, 255)), base, 2, 2)
    assert s == 0 and abs(l - 255) < 1e-6, 'white: no saturation, full luminance'
    print('compare_pngs selftest: ok')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('a', nargs='?')
    ap.add_argument('b', nargs='?')
    ap.add_argument('--lum', type=float, default=2.0, help='largest mean luminance difference, 0-255 levels (2)')
    ap.add_argument('--sat', type=float, default=2.0, help='largest mean saturation difference, percent points (2)')
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not (args.a and args.b):
        ap.print_usage()
        sys.exit(2)
    pa, pb = newest_png(args.a), newest_png(args.b)
    with Image.open(pa) as ia, Image.open(pb) as ib:
        if ia.size != ib.size:
            print(f'note: sizes differ ({ia.size[0]}x{ia.size[1]} and {ib.size[0]}x{ib.size[1]}); means still compare')
        (ra, sa, la), (rb, sb, lb), ok = compare(ia, ib, args.lum, args.sat)
    for name, p, rgb, s, l in (('A', pa, ra, sa, la), ('B', pb, rb, sb, lb)):
        print(f'{name} {p}\n  mean RGB {rgb[0]:.2f} {rgb[1]:.2f} {rgb[2]:.2f}  saturation {s:.2f}%  luminance {l:.2f}')
    print(f'difference  RGB {rb[0]-ra[0]:+.2f} {rb[1]-ra[1]:+.2f} {rb[2]-ra[2]:+.2f}  '
          f'saturation {sb-sa:+.2f} points (limit {args.sat:g})  luminance {lb-la:+.2f} levels (limit {args.lum:g})')
    print('PASS: the world looks the same' if ok else 'FAIL: the world differs')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
