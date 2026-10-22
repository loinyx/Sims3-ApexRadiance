"""Attaches the profiler scripts to TS3W.exe for a fixed time, then unloads them (each flushes its files on unload).

Usage: python attach.py [--keep] <pid> <seconds> <script.js> <params-json> [<script.js> <params-json> ...]
Needs Python with the frida package. One Frida session for all scripts; they unload in reverse load order.
--keep: after <seconds>, call each script's close() export (it writes its last rows and closes its files) and leave the
session loaded until the game exits. Used for the frame recorder: unloading its hook on the main thread's per-frame path
while the game runs is where an end-of-profile crash was seen.
Adapted from sims3-modernization tools/harness (MIT, see ../LICENSE-sims3-modernization).
"""
import json
import sys
import threading
import time

import frida


def main():
    args = sys.argv[1:]
    keep = bool(args) and args[0] == "--keep"
    if keep:
        args = args[1:]
    if len(args) < 4 or len(args) % 2:
        sys.exit(__doc__)
    pid, seconds, pairs = int(args[0]), float(args[1]), args[2:]
    session = frida.attach(pid)
    gone = threading.Event()
    session.on("detached", lambda *_: gone.set())
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
        if keep:
            for script in reversed(scripts):
                script.exports_sync.close()
            print("closed; the session stays loaded until the game exits", flush=True)
            gone.wait()
            return
    finally:
        if not keep or not gone.is_set():
            for script in reversed(scripts):
                try:
                    script.unload()
                except frida.InvalidOperationError:
                    pass  # the game exited first
            try:
                session.detach()
            except frida.InvalidOperationError:
                pass


if __name__ == "__main__":
    main()
