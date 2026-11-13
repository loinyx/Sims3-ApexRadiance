"""Records what the game window shows, independent of the mod and of the game: Windows Graphics Capture (the API of the
Xbox Game Bar and OBS), which reads a window even when it is shown by independent flip or covered by other windows
(PrintWindow read those as black frames). Nothing is injected into the game.

Usage: python grab_wgc.py <game pid> <out dir> [fps=20]
Writes luma.csv (qpcMs, clock as in Apex's log, frame, then the mean luma of a 16 x 9 grid of cells) and each frame as a
480x270 NNNNNN.jpg; flash_scan.py reads them. Ends when the game exits.
Needs: pip install windows-capture (it brings numpy and OpenCV).
"""
import ctypes
import ctypes.wintypes as wt
import datetime
import os
import sys
import time

import cv2
import numpy
from windows_capture import WindowsCapture

user32, kernel32 = ctypes.windll.user32, ctypes.windll.kernel32


def main_window(pid):
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def each(hwnd, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd) and not user32.GetWindow(hwnd, 4):  # GW_OWNER: a top window
            found.append(hwnd)
        return True

    user32.EnumWindows(each, 0)
    return found[0] if found else None


def qpc_ms():
    c, f = ctypes.c_int64(), ctypes.c_int64()
    kernel32.QueryPerformanceCounter(ctypes.byref(c))
    kernel32.QueryPerformanceFrequency(ctypes.byref(f))
    return c.value * 1000.0 / f.value


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    pid, out = int(sys.argv[1]), sys.argv[2]
    period = 1000.0 / (float(sys.argv[3]) if len(sys.argv) > 3 else 20.0)
    os.makedirs(out, exist_ok=True)
    proc = kernel32.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
    hwnd = None
    while hwnd is None:
        if kernel32.WaitForSingleObject(proc, 0) == 0:
            return
        hwnd = main_window(pid)
        time.sleep(0.5)
    csv = open(os.path.join(out, "luma.csv"), "w", newline="")
    csv.write("qpcMs,clock,frame," + ",".join("c%d" % i for i in range(144)) + "\n")
    state = {"next": 0.0, "n": 0}
    cap = WindowsCapture(cursor_capture=False, draw_border=False, window_hwnd=hwnd)

    @cap.event
    def on_frame_arrived(frame, control):
        if kernel32.WaitForSingleObject(proc, 0) == 0:
            control.stop()
            return
        t = qpc_ms()
        if t < state["next"]:
            return
        state["next"] = max(state["next"] + period, t)
        small = cv2.resize(frame.frame_buffer[:, :, :3], (480, 270), interpolation=cv2.INTER_AREA)
        luma = small[:, :, 0] * 0.0722 + small[:, :, 1] * 0.7152 + small[:, :, 2] * 0.2126
        cells = cv2.resize(luma.astype(numpy.float32), (16, 9), interpolation=cv2.INTER_AREA).ravel()
        n = state["n"]
        cv2.imwrite(os.path.join(out, "%06d.jpg" % n), small, [cv2.IMWRITE_JPEG_QUALITY, 80])
        clock = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        csv.write("%.1f,%s,%d,%s\n" % (t, clock, n, ",".join("%.1f" % v for v in cells)))
        state["n"] = n + 1

    @cap.event
    def on_closed():
        csv.close()

    cap.start()  # blocks until stopped or the window closes
    if not csv.closed:
        csv.close()


if __name__ == "__main__":
    main()
