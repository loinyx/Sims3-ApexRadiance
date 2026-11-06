#!/usr/bin/env python3
"""Offline checks of generate.py (no game files): legs out of a mesh, what is no table, the chain of a slanted leg."""
from generate import chain, legs, top_height


def box(x, z, y0, y1, h, p, t, shift=0.0):
    """An upright box, its top end moved by `shift` along x, appended to positions p and triangles t."""
    b = len(p)
    for y, s in ((y0, 0.0), (y1, shift)):
        p += [(x + s - h, y, z - h), (x + s + h, y, z - h), (x + s + h, y, z + h), (x + s - h, y, z + h)]
    for i in range(4):
        j = (i + 1) % 4
        t += [(b + i, b + j, b + 4 + j), (b + i, b + 4 + j, b + 4 + i)]


# four upright legs under a top at 0.7 m: one capsule each, at their place, about their half width
p, t = [], []
for x in (-0.3, 0.3):
    for z in (-0.3, 0.3):
        box(x, z, 0.0, 0.7, 0.02, p, t)
s = legs(p, t, 0.7)
assert len(s) == 4, s
assert all(abs(abs(c[0]) - 0.3) < 1e-6 and abs(abs(c[2]) - 0.3) < 1e-6 and c[1] < 0.15 and c[4] > 0.6 and abs(c[6] - 0.02) < 1e-6 for c in s), s

# nothing reaching the floor is no leg (a towel rack), a stub shorter than 15 cm neither
p, t = [], []
box(0.0, 0.0, 0.4, 0.7, 0.02, p, t)
assert legs(p, t, 0.7) == []
p, t = [], []
box(0.0, 0.0, 0.0, 0.1, 0.02, p, t)
assert legs(p, t, 0.7) == []

# a slanted leg stays one capsule, a bent one takes a joint
track = [(0.1 * i, 0.1 * i, 0.0, 0.02) for i in range(5)]
assert len(chain(track)) == 1
track = [(0.0, 0.1 * i, 0.0, 0.02) for i in range(4)] + [(0.1 * i, 0.3 + 0.1 * i, 0.0, 0.02) for i in range(1, 5)]
assert len(chain(track)) == 2, chain(track)

# (type, origin, normal, ...): a level top qualifies, an upright panel beside it is a body, no level top none
level = (1, (0, 0.7, 0, 0, 1, 0) + (0.0,) * 7)
panel = (1, (0, 0.3, 0, 1, 0, 0) + (0.0,) * 7)
assert top_height([level]) == 0.7
assert top_height([level, panel]) is None
assert top_height([]) is None
print("object_legs_table checks passed")
