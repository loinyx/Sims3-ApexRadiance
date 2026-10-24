"""Package the temporary script observer without replacing game resources."""
import pathlib
import struct
import sys
import xml.etree.ElementTree as ET


def resource_id(name):
    value = 0xCBF29CE484222325
    for byte in name.lower().encode("ascii"):
        value = ((value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF) ^ byte
    return value


def package(script, tuning, target):
    ET.fromstring(tuning)
    entries = [(0x073FAA07, resource_id("ApexLoadingStudy"), script),
               (0x0333406C, resource_id("ApexRadiance.LoadingStudy.Bootstrap"), tuning)]
    data = bytearray(96)
    data[:4] = b"DBPF"
    struct.pack_into("<I", data, 4, 2)
    struct.pack_into("<I", data, 36, len(entries))
    struct.pack_into("<I", data, 44, 8 + 28 * len(entries))
    struct.pack_into("<I", data, 60, 3)
    index = []
    for kind, key, payload in entries:
        index.append((kind, key >> 32, key & 0xFFFFFFFF, len(data),
                      len(payload) | 0x80000000, len(payload), 0x10000))
        data.extend(payload)
    struct.pack_into("<I", data, 64, len(data))
    data.extend(struct.pack("<2I", 2, 0))
    for row in index:
        data.extend(struct.pack("<7I", *row))
    target.write_bytes(data)
    # Validate the written index and exact resource payloads.
    reread = target.read_bytes()
    offset = struct.unpack_from("<I", reread, 64)[0] + 8
    for i, (kind, key, payload) in enumerate(entries):
        row = struct.unpack_from("<7I", reread, offset + 28 * i)
        assert row[:3] == (kind, key >> 32, key & 0xFFFFFFFF)
        assert reread[row[3]:row[3] + (row[4] & 0x7FFFFFFF)] == payload
    return len(data)


if __name__ == "__main__":
    script, tuning, target = map(pathlib.Path, sys.argv[1:])
    print("Script observer package verified:", package(script.read_bytes(), tuning.read_bytes(), target), "bytes")
