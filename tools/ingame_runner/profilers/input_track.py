"""The tester's own clicks and keys, recorded and played back exactly (Windows low-level hooks and SendInput).

  python input_track.py record <out.json> <stop file>   records until the stop file exists
  python input_track.py play <in.json> [<stop file>]     plays them back (stops early if the stop file appears or
                                                          the tester moves the real mouse or presses a real key)
  python input_track.py session <dir> <stop file> [--no-clips]  F9 (unless --no-clips) (kept from the game) starts and stops a clip, as often as the
                                                          tester likes: <dir>/clip-N/input.json, then a small window
                                                          asks what went wrong (desc.txt). Markers for the runner:
                                                          clip-N/start at its start, clip-N/stopped at its end (each
                                                          holds the ms since the session's start). The whole session
                                                          is kept too: <dir>/all.json, every event since the start

Mouse places are kept relative to the game window's client area (TS3W.exe), so a moved or resized window still gets the
clicks in the same place. Times count from the start of the recording; a playback starts when the script starts, so the
runner starts both at the same point (the save loaded and settled). Keys go by scan code (what the game reads).
Playback brings the game window to the front: the mouse and keyboard are the game's until it ends.
"""
import ctypes
import ctypes.wintypes as wt
import json
import os
import sys
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
try:
    ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)  # (physical pixels, as the game window)
except Exception:
    user32.SetProcessDPIAware()

WH_KEYBOARD_LL, WH_MOUSE_LL = 13, 14
WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN, WM_SYSKEYUP = 0x100, 0x101, 0x104, 0x105
WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_RBUTTONDOWN, WM_RBUTTONUP = 0x200, 0x201, 0x202, 0x204, 0x205
WM_MBUTTONDOWN, WM_MBUTTONUP, WM_MOUSEWHEEL = 0x207, 0x208, 0x20A
LLMHF_INJECTED, LLKHF_INJECTED, LLKHF_EXTENDED = 0x1, 0x10, 0x1
PM_REMOVE = 1


class KBDLLHOOKSTRUCT(ctypes.Structure):
    _fields_ = [("vkCode", wt.DWORD), ("scanCode", wt.DWORD), ("flags", wt.DWORD), ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class MSLLHOOKSTRUCT(ctypes.Structure):
    _fields_ = [("pt", wt.POINT), ("mouseData", wt.DWORD), ("flags", wt.DWORD), ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


HOOKPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, ctypes.c_int, wt.WPARAM, wt.LPARAM)
user32.SetWindowsHookExW.argtypes = [ctypes.c_int, HOOKPROC, wt.HINSTANCE, wt.DWORD]
user32.SetWindowsHookExW.restype = wt.HHOOK
user32.CallNextHookEx.argtypes = [wt.HHOOK, ctypes.c_int, wt.WPARAM, wt.LPARAM]
user32.CallNextHookEx.restype = ctypes.c_ssize_t
WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


def game_window():
    """The visible top-level window of TS3W.exe with the largest client area."""
    best = [None, 0]

    def cb(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        h = kernel32.OpenProcess(0x1000, False, pid.value)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return True
        buf = ctypes.create_unicode_buffer(260)
        n = wt.DWORD(260)
        ok = kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n))
        kernel32.CloseHandle(h)
        if ok and buf.value.lower().endswith("ts3w.exe"):
            r = wt.RECT()
            user32.GetClientRect(hwnd, ctypes.byref(r))
            if r.right * r.bottom > best[1]:
                best[0], best[1] = hwnd, r.right * r.bottom
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return best[0]


def client_rect(hwnd):
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    p = wt.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(p))
    return p.x, p.y, max(1, r.right), max(1, r.bottom)


def pump_until(done):
    msg = wt.MSG()
    while not done():
        while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, PM_REMOVE):
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        time.sleep(0.001)


def record(out, stop):
    hwnd = game_window()
    if not hwnd:
        sys.exit("TS3W.exe window not found")
    t0 = time.perf_counter()
    ev, last_move = [], [0.0]
    rect = [client_rect(hwnd), t0]

    def rel(x, y):
        now = time.perf_counter()
        if now - rect[1] > 1.0:  # (the window may move: its place read again each second)
            rect[0], rect[1] = client_rect(hwnd), now
        cx, cy, w, h = rect[0]
        return round((x - cx) / w, 5), round((y - cy) / h, 5)

    def on_mouse(code, wp, lp):
        if code == 0:
            m = ctypes.cast(lp, ctypes.POINTER(MSLLHOOKSTRUCT)).contents
            t = round((time.perf_counter() - t0) * 1000, 1)
            x, y = rel(m.pt.x, m.pt.y)
            if wp == WM_MOUSEMOVE:
                if t - last_move[0] >= 8:  # (about 120 moves a second at most)
                    last_move[0] = t
                    ev.append([t, "move", x, y])
            elif wp in (WM_LBUTTONDOWN, WM_LBUTTONUP, WM_RBUTTONDOWN, WM_RBUTTONUP, WM_MBUTTONDOWN, WM_MBUTTONUP):
                ev.append([t, {WM_LBUTTONDOWN: "ld", WM_LBUTTONUP: "lu", WM_RBUTTONDOWN: "rd", WM_RBUTTONUP: "ru",
                               WM_MBUTTONDOWN: "md", WM_MBUTTONUP: "mu"}[wp], x, y])
            elif wp == WM_MOUSEWHEEL:
                ev.append([t, "wheel", x, y, ctypes.c_short(m.mouseData >> 16).value])
        return user32.CallNextHookEx(None, code, wp, lp)

    def on_key(code, wp, lp):
        if code == 0:
            k = ctypes.cast(lp, ctypes.POINTER(KBDLLHOOKSTRUCT)).contents
            t = round((time.perf_counter() - t0) * 1000, 1)
            ev.append([t, "kd" if wp in (WM_KEYDOWN, WM_SYSKEYDOWN) else "ku", k.vkCode, k.scanCode, k.flags & LLKHF_EXTENDED])
        return user32.CallNextHookEx(None, code, wp, lp)

    mp, kp = HOOKPROC(on_mouse), HOOKPROC(on_key)
    hm = user32.SetWindowsHookExW(WH_MOUSE_LL, mp, None, 0)
    hk = user32.SetWindowsHookExW(WH_KEYBOARD_LL, kp, None, 0)
    if not hm or not hk:
        sys.exit("hooks not set (error %d)" % ctypes.get_last_error())
    pump_until(lambda: os.path.exists(stop))
    user32.UnhookWindowsHookEx(hm), user32.UnhookWindowsHookEx(hk)
    json.dump({"window": list(rect[0][2:]), "events": ev}, open(out, "w"))
    print("%d events, %.1f s" % (len(ev), (time.perf_counter() - t0)))


VK_F9 = 0x78


def ask_description(n):
    import tkinter as tk
    from tkinter import simpledialog
    root = tk.Tk()
    root.withdraw()
    root.attributes("-topmost", True)
    root.focus_force()
    text = simpledialog.askstring("Clip %d" % n, "What went wrong in this clip? (empty: nothing)", parent=root)
    root.destroy()
    return text or ""


def session(folder, stop, clips=True):
    hwnd = game_window()
    if not hwnd:
        sys.exit("TS3W.exe window not found")
    s0 = time.perf_counter()
    st = {"on": False, "n": 0, "t0": 0.0, "ev": [], "last": 0.0, "toggle": False, "asking": False, "all": [], "lastAll": 0.0}
    open(os.path.join(folder, "session-start"), "w").write("%.3f" % time.time())

    def ms():
        return round((time.perf_counter() - s0) * 1000, 1)
    rect = [client_rect(hwnd), time.perf_counter()]

    def rel(x, y):
        now = time.perf_counter()
        if now - rect[1] > 1.0:
            rect[0], rect[1] = client_rect(hwnd), now
        cx, cy, w, h = rect[0]
        return round((x - cx) / w, 5), round((y - cy) / h, 5)

    def add(e, move):  # (one event: to the whole session, and to the clip running)
        ta = ms()
        if not move or ta - st["lastAll"] >= 8:
            if move:
                st["lastAll"] = ta
            st["all"].append([ta] + e)
        if st["on"]:
            t = round((time.perf_counter() - st["t0"]) * 1000, 1)
            if not move or t - st["last"] >= 8:
                if move:
                    st["last"] = t
                st["ev"].append([t] + e)

    def on_mouse(code, wp, lp):
        if code == 0:
            m = ctypes.cast(lp, ctypes.POINTER(MSLLHOOKSTRUCT)).contents
            x, y = rel(m.pt.x, m.pt.y)
            if wp == WM_MOUSEMOVE:
                add(["move", x, y], True)
            elif wp in (WM_LBUTTONDOWN, WM_LBUTTONUP, WM_RBUTTONDOWN, WM_RBUTTONUP, WM_MBUTTONDOWN, WM_MBUTTONUP):
                add([{WM_LBUTTONDOWN: "ld", WM_LBUTTONUP: "lu", WM_RBUTTONDOWN: "rd", WM_RBUTTONUP: "ru",
                      WM_MBUTTONDOWN: "md", WM_MBUTTONUP: "mu"}[wp], x, y], False)
            elif wp == WM_MOUSEWHEEL:
                add(["wheel", x, y, ctypes.c_short(m.mouseData >> 16).value], False)
        return user32.CallNextHookEx(None, code, wp, lp)

    def on_key(code, wp, lp):
        if code == 0:
            k = ctypes.cast(lp, ctypes.POINTER(KBDLLHOOKSTRUCT)).contents
            if clips and k.vkCode == VK_F9 and not k.flags & LLKHF_INJECTED:
                if wp in (WM_KEYDOWN, WM_SYSKEYDOWN) and not st["asking"]:
                    st["toggle"] = True
                return 1  # (the game never sees F9)
            add(["kd" if wp in (WM_KEYDOWN, WM_SYSKEYDOWN) else "ku", k.vkCode, k.scanCode, k.flags & LLKHF_EXTENDED], False)
        return user32.CallNextHookEx(None, code, wp, lp)

    def save(d):
        json.dump({"window": list(rect[0][2:]), "events": st["ev"]}, open(os.path.join(d, "input.json"), "w"))
        open(os.path.join(d, "stopped"), "w").write("%.1f" % ms())

    def save_all():
        json.dump({"window": list(rect[0][2:]), "events": st["all"]}, open(os.path.join(folder, "all.json"), "w"))

    mp, kp = HOOKPROC(on_mouse), HOOKPROC(on_key)
    hm = user32.SetWindowsHookExW(WH_MOUSE_LL, mp, None, 0)
    hk = user32.SetWindowsHookExW(WH_KEYBOARD_LL, kp, None, 0)
    if not hm or not hk:
        sys.exit("hooks not set (error %d)" % ctypes.get_last_error())
    msg = wt.MSG()
    saved = time.perf_counter()
    while not os.path.exists(stop):
        if time.perf_counter() - saved > 30:  # (kept every 30 s: a game that closes keeps the session so far)
            saved = time.perf_counter()
            save_all()
        while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, PM_REMOVE):
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        if st["toggle"]:
            st["toggle"] = False
            d = os.path.join(folder, "clip-%d" % (st["n"] + (0 if st["on"] else 1)))
            if not st["on"]:
                st["n"] += 1
                os.makedirs(d, exist_ok=True)
                st["ev"], st["last"], st["t0"], st["on"] = [], 0.0, time.perf_counter(), True
                open(os.path.join(d, "start"), "w").write("%.1f" % ms())
                print("clip %d started" % st["n"], flush=True)
            else:
                st["on"] = False
                save(d)
                print("clip %d stopped: %d events, %.1f s" % (st["n"], len(st["ev"]), time.perf_counter() - st["t0"]), flush=True)
                st["asking"] = True
                try:
                    desc = ask_description(st["n"])
                except Exception as e:
                    desc = "(no description window: %s)" % e
                st["asking"] = False
                open(os.path.join(d, "desc.txt"), "w", encoding="utf-8").write(desc)
                print("clip %d: %s" % (st["n"], desc), flush=True)
        time.sleep(0.001)
    if st["on"]:  # (a clip still running when the session ends is kept)
        save(os.path.join(folder, "clip-%d" % st["n"]))
    save_all()
    user32.UnhookWindowsHookEx(hm), user32.UnhookWindowsHookEx(hk)


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD), ("dwFlags", wt.DWORD), ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD), ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class _U(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("u", _U)]


MOVE, ABS, VDESK = 0x1, 0x8000, 0x4000
BTN = {"ld": 0x2, "lu": 0x4, "rd": 0x8, "ru": 0x10, "md": 0x20, "mu": 0x40}
KEY_UP, KEY_SCAN, KEY_EXT = 0x2, 0x8, 0x1


def send(i):
    user32.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def play(path, stop):
    data = json.load(open(path))
    hwnd = game_window()
    if not hwnd:
        sys.exit("TS3W.exe window not found")
    # (to the front: a foreground lock is lifted by a key press of ours first)
    send(INPUT(1, _U(ki=KEYBDINPUT(0x12, 0, 0, 0, 0))))
    send(INPUT(1, _U(ki=KEYBDINPUT(0x12, 0, KEY_UP, 0, 0))))
    user32.SetForegroundWindow(hwnd)
    vx, vy = user32.GetSystemMetrics(76), user32.GetSystemMetrics(77)
    vw, vh = user32.GetSystemMetrics(78), user32.GetSystemMetrics(79)
    real = [False]

    def on_real(code, wp, lp):  # (the tester takes over: a real mouse move or key ends the playback)
        if code == 0:
            if wp in (WM_KEYDOWN, WM_SYSKEYDOWN):
                if not ctypes.cast(lp, ctypes.POINTER(KBDLLHOOKSTRUCT)).contents.flags & LLKHF_INJECTED:
                    real[0] = True
        return user32.CallNextHookEx(None, code, wp, lp)

    def on_real_mouse(code, wp, lp):
        if code == 0 and wp != WM_MOUSEMOVE:
            if not ctypes.cast(lp, ctypes.POINTER(MSLLHOOKSTRUCT)).contents.flags & LLMHF_INJECTED:
                real[0] = True
        return user32.CallNextHookEx(None, code, wp, lp)

    kp, mp = HOOKPROC(on_real), HOOKPROC(on_real_mouse)
    hk = user32.SetWindowsHookExW(WH_KEYBOARD_LL, kp, None, 0)
    hm = user32.SetWindowsHookExW(WH_MOUSE_LL, mp, None, 0)
    t0 = time.perf_counter()
    sent, msg, held = 0, wt.MSG(), {}
    rect, rect_at = client_rect(hwnd), t0
    for e in data["events"]:
        while (time.perf_counter() - t0) * 1000 < e[0]:
            while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, PM_REMOVE):
                user32.DispatchMessageW(ctypes.byref(msg))
            if real[0] or (stop and os.path.exists(stop)):
                break
            time.sleep(0.0005)
        if real[0] or (stop and os.path.exists(stop)):
            print("stopped at %.1f s (%s)" % (e[0] / 1000, "the tester took over" if real[0] else "stop file"))
            break
        if time.perf_counter() - rect_at > 1.0:
            rect, rect_at = client_rect(hwnd), time.perf_counter()
        kind = e[1]
        if kind in ("kd", "ku"):
            fl = KEY_SCAN | (KEY_EXT if e[4] else 0) | (KEY_UP if kind == "ku" else 0)
            send(INPUT(1, _U(ki=KEYBDINPUT(0, e[3], fl, 0, 0))))
            if kind == "kd":
                held[e[3]] = fl & KEY_EXT
            else:
                held.pop(e[3], None)
        else:
            cx, cy, w, h = rect
            x, y = cx + e[2] * w, cy + e[3] * h
            ax, ay = int((x - vx) * 65535 / max(1, vw - 1)), int((y - vy) * 65535 / max(1, vh - 1))
            fl = MOVE | ABS | VDESK
            data_ = 0
            if kind in BTN:
                fl |= BTN[kind]
            elif kind == "wheel":
                fl |= 0x800
                data_ = e[4] & 0xFFFFFFFF
            send(INPUT(0, _U(mi=MOUSEINPUT(ax, ay, data_, fl, 0, 0))))
        sent += 1
    for h in (hk, hm):
        user32.UnhookWindowsHookEx(h)
    for scan, ext in held.items():  # (no key left held down by a playback stopped early)
        send(INPUT(1, _U(ki=KEYBDINPUT(0, scan, KEY_SCAN | KEY_UP | ext, 0, 0))))
    print("%d of %d events played in %.1f s" % (sent, len(data["events"]), time.perf_counter() - t0))


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] not in ("record", "play", "session"):
        sys.exit(__doc__)
    if sys.argv[1] == "session":
        if len(sys.argv) < 4:
            sys.exit(__doc__)
        session(sys.argv[2], sys.argv[3], "--no-clips" not in sys.argv)
    elif sys.argv[1] == "record":
        if len(sys.argv) < 4:
            sys.exit(__doc__)
        record(sys.argv[2], sys.argv[3])
    else:
        play(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
