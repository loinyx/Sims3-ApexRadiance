// Copied from sims3-modernization (MIT, see ../LICENSE-sims3-modernization; https://github.com/rodneypantonial/sims3-modernization).
// The docs/ references below point to that repository. Run through attach.py by run.ps1 -Profiler; read-only hooks.
// Frame-time recorder for TS3W.exe (Steam build, SHA-256 10b2423e...0609).
// Hooks FUN_00ec9f00(bool present) (0xEC9F00-0xEC9FDE, __thiscall, RET 4), verified in Ghidra 2026-10-13 (docs/04 F19):
//   - BeginScene (0x611620, device vtable +0xA4), scene render, StreamHost flush, then with present != 0 EndScene + Present
//     (0x611760 -> 0x611680, vtable +0xA8 / +0x44), then the inactive-window ~30 FPS sleep at 0xEC9FBA (S3SS Smooth Patch
//     anchor: S3SS jumps out there and back; the prologue stays intact). No tail calls, no other exits.
//   - Callers: the main loop FUN_00eca960 (entry -> __tmainCRTStartup -> 0xECBE30 -> 0xECBD00 -> 0xECA960, so the main/render
//     thread, not the sim thread) calls it with present = 1 once per loop iteration at 0xECAAFC; the window-message path
//     0xECA2A0 also with 1; the callback 0xECA030 (run by 0x9DE140 earlier in the same iteration) with present = 0, which
//     renders without presenting. Only present = 1 calls are frames; present = 0 calls are counted, not recorded.
// The hook is a CModule: the main thread never takes Frida's JS lock, so the probe's samplers (which suspend the sim thread,
// possibly while it holds that lock in stutter-profile.js) cannot stall a frame or deadlock with it. No thread is suspended.
// Output <out>: CSV `epochMs,qpcMs,frameMs,workMs,fg` per presented frame. qpcMs = QueryPerformanceCounter in ms at the call
// (the clock stutter-profile.js stamps sim hitches with), frameMs = time since the previous frame started, workMs = time
// inside the call (render + Present, incl. DXVK's frame cap and S3SS's limiter), fg = 1 when the game window had focus
// (unfocused, the game caps itself at ~30 FPS, so B numbers only count with fg = 1).
'use strict';

const FRAME = [0xec9f00, '83 ec 20 56 e8 b7 43 74']; // sub esp,20h; push esi; call 0x60E2C0 (graphics device)
const N = 8192; // ring slots: ~2 min at 60 FPS, drained every 250 ms

const k32 = Process.getModuleByName('kernel32.dll');
const QPC = k32.getExportByName('QueryPerformanceCounter');
const qpc = new NativeFunction(QPC, 'int', ['pointer']);
const qpf = new NativeFunction(k32.getExportByName('QueryPerformanceFrequency'), 'int', ['pointer']);
const qbuf = Memory.alloc(8);
qpf(qbuf);
const ticksPerMs = Number(qbuf.readS64()) / 1000;
const nowMs = () => { qpc(qbuf); return Number(qbuf.readS64()) / ticksPerMs; };
const epoch0 = Date.now(), q0 = nowMs();

const u32 = Process.getModuleByName('user32.dll');
const getFg = new NativeFunction(u32.getExportByName('GetForegroundWindow'), 'pointer', []);
const getPid = new NativeFunction(u32.getExportByName('GetWindowThreadProcessId'), 'uint32', ['pointer', 'pointer']);
const pidBuf = Memory.alloc(4);
function foreground() { const h = getFg(); if (h.isNull()) return 0; getPid(h, pidBuf); return pidBuf.readU32() === Process.id ? 1 : 0; }

const C = `
#include <gum/guminterceptor.h>
#define N ${N}
typedef struct { gint64 start, end; } Rec;
typedef struct { gint64 start; } State;
extern int __attribute__((stdcall)) QueryPerformanceCounter (gint64 * t);
/* in JS-allocated memory: CModule's own data is not writable on Windows (dummy run, 2026-10-13) */
extern volatile guint32 st[4]; /* head, main_tid, other_thread, no_present */
extern Rec recs[N];
#define head st[0]
#define main_tid st[1]
#define other_thread st[2]
#define no_present st[3]

void onEnter (GumInvocationContext * ic) {
  State * s = GUM_IC_GET_INVOCATION_DATA (ic, State);
  guint32 tid = gum_invocation_context_get_thread_id (ic);
  s->start = 0;
  if (main_tid == 0) main_tid = tid;
  if (tid != main_tid) { other_thread++; return; }
  if (gum_invocation_context_get_nth_argument (ic, 0) == NULL) { no_present++; return; }
  QueryPerformanceCounter (&s->start);
}

void onLeave (GumInvocationContext * ic) {
  State * s = GUM_IC_GET_INVOCATION_DATA (ic, State);
  gint64 end;
  if (s->start == 0) return;
  QueryPerformanceCounter (&end);
  recs[head % N].start = s->start;
  recs[head % N].end = end;
  head++; /* x86 keeps store order: the record is written before head moves */
}
`;

const mem = Memory.alloc(16 + N * 16); // st[4] + recs[N], zeroed
const ST = i => mem.add(4 * i).readU32(), recs = mem.add(16);
let cm = null, file = null, listener = null, timer = null, read = 0, frames = 0, lost = 0, last = 0, fg = 1, lastFg = 0;
const r2 = x => Math.round(x * 100) / 100;

function drain() {
  const head = ST(0);
  if (head - read > N) { lost += head - read - N; read = head - N; last = 0; } // overwritten: restart the frame chain
  const out = [];
  for (; read !== head; read++) {
    const rec = recs.add((read % N) * 16);
    const t = Number(rec.readS64()) / ticksPerMs, end = Number(rec.add(8).readS64()) / ticksPerMs;
    if (last) { out.push(`${Math.round(epoch0 + t - q0)},${r2(t)},${r2(t - last)},${r2(end - t)},${fg}\n`); frames++; }
    last = t;
  }
  if (out.length) { file.write(out.join('')); file.flush(); }
  const n = nowMs();
  if (n - lastFg >= 1000) { fg = foreground(); lastFg = n; }
}

function install(out) {
  if (!Process.getModuleByName('TS3W.exe').base.equals(ptr(0x400000))) throw new Error('TS3W.exe not at 0x400000');
  const p = ptr(FRAME[0]);
  const got = Array.from(new Uint8Array(p.readByteArray(8))).map(b => b.toString(16).padStart(2, '0')).join(' ');
  if (got !== FRAME[1]) throw new Error(`frame fn @ ${p}: bytes ${got}, expected ${FRAME[1]} (hooked by another session, detoured, or a different build)`);
  cm = new CModule(C, { QueryPerformanceCounter: QPC, st: mem, recs });
  file = new File(out, 'w');
  file.write('epochMs,qpcMs,frameMs,workMs,fg\n');
  fg = foreground();
  listener = Interceptor.attach(p, cm);
  timer = setInterval(drain, 250);
  console.log(`[frames] hooked ${p} (CModule) -> ${out}; window ${fg ? 'focused' : 'NOT focused'}`);
}

rpc.exports = {
  init(stage, parameters) { install(parameters.out); }, // throws on a prologue mismatch: nothing gets hooked
  dispose() {
    if (!listener) return;
    clearInterval(timer);
    listener.detach();
    Interceptor.flush();
    // Cheap guard: the render thread may still be inside the CModule hook. The end-of-B crashes (bp-cur, 2026-10-13) were
    // not this: they were two frida-agents unloading in load order (FIFO); tests-be.ts now detaches the profiler first.
    Thread.sleep(0.5);
    drain();
    file.close();
    console.log(`[frames] ${frames} frames on thread ${ST(1)}, ${ST(3)} render calls without present, ` +
      `${ST(2)} calls from other threads, ${lost} lost`);
  },
};
