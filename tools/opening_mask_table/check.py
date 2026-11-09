#!/usr/bin/env python3
"""Offline checks of generate.py (no game files): the hole of a mask, what is no mask, a DXT1 block and a RefPack stream."""
import struct

from generate import hole_shape, mask_values, refpack


def image(w, h, cut):
    return [bytearray(0 if cut(x, y) else 255 for x in range(w)) for y in range(h)]


# a door: columns 8..55 from the base up to row 32 (2.25 m), bottom rows lost to a DXT block
s = hole_shape(64, 128, image(64, 128, lambda x, y: 8 <= x < 56 and 32 <= y < 126))
assert s == (32, 223, 0) + (191,) * 5, s
# a window: a sill over the base stays
s = hole_shape(64, 128, image(64, 128, lambda x, y: 16 <= x < 48 and 32 <= y < 96))
assert s[:3] == (64, 191, 64) and s[3] == 191, s
# a round head: lower at the sides
s = hole_shape(64, 128, image(64, 128, lambda x, y: 8 <= x < 56 and y >= 32 + abs(x - 32)))
assert s[5] > s[3] and s[5] > s[7], s
# no hole, a cell cut whole, a painted texture
assert hole_shape(64, 128, image(64, 128, lambda x, y: False)) is None
assert hole_shape(64, 128, image(64, 128, lambda x, y: True)) is None
assert hole_shape(64, 128, [bytearray(100 + x for x in range(64)) for _ in range(128)]) is None

# one DXT1 block: c0 white, c1 black, every texel index 1 (black): G = 0 everywhere
dds = bytearray(128 + 8)
dds[:4] = b"DDS "
struct.pack_into("<II", dds, 12, 4, 4)
dds[84:88] = b"DXT1"
struct.pack_into("<HHI", dds, 128, 0xFFFF, 0x0000, 0x55555555)
w, h, img = mask_values(bytes(dds))
assert (w, h) == (4, 4) and all(v == 0 for row in img for v in row)

# a RefPack stream of three literals and its end code
assert refpack(bytes([0x10, 0xFB, 0, 0, 3, 0xFF, 65, 66, 67])) == b"ABC"
print("opening_mask_table checks passed")
