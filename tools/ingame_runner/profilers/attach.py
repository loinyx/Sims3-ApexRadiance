"""Attaches the profiler scripts to TS3W.exe for a fixed time, then unloads them (each flushes its files on unload).

Usage: python attach.py <pid> <seconds> <script.js> <params-json> [<script.js> <params-json> ...]
Needs Python with the frida package. One Frida session for all scripts; they unload in reverse load order.
Adapted from sims3-modernization tools/harness (MIT, see ../LICENSE-sims3-modernization).
"""
import json
import sys
import time

import frida


def main():
    pid, seconds, pairs = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3:]
    if not pairs or len(pairs) % 2:
        sys.exit(__doc__)
    session = frida.attach(pid)
    scripts = []
    try:
        for path, params in zip(pairs[::2], pairs[1::2]):
            with open(path, encoding="utf-8") as f:
                script = session.create_script(f.read())
            script.on("message", lambda m, d: print(m.get("payload", m), flush=True))
            script.load()
            script.exports_sync.init("late", json.loads(params))
            scripts.append(script)
        print("ready", flush=True)
        time.sleep(seconds)
    finally:
        for script in reversed(scripts):
            script.unload()
        session.detach()


if __name__ == "__main__":
    main()
