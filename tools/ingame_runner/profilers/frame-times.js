// Copied from sims3-modernization (MIT, see ../LICENSE-sims3-modernization; https://github.com/rodneypantonial/sims3-modernization).
// Run through attach.py by run.ps1 -Profiler; read-only hooks.
// Frame-time recorder for TS3W.exe (Steam build, SHA-256 10b2423e...0609).
// Hooks FUN_00ec9f00(bool present) (0xEC9F00-0xEC9FDE, __thiscall, RET 4), verified in the disassembly:
//   - BeginScene (0x611620, device vtable +0xA4), scene render, StreamHost flush, then with present != 0 EndScene + Present
//     (0x611760 -> 0x611680, vtable +0xA8 / +0x44), then the inactive-window ~30 FPS sleep at 0xEC9FBA (S3SS Smooth Patch
//     anchor: S3SS jumps out there and back; the prologue stays intact). No tail calls, no other exits.
//   - Callers: the main loop FUN_00eca960 (entry -> __tmainCRTStartup -> 0xECBE30 -> 0xECBD00 -> 0xECA960, so the main/render
//     thread, not the sim thread) calls it with present = 1 once per loop iteration at 0xECAAFC; the window-message path
//     0xECA2A0 also with 1; the callback 0xECA030 (run by 0x9DE140 earlier in the same iteration) with present = 0, which
//     renders without presenting. Only present = 1 calls are frames; present = 0 calls are counted, not recorded.
// Stage timers: entry hooks on the MainLoop callees, each accepted only from its MainLoop call site
// (return address, so the same function called from elsewhere is not counted) or, for the nested ones, only on the main
// thread. Every prologue is checked byte for byte; a mismatch (detoured by another plugin or another session) leaves
// that stage unhooked, logged, and its column `na`. Byte and call-site addresses verified in the Steam 1.67 disassembly.
// Stage times are summed per frame interval [previous presented frame's start, this one's start],
// so they explain frameMs; a stage call that straddles a frame start (a nested present from the window path) is clipped.
// The hook is a CModule: the main thread never takes Frida's JS lock, so a sampler that suspends the sim thread (possibly
// while it holds that lock in stutter-profile.js) cannot stall a frame or deadlock with it. No thread is suspended.
// close() (attach.py --keep) stops the file at the end of the measured window but leaves the hooks in place: unloading this
// CModule while the game runs is where an end-of-profile crash was seen, so the session stays loaded until the game exits.
// Output <out>: CSV per presented frame: epochMs,qpcMs,frameMs,workMs,fg, then the stage columns (STAGE_COLS below).
// qpcMs = QueryPerformanceCounter in ms at the call (the clock stutter-profile.js stamps sim hitches with), frameMs = time
// since the previous frame started, workMs = time inside the call (render + Present, incl. DXVK's frame cap and S3SS's
// limiter), fg = 1 when the game window had focus (unfocused, the game caps itself at ~30 FPS, so B numbers only count with
// fg = 1). Stage ms are inclusive (svcMs contains consumeMs, captureMs contains rcbMs); otherMs = frameMs minus the top-level
// stages (clock tick, mode apply, loading-cap spin, 0xC0FD60 and every unhooked stage). consumes / tryFail / notReady =
// FrameDb_ConsumeAndInterpolate calls, of them skipped before reading a pair (TryEnterCriticalSection(+0x48) failed or
// +0xF8 set) and skipped with no ready frame (sim late); drains / msgs = FrameDb_DrainEvents calls and render messages run.
// Per-service split of svcMs: two probes inside ServiceList_Update 0x59ED20 (the whole function's bytes are
// checked first): 0x59ED3D (esi = list node about to be called through vtable +0x1C) starts a timer, 0x59ED59 (the return
// of `call eax`, also the jump target of a skipped node: told apart by esi) stops it. Only the outermost list walk inside the
// svc stage on the main thread counts (a nested walk is inside its outer service). Per frame the three services with the most
// time: svc1,svc1Ms,svc2,svc2Ms,svc3,svc3Ms (name from SERVICES by vtable, else vt_<hex>; empty when fewer ran; `na` when the
// probes are not installed). Unknown vtables are still timed.
'use strict';

const FRAME = [0xec9f00, '83 ec 20 56 e8 b7 43 74']; // sub esp,20h; push esi; call 0x60E2C0 (graphics device)
const N = 8192; // ring slots: ~2 min at 60 FPS, drained every 250 ms
// [column, function, expected first bytes, MainLoop return address (0 = any caller, main thread only)]; index = C stage id.
// draw / post have no hook: draw = frame entry -> Present entry, post = Present return -> frame return (S3SS Precise limiter
// at 0xEC9FBA and the inactive SleepEx(30)).
const STAGES = [
  ['load', 0xec6c30, '56 8b f1 8b 4e 04', 0xeca9b9],              // LoadState_Update, call at 0xECA9B4
  ['idle', 0x5887a0, '51 ff 15 f8 58 f9 00', 0xeca9ed],           // App_IdleIfNotForeground (Sleep(10)), call at 0xECA9E8
  ['svc', 0x588e00, 'd9 44 24 08 8b 0d f4 e3', 0xecaa55],         // ServiceList_Update thunk -> 0x59ED20, call at 0xECAA50
  ['consume', 0x693780, '83 ec 54 53 57', 0],                     // FrameDb_ConsumeAndInterpolate (from 0x6E3653); another plugin may own it
  ['capture', 0x9de140, '55 8b ec 83 e4 f0', 0xecaadc],           // VideoCapture_RenderFrame, call at 0xECAAD7
  ['rcb', 0xeca030, '57 8b 7c 24 08', 0],                         // Frame_RenderCallback (present = 0), via call [ebp+8] at 0x9DE2E7
  ['begin', 0x6ebb70, '51 56 8b f1 80 be 90 02', 0xecaaee],       // Renderer_BeginFrame, call at 0xECAAE9
  ['draw', 0, '', 0],
  ['present', 0x611760, '6a 00 6a 00 6a 00', 0xec9fba],           // Device_EndSceneAndPresent, call at 0xEC9FB5
  ['post', 0, '', 0],
  ['end', 0x6e8810, '53 56 8b f1 e8 f7 ca 2f', 0xecab0c],         // Renderer_EndFrame (JobManager_Wait), call at 0xECAB07
  ['readpair', 0x593440, '56 57 8b 79 68', 0],                    // FrameDb_GetReadPair: first call after a successful TryEnter
  ['drain', 0x5937b0, '51 53 55 8b 2d 2c 51 f9', 0],              // FrameDb_DrainEvents, returns the message count (eax at 0x5938FA)
];
const NS = 11; // timed stages (0..10); 11, 12 only count
// ServiceList_Update 0x59ED20 (list object [0x11CE3F4], node +8 service, +0x14 flags bit 0, +0x1A enabled):
// all 68 bytes checked, then probes at the call setup and at the call's return
const SVC_WALK = { list: 0x11ce3f4, check: [[0x59ed20, '57 8b f9 8b 47 0c 85 c0 74 36 56 8b 37 3b f7 74 2e 80 7e 1a 00 74 22 f6 46 14 01 74 1c 8b 4e 08 ' +
  'd9 44 24 10 8b 11 8b 42 1c 83 ec 08 d9 5c 24 04 d9 44 24 14 d9 1c 24 ff d0 8b 36 3b f7 75 d2 5e 5f c2 08 00']], start: 0x59ed3d, end: 0x59ed59 };
// main-slot services (vtable +0x1C not the stub 0x861C50), Steam 1.67 [verified vtables and slots; names from the heap/config
// strings at the creator, inferred]: vtable -> name
const SERVICES = {
  0xfcbccc: 'MessageServer', 0xfcbd34: 'Canvas', 0xfd9690: 'Input', 0xfd9810: 'JobManager', 0xfe514c: 'CAS/SimService',
  0xfe6b08: 'CAS/TextureCompositor', 0xfe6f08: 'NPMessageServer', 0xff0070: 'AnimatedTextureManager', 0xff7d84: 'RenderSystem',
  0xffc420: 'SwarmSystem', 0xffcf8c: 'UTFWin', 0xffdf58: 'ResourceChangeMonitor', 0xffeb10: 'ResourceSystem', 0x1007bec: 'ScriptThread',
  0x1015df4: 'ShaderSystem', 0x101c158: 'UI/IME', 0x1029060: 'Audio', 0x102cce0: 'OnlineInformation', 0x10450dc: 'Camera',
  0x1051c78: 'VideoManager', 0x105dee4: 'OnlineEventListener', 0x106be6c: 'ObjectDesigner', 0x107a358: 'World/Service', 0x1088f2c: 'UI/ThumbnailManager',
};
const MAXSV = 64;
const TOP = ['load', 'idle', 'svc', 'capture', 'begin', 'draw', 'present', 'post', 'end']; // not nested in another stage
const STAGE_COLS = 'loadMs,idleMs,svcMs,consumeMs,captureMs,rcbMs,beginMs,drawMs,presentMs,postMs,endMs,otherMs,consumes,tryFail,notReady,drains,msgs,' +
  'svc1,svc1Ms,svc2,svc2Ms,svc3,svc3Ms';
const REC = 96; // sizeof(Rec)

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
#define NS ${NS}
#define MAXSV ${MAXSV}
enum { S_LOAD, S_IDLE, S_SVC, S_CONS, S_CAP, S_RCB, S_BEGIN, S_DRAW, S_PRES, S_POST, S_END, X_READPAIR, X_DRAIN };
typedef struct { gint64 start, end; guint32 st[NS]; guint32 msgs, cnt, tv[3], tt[3], pad; } Rec; /* 96 bytes; tv/tt = top-3 service vtables, ticks */
/* main thread only after main_tid is set; JS reads the first 4 words and writes ret[] */
typedef struct {
  guint32 head, main_tid, other_thread, no_present;
  guint32 ret[16];
  guint32 acc[NS], msgs, cnt;   /* since the last presented frame's start */
  guint32 snap[NS], snap_msgs, snap_cnt;
  guint32 rp, dr, cons_rp, cons_dr;
  gint64 frame_t, pres_in, pres_out;
  /* services: in_svc = inside the svc stage; walk stack (node, vtable, start) for nested list walks */
  guint32 in_svc, sdepth, nsv, pad2;
  guint32 snode[4], svt[4];
  gint64 sstart[4];
  guint32 sv_vt[MAXSV], sv_acc[MAXSV];
  guint32 top_vt[3], top_t[3];
} G;
extern int __attribute__((stdcall)) QueryPerformanceCounter (gint64 * t);
/* in JS-allocated memory: CModule's own data is not writable on Windows */
extern volatile G g;
extern Rec recs[N];

/* cnt bytes: consumes, tryFail, notReady, drains (saturating) */
static void bump (int shift) { if (((g.cnt >> shift) & 0xff) != 0xff) g.cnt += 1u << shift; }

void onEnter (GumInvocationContext * ic) {
  gint64 * s = GUM_IC_GET_INVOCATION_DATA (ic, gint64);
  guint32 tid = gum_invocation_context_get_thread_id (ic);
  int i;
  *s = 0;
  if (g.main_tid == 0) g.main_tid = tid;
  if (tid != g.main_tid) { g.other_thread++; return; }
  if (gum_invocation_context_get_nth_argument (ic, 0) == NULL) { g.no_present++; return; }
  QueryPerformanceCounter (s);
  for (i = 0; i < NS; i++) { g.snap[i] = g.acc[i]; g.acc[i] = 0; }
  g.snap_msgs = g.msgs; g.snap_cnt = g.cnt; g.msgs = 0; g.cnt = 0;
  for (i = 0; i < 3; i++) { g.top_vt[i] = 0; g.top_t[i] = 0; }
  for (i = 0; i < (int) g.nsv; i++) {
    guint32 t = g.sv_acc[i], v = g.sv_vt[i];
    int k;
    g.sv_acc[i] = 0;
    if (t == 0 || t <= g.top_t[2]) continue;
    for (k = 2; k > 0 && t > g.top_t[k - 1]; k--) { g.top_t[k] = g.top_t[k - 1]; g.top_vt[k] = g.top_vt[k - 1]; }
    g.top_t[k] = t; g.top_vt[k] = v;
  }
  g.frame_t = *s; g.pres_in = 0; g.pres_out = 0;
}

void onLeave (GumInvocationContext * ic) {
  gint64 * s = GUM_IC_GET_INVOCATION_DATA (ic, gint64);
  gint64 end;
  guint32 h;
  int i;
  if (*s == 0) return;
  QueryPerformanceCounter (&end);
  h = g.head % N;
  recs[h].start = *s;
  recs[h].end = end;
  for (i = 0; i < NS; i++) recs[h].st[i] = g.snap[i];
  recs[h].msgs = g.snap_msgs;
  recs[h].cnt = g.snap_cnt;
  for (i = 0; i < 3; i++) { recs[h].tv[i] = g.top_vt[i]; recs[h].tt[i] = g.top_t[i]; }
  g.head++; /* x86 keeps store order: the record is written before head moves */
  /* this call's draw / Present / after-Present parts belong to the next frame interval */
  if (g.pres_out > *s && g.pres_in >= *s) {
    g.acc[S_DRAW] += (guint32) (g.pres_in - *s);
    g.acc[S_PRES] += (guint32) (g.pres_out - g.pres_in);
    g.acc[S_POST] += (guint32) (end - g.pres_out);
  } else g.acc[S_DRAW] += (guint32) (end - *s);
}

void stage_enter (GumInvocationContext * ic) {
  guint32 i = GPOINTER_TO_UINT (gum_invocation_context_get_listener_function_data (ic));
  gint64 * s = GUM_IC_GET_INVOCATION_DATA (ic, gint64);
  *s = 0;
  if (g.ret[i] != 0 && GPOINTER_TO_UINT (gum_invocation_context_get_return_address (ic)) != g.ret[i]) return;
  if (g.main_tid == 0 || gum_invocation_context_get_thread_id (ic) != g.main_tid) return;
  if (i == X_READPAIR) { g.rp++; return; }
  if (i == X_DRAIN) { *s = 1; return; }
  QueryPerformanceCounter (s);
  if (i == S_PRES) g.pres_in = *s;
  else if (i == S_SVC) { g.in_svc = 1; g.sdepth = 0; }
  else if (i == S_CONS) { g.cons_rp = g.rp; g.cons_dr = g.dr; }
}

void stage_leave (GumInvocationContext * ic) {
  guint32 i = GPOINTER_TO_UINT (gum_invocation_context_get_listener_function_data (ic));
  gint64 * s = GUM_IC_GET_INVOCATION_DATA (ic, gint64);
  gint64 end;
  if (*s == 0) return;
  if (i == X_DRAIN) { g.dr++; bump (24); g.msgs += GPOINTER_TO_UINT (gum_invocation_context_get_return_value (ic)); return; }
  QueryPerformanceCounter (&end);
  if (i == S_PRES) { g.pres_out = end; return; }
  if (i == S_SVC) g.in_svc = 0;
  g.acc[i] += (guint32) (end - (*s > g.frame_t ? *s : g.frame_t));
  if (i == S_CONS) {
    bump (0);
    if (g.rp == g.cons_rp) bump (8);        /* returned before FrameDb_GetReadPair: TryEnter failed or +0xF8 set */
    else if (g.dr == g.cons_dr) bump (16);  /* pair read, no drain: newest frame not ready */
  }
}

/* probe at the list walk's call setup: esi = node, [esi+8] = the service the game calls next through its vtable +0x1C */
void svc_start (GumInvocationContext * ic) {
  guint32 d, node;
  if (!g.in_svc || gum_invocation_context_get_thread_id (ic) != g.main_tid) return;
  d = g.sdepth++;
  if (d >= 4) return;
  node = ic->cpu_context->esi;
  g.snode[d] = node;
  g.svt[d] = **(guint32 **) (node + 8);
  QueryPerformanceCounter ((gint64 *) &g.sstart[d]);
}

/* probe at the call's return, also reached by a skipped node (jump, no call): only the node on top of the stack ends */
void svc_end (GumInvocationContext * ic) {
  guint32 d, i, vt;
  gint64 end, s;
  if (g.sdepth == 0 || gum_invocation_context_get_thread_id (ic) != g.main_tid) return;
  d = g.sdepth - 1;
  if (d < 4 && g.snode[d] != ic->cpu_context->esi) return;
  g.sdepth = d;
  if (d != 0) return; /* nested walk: its time is inside the outer service */
  QueryPerformanceCounter (&end);
  vt = g.svt[0]; s = g.sstart[0];
  for (i = 0; i < g.nsv && g.sv_vt[i] != vt; i++) ;
  if (i == g.nsv) { if (i == MAXSV) return; g.sv_vt[i] = vt; g.nsv = i + 1; }
  g.sv_acc[i] += (guint32) (end - (s > g.frame_t ? s : g.frame_t));
}
`;

const gmem = Memory.alloc(2048), recs = Memory.alloc(N * REC); // zeroed
const ST = i => gmem.add(4 * i).readU32();
let cm = null, file = null, listener = null, timer = null, read = 0, frames = 0, lost = 0, last = 0, fg = 1, lastFg = 0;
const stageListeners = [], hooked = new Set();
let svcNames = SERVICES;
const svcName = vt => svcNames[vt] ?? `vt_${vt.toString(16)}`;
const r2 = x => Math.round(x * 100) / 100;
const hexAt = (p, n) => Array.from(new Uint8Array(p.readByteArray(n))).map(b => b.toString(16).padStart(2, '0')).join(' ');

function stageCols(rec, frameMs) {
  const v = STAGES.slice(0, NS).map(([name], i) => {
    if (name === 'draw' ? false : name === 'post' ? !hooked.has('present') : !hooked.has(name)) return NaN;
    return rec.add(16 + 4 * i).readU32() / ticksPerMs;
  });
  const top = TOP.reduce((s, name) => { const x = v[STAGES.findIndex(st => st[0] === name)]; return Number.isNaN(x) ? s : s + x; }, 0);
  const cnt = rec.add(64).readU32();
  const cons = hooked.has('consume');
  const svc = [0, 1, 2].map(k => {
    if (!hooked.has('services')) return 'na,na';
    const vt = rec.add(68 + 4 * k).readU32();
    return vt ? `${svcName(vt)},${r2(rec.add(80 + 4 * k).readU32() / ticksPerMs)}` : ',';
  });
  return [...v.map(x => Number.isNaN(x) ? 'na' : r2(x)), r2(Math.max(0, frameMs - top)),
    cons ? cnt & 0xff : 'na', cons && hooked.has('readpair') ? (cnt >>> 8) & 0xff : 'na', cons && hooked.has('readpair') && hooked.has('drain') ? (cnt >>> 16) & 0xff : 'na',
    hooked.has('drain') ? cnt >>> 24 : 'na', hooked.has('drain') ? rec.add(60).readU32() : 'na', ...svc].join(',');
}

function drain() {
  const head = ST(0);
  if (head - read > N) { lost += head - read - N; read = head - N; last = 0; } // overwritten: restart the frame chain
  const out = [];
  for (; read !== head; read++) {
    const rec = recs.add((read % N) * REC);
    const t = Number(rec.readS64()) / ticksPerMs, end = Number(rec.add(8).readS64()) / ticksPerMs;
    if (last) { out.push(`${Math.round(epoch0 + t - q0)},${r2(t)},${r2(t - last)},${r2(end - t)},${fg},${stageCols(rec, t - last)}\n`); frames++; }
    last = t;
  }
  if (out.length) { file.write(out.join('')); file.flush(); }
  const n = nowMs();
  if (n - lastFg >= 1000) { fg = foreground(); lastFg = n; }
}

// addrs (dummy test only, never for the game): { frame: [addr, bytes], <stage>: [addr, bytes, ret] } replaces the table
function install(out, addrs) {
  if (!addrs && !Process.getModuleByName('TS3W.exe').base.equals(ptr(0x400000))) throw new Error('TS3W.exe not at 0x400000');
  const frame = addrs?.frame ?? FRAME;
  const p = ptr(frame[0]);
  const got = hexAt(p, 8);
  if (!got.startsWith(frame[1])) throw new Error(`frame fn @ ${p}: bytes ${got}, expected ${frame[1]} (hooked by another session, detoured, or a different build)`);
  cm = new CModule(C, { QueryPerformanceCounter: QPC, g: gmem, recs });
  file = new File(out, 'w');
  file.write(`epochMs,qpcMs,frameMs,workMs,fg,${STAGE_COLS}\n`);
  fg = foreground();
  // stage hooks first: a frame without its stages would read as all 'other'
  const refused = [];
  STAGES.forEach(([name, fn, bytes, ret], i) => {
    const [a, b, r] = addrs ? (addrs[name] ?? [0]) : [fn, bytes, ret];
    if (!a) return;
    const sp = ptr(a), have = hexAt(sp, 8);
    if (!have.startsWith(b)) { refused.push(`${name} @ ${sp}: bytes ${have}, expected ${b}`); return; }
    gmem.add(16 + 4 * i).writeU32(r);
    stageListeners.push(Interceptor.attach(sp, { onEnter: cm.stage_enter, onLeave: cm.stage_leave }, ptr(i)));
    hooked.add(name);
  });
  // per-service split: needs the svc stage (in_svc) and the unchanged list walk; the dummy passes its own walk
  const walk = addrs ? addrs.walk : SVC_WALK;
  if (walk && hooked.has('svc')) {
    const bad = walk.check.filter(([a, b]) => hexAt(ptr(a), b.split(' ').length) !== b);
    if (bad.length) refused.push(`services @ ${bad.map(([a]) => ptr(a)).join(', ')}: ServiceList_Update bytes changed (${hexAt(ptr(bad[0][0]), 8)} ...)`);
    else {
      if (walk.names) svcNames = { ...SERVICES, ...walk.names };
      stageListeners.push(Interceptor.attach(ptr(walk.start), cm.svc_start), Interceptor.attach(ptr(walk.end), cm.svc_end));
      hooked.add('services');
      try { // the list as it is now (order = call order); read only
        const L = ptr(walk.list).readPointer(), on = [], off = [];
        for (let n = L.readPointer(), k = 0; !n.equals(L) && k < 200; n = n.readPointer(), k++) {
          const vt = n.add(8).readPointer().readU32();
          ((n.add(0x14).readU32() & 1) && n.add(0x1a).readU8() ? on : off).push(svcName(vt));
        }
        console.log(`[frames] services (+0x1C, call order): ${on.join(' ')}; not called: ${off.length}`);
      } catch (e) { console.log(`[frames] service list not readable: ${e.message}`); }
    }
  }
  listener = Interceptor.attach(p, cm);
  timer = setInterval(drain, 250);
  console.log(`[frames] hooked ${p} (CModule) -> ${out}; window ${fg ? 'focused' : 'NOT focused'}; stages ${[...hooked].join(' ')}`);
  for (const x of refused) console.log(`[frames] stage NOT hooked (prologue changed: another plugin's detour or another session): ${x}`);
  // crash triage: name where the CModule's code lives so a fault eip outside every module can be matched
  const syms = Object.entries(cm).filter(([, v]) => v instanceof NativePointer).map(([k, v]) => `${k}=${v}`).join(' ');
  const frida = Process.enumerateModules().filter(m => /frida/i.test(m.name)).map(m => `${m.name} ${m.base}+0x${m.size.toString(16)}`).join(', ');
  console.log(`[frames] cmodule ${syms}; frida modules: ${frida || 'none listed'}`);
}

rpc.exports = {
  init(stage, parameters) { install(parameters.out, parameters.addrs); }, // throws on a frame prologue mismatch: nothing gets hooked
  // end of the measured window: last rows written, file closed; the hooks stay (the ring keeps wrapping, nothing is read)
  close() { closeFile(); },
  dispose() {
    if (!listener) return;
    for (const l of stageListeners) l.detach();
    listener.detach();
    Interceptor.flush();
    // Cheap guard: the render thread may still be inside the CModule hook. Two Frida agents unloading together crashed the
    // game in earlier runs, so attach.py gives the frame recorder its own session.
    Thread.sleep(0.5);
    closeFile();
  },
};

function closeFile() {
  if (!file) return;
  clearInterval(timer);
  drain();
  file.close();
  file = null;
  console.log(`[frames] ${frames} frames on thread ${ST(1)}, ${ST(3)} render calls without present, ` +
    `${ST(2)} calls from other threads, ${lost} lost`);
}
