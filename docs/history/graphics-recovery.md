# Graphics recovery: history

## 2026-10-09 — Resource restart and module lifetime

Added a render-thread restart request for Color, AO, Depth Blur and Edge Smoothing resources. The action keeps settings and hooks in place. Windows caches and other programs are outside its scope.

Review identified unsafe live unload: existing workers and hooks could outlive the DLL mapping. Attachment now pins the module before hooks or workers start, and process detach avoids blocking teardown under the loader lock.

Failure tests exposed incomplete state capture; AO/Depth now fail closed and release partial references. Independent review of the caller then found that failed Depth Blur capture retained the backbuffer and latched its reentry flag. Scope guards corrected both; the next-frame and failed-copy entry cases passed. Neither the isolated tests nor the general gameplay report establish a causal fix for every historical visual symptom.
