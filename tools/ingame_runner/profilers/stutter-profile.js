// Copied from sims3-modernization (MIT, see ../LICENSE-sims3-modernization; https://github.com/rodneypantonial/sims3-modernization).
// Run through attach.py by run.ps1 -Profiler; read-only hooks.
// Sim-thread stutter profiler for TS3W.exe (Steam build, SHA-256 10b2423e...0609).
// Times what the simulation loop (MonoScriptHost::Simulate, 0xD81840) spends per iteration:
// script tick, route searches, GC stop-the-world marking, finalizers, catch-up steps, animation (two passes), locomotion,
// render publish, and the four scheduler seams
// (ScriptTick 0xD80BC0, TaskObjects 0xD7FC60, OneShotObjects 0xD74800, ThreadedObjects 0xD7F8B0) as
// separate buckets, so each hitch is split by stage. S3PP's SimulatorStageProfiler detours the same prologues: turn it
// off in Sims3Performance.ini while profiling (the byte check refuses to hook a detoured prologue).
//
// Run (game already in Live mode): run.ps1 -Profiler Sim, or frida -n TS3W.exe -l stutter-profile.js -P '{"out":"<prefix>","hitchMs":50}'
// Output: <out>-seconds.csv (one row per second), <out>-hitches.jsonl (one line per slow iteration, `n` = iteration number)
// and <out>-freezes.jsonl (one line per sim-to-render publish gap >= hitchMs: what the player sees as frozen Sims).
// -P stages:false keeps only the iteration boundary, catch-up steps and publish gaps (no stage, GC or route hooks): hitch
// lines keep wallMs/steps/n and freezes.jsonl stays complete, but carry no stage split.
// Default <out> is ts3prof-<timestamp> in the game's working directory (Game\Bin).

'use strict';

const HOOKS = {
  // name: [address, expected first bytes]
  simulate: [0xd81840, '81 ec 88 00 00 00 53 55'],
  scriptTick: [0xd80bc0, '55 8b ec 83 e4 f8 81 ec'],
  // the other scheduler seams, siblings of scriptTick inside Simulate; each runs at most once per iteration
  taskObjects: [0xd7fc60, '55 8b ec 83 e4 f8 83 ec'],
  oneShot: [0xd74800, '83 ec 10 53 55 56 57 8b'],
  threaded: [0xd7f8b0, '55 8b ec 83 e4 f8 81 ec'],
  catchUpSteps: [0xd70730, '83 ec 1c 53 55 56 57 89'],
  gcTryToCollect: [0xe4a050, '83 3d 94 30 1f 01 00 74'],
  gcStoppedMark: [0xe49980, '51 e8 1a 78 00 00 a1 60'],
  gcInvokeFinalizers: [0xe697f0, 'e8 eb 1b fe ff 85 c0 74'],
  routeOuter: [0x82c6e0, '81 ec d8 00 00 00 53 56'], // lock + setup + search + unlock, returns success in AL
  routeSearch: [0x81b2a0, '55 8b ec 83 e4 f0 81 ec'], // the A* search itself
  // script entry points (the internal calls the scripts reach routing through)
  Route_Plan: [0x765160, '0f 57 c0 8b 0d c4 40 1d'],
  Route_Replan: [0x765490, '8b 0d c4 40 1d 01 83 ec'],
  Route_PlanFromPoint: [0x765240, '55 8b ec 83 e4 f0 0f 57'],
  Route_ReplanFromPosition: [0x765550, '55 8b ec 83 e4 f0 8b 0d'],
  Route_IsPointRoutable: [0x7538c0, '55 8b ec 83 e4 f0 8b 0d'],
  // the rest of Simulate's untimed share: two animation passes (call 0xD81A70 -> SACS driver,
  // time step, rig pose; call 0xD81C73 -> time step, rig pose 0x940430), locomotion, and the sim-to-render publish
  animSacs: [0x571ad0, '53 56 8b f1 83 4e 28 08'],
  animPose: [0x571b90, '83 ec 08 55 57 8b f9 8b'],
  locomotion: [0x83ced0, '55 8b ec 83 e4 f0 81 ec'],
  publish: [0x593590, '53 56 57 8b f1 8d 7e 48'], // triple-buffer sync (EnterCriticalSection); may nest in other stages
};
// sim-thread ms per stage, summed like the scheduler seams
const EXTRA_STAGES = ['animSacs', 'animPose', 'locomotion', 'publish'];
const ROUTE_ENTRIES = ['Route_Plan', 'Route_Replan', 'Route_PlanFromPoint', 'Route_ReplanFromPosition', 'Route_IsPointRoutable'];

let cfg = { out: null, hitchMs: 50 };

// --- high-resolution clock (QPC), in ms ---
const k32 = Process.getModuleByName('kernel32.dll');
const qpc = new NativeFunction(k32.getExportByName('QueryPerformanceCounter'), 'int', ['pointer']);
const qpf = new NativeFunction(k32.getExportByName('QueryPerformanceFrequency'), 'int', ['pointer']);
const qbuf = Memory.alloc(8);
qpf(qbuf);
const ticksPerMs = Number(qbuf.readS64()) / 1000;
function now() { qpc(qbuf); return Number(qbuf.readS64()) / ticksPerMs; }

// --- per-iteration and per-second accumulators (sim thread only, except routeOtherThreads) ---
function blankIter() {
  return { script: 0, gcMark: 0, gcMarks: 0, gcAborted: 0, gcForced: 0, gcForcedMs: 0, fin: 0, finRun: 0,
           steps: 0, route: 0, routeSearch: 0, routes: 0, routeFails: 0, byEntry: {},
           taskObjects: 0, oneShot: 0, threaded: 0, animSacs: 0, animPose: 0, locomotion: 0, publish: 0,
           // ts3native [HitchAttr]: sim waits for the route job, ms by class, count, longest and its key
           rtLock: 0, rtOtherLock: 0, rtJob: 0, rtPlanWait: 0, rtWaits: 0, rtMaxMs: 0, rtMaxKey: '' };
}
function blankSecond() {
  return { iters: 0, maxIterMs: 0, hitches: 0, script: 0, gcMark: 0, gcMarks: 0, gcAborted: 0, gcForced: 0,
           gcForcedMs: 0, maxGcMarkMs: 0, fin: 0, finRun: 0, maxSteps: 0, route: 0, routeSearch: 0, routes: 0,
           routeFails: 0, maxRouteMs: 0, routeOtherThreads: 0,
           taskObjects: 0, oneShot: 0, threaded: 0, maxTaskObjectsMs: 0,
           animSacs: 0, animPose: 0, locomotion: 0, publish: 0,
           // ts3native [LotStream]; 0 when the plugin or the section is off
           lotAdds: 0, lotAddMs: 0, lotAddMaxMs: 0, lotAddObjs: 0, lotRemoves: 0, lotRemoveMs: 0, pumpMaxMs: 0, lotScoreMs: 0,
           lotSetActive: 0,
           // P-clock: 1 when Simulate's paused copy was set at the start of any iteration of this second; the
           // step accumulator host+0xC14 at the start of the last iteration (left over from the previous one's steps;
           // -1 = P-clock off or no iteration this second; NaN/Infinity = the accumulator held no number)
           simPaused: 0, stepAcc: -1,
           // ts3native [HitchAttr], ms per second; 0 when the plugin or the section is off
           rtLock: 0, rtOtherLock: 0, rtJob: 0, rtPlanWait: 0, rtWaits: 0, // new columns go last: readers use the header
           // FrameDb_PublishFrame 0x593600 calls on the sim thread: 0 or 1 per iteration; longest publish gap ending here
           publishes: 0, maxPubGapMs: 0 };
}

// Sim-to-render publish: Simulate calls 0x76C8A0 -> FrameDb_PublishFrame once per iteration at most (0xD81DF5 after the
// steps, 0xD81E9E when paused; an unpaused iteration with 0 steps publishes nothing). The renderer interpolates between the
// two newest published frames, so the time between two publishes is how long the Sims stand still on screen. Not in HOOKS:
// a mismatch only turns the freeze log off instead of stopping the whole profiler.
const PUBLISH_FRAME = [0x593600, '56 57 8b f1 8d 7e 48 57'];
let iterNo = 0, lastPub = 0, prevPubGap = 0, itersSincePub = 0;

// P-clock: Simulate's step branch reads its paused copy through `mov ecx, edi; call 0x8A6D10` at 0xD81B38
// (0x8A6D10 = `mov al, [ecx+0x10]; ret`, edi = host+0xA64) after `fstp [esi+0xC14]`. Never hook inside Simulate: it
// never returns, and a Frida hook there keeps the script from unloading, so the
// profiler outlives its -t window. Instead the existing
// GC_try_to_collect hook reads the same fields once per iteration: Simulate keeps host in esi from 0xD81849 on and calls
// it directly at 0xD819AA, so at that entry esi = host, paused copy = host+0xA74, accumulator = host+0xC14.
const CLOCK_CHECKS = [[0xd81849, '8b f1'], [0xd819aa, 'e8 a1 86 0c 00'], [0xd81a46, '8d be 64 0a 00 00'],
                      [0xd81b32, 'd9 9e 14 0c 00 00 e8 d3 51 b2 ff'], [0x8a6d10, '8a 41 10 c3']];
const CLOCK_RET = ptr(0xd819af);
let clockOn = false;

// ts3native [LotStream] counters, counter layout version 19 byte offsets.
// Totals become per-second deltas; the *MaxUs fields hold the max since the last reset, so they are read and zeroed here.
const LS_OFF = { lotAdds: 0x2cc, lotAddUs: 0x2d0, lotAddMaxUs: 0x2d4, lotAddObjs: 0x2d8, lotRemoves: 0x2e0, lotRemoveUs: 0x2e4,
                 pumpMaxUs: 0x2f4, lotScoreUs: 0x304, lotSetOn: 0x310, lotSetOff: 0x314 };
let lsCounters = null, lsPrev = null; // undefined once known absent
function lotStreamSecond() {
  if (lsCounters === undefined) return;
  if (lsCounters === null) {
    const m = Process.findModuleByName('ts3native.asi');
    const c = m === null ? null : m.findExportByName('ts3native_counters');
    lsCounters = c !== null && c.readS32() >= 19 && (c.add(0x2c4).readS32() & 31) === 31 ? c : undefined; // lsReady: all five hooks
    console.log(`[ts3prof] ts3native lot stream counters ${lsCounters ? 'read every second' : 'not active (columns stay 0)'}`);
    if (lsCounters === undefined) return;
  }
  const cur = Object.fromEntries(Object.entries(LS_OFF).map(([k, o]) => [k, lsCounters.add(o).readS32()]));
  for (const k of ['lotAddMaxUs', 'pumpMaxUs']) lsCounters.add(LS_OFF[k]).writeS32(0);
  if (lsPrev) {
    const d = k => cur[k] - lsPrev[k];
    Object.assign(sec, { lotAdds: d('lotAdds'), lotAddMs: d('lotAddUs') / 1000, lotAddMaxMs: cur.lotAddMaxUs / 1000, lotAddObjs: d('lotAddObjs'),
      lotRemoves: d('lotRemoves'), lotRemoveMs: d('lotRemoveUs') / 1000, pumpMaxMs: cur.pumpMaxUs / 1000, lotScoreMs: d('lotScoreUs') / 1000,
      lotSetActive: d('lotSetOn') + d('lotSetOff') });
  } // the first read only sets the baseline (its maxima date from the game start)
  lsPrev = cur;
}
// ts3native [HitchAttr] frame block `ts3native_hitchattr` (hitchattr.cpp HaFrame): version, enabled, us[4], n, maxUs, maxKey.
// Read and zeroed at each iteration boundary on the sim thread, the only thread that writes it, so no clock fit is needed.
let haBlock = null; // undefined once known absent
const HA_ZERO = new ArrayBuffer(28);
function hitchAttrFrame() {
  if (haBlock === undefined) return;
  if (haBlock === null) {
    const m = Process.findModuleByName('ts3native.asi');
    const e = m === null ? null : m.findExportByName('ts3native_hitchattr');
    haBlock = e !== null && e.readS32() >= 1 && e.add(4).readS32() === 1 ? e : undefined;
    console.log(`[ts3prof] ts3native hitch attribution ${haBlock ? 'read every iteration' : 'not active (rt* fields stay 0)'}`);
    if (haBlock === undefined) return;
  }
  const v = new Int32Array(haBlock.add(8).readByteArray(28));
  haBlock.add(8).writeByteArray(HA_ZERO);
  iter.rtLock += v[0] / 1000; iter.rtOtherLock += v[1] / 1000; iter.rtJob += v[2] / 1000; iter.rtPlanWait += v[3] / 1000;
  iter.rtWaits += v[4];
  if (v[5] / 1000 > iter.rtMaxMs) { iter.rtMaxMs = v[5] / 1000; iter.rtMaxKey = '0x' + (v[6] >>> 0).toString(16).padStart(8, '0'); }
}
let simThread = null;
let iter = blankIter();
let iterStart = 0;
let sec = blankSecond();
let secStart = 0;
let inTryToCollect = false;
const entryStack = []; // script entry point for the current route call (sim thread)

let csv = null, jsonl = null, freezes = null;

function openOutputs() {
  const base = cfg.out || `ts3prof-${new Date().toISOString().replace(/[:.]/g, '-')}`;
  csv = new File(`${base}-seconds.csv`, 'w');
  jsonl = new File(`${base}-hitches.jsonl`, 'w');
  freezes = new File(`${base}-freezes.jsonl`, 'w');
  csv.write(Object.keys(blankSecond()).concat(['t']).join(',') + '\n');
  csv.flush();
  console.log(`[ts3prof] writing ${base}-seconds.csv and ${base}-hitches.jsonl (hitch >= ${cfg.hitchMs} ms)`);
}

function round(x) { return Math.round(x * 100) / 100; }

function endIteration(t) {
  hitchAttrFrame();
  if (iterStart === 0) { iterStart = t; secStart = t; iter = blankIter(); return; }
  const wall = t - iterStart;
  sec.iters++;
  sec.maxIterMs = Math.max(sec.maxIterMs, wall);
  for (const k of ['script', 'gcMark', 'gcMarks', 'gcAborted', 'gcForced', 'gcForcedMs', 'fin', 'finRun',
                   'route', 'routeSearch', 'routes', 'routeFails', 'taskObjects', 'oneShot', 'threaded', ...EXTRA_STAGES,
                   'rtLock', 'rtOtherLock', 'rtJob', 'rtPlanWait', 'rtWaits']) sec[k] += iter[k];
  sec.maxSteps = Math.max(sec.maxSteps, iter.steps);
  if (wall >= cfg.hitchMs) {
    sec.hitches++;
    const rec = { t: round(t), wallMs: round(wall), n: iterNo };
    for (const [k, v] of Object.entries(iter)) rec[k] = typeof v === 'number' ? round(v) : v;
    jsonl.write(JSON.stringify(rec) + '\n');
    jsonl.flush();
  }
  if (t - secStart >= 1000) flushSecond(t);
  iter = blankIter();
  iterStart = t;
  iterNo++; itersSincePub++;
}

// on the sim thread at FrameDb_PublishFrame's return; n = the iteration it closes, iters = iteration boundaries inside the
// gap (n - iters .. n are the iterations it overlaps; their hitches.jsonl lines carry the same n)
function onPublish(t) {
  if (iterStart === 0) return; // before the first iteration boundary: no n yet
  if (lastPub) {
    const gap = t - lastPub;
    sec.publishes++;
    sec.maxPubGapMs = Math.max(sec.maxPubGapMs, gap);
    if (gap >= cfg.hitchMs) {
      freezes.write(JSON.stringify({ t0: round(lastPub), t1: round(t), gapMs: round(gap), prevGapMs: round(prevPubGap), n: iterNo, iters: itersSincePub }) + '\n');
      freezes.flush();
    }
    prevPubGap = gap;
  }
  lastPub = t;
  itersSincePub = 0;
}

function flushSecond(t) {
  lotStreamSecond();
  const row = Object.values(sec).map(v => round(v)).concat([round(t)]);
  csv.write(row.join(',') + '\n');
  csv.flush();
  if (sec.hitches > 0 || sec.maxIterMs > cfg.hitchMs)
    console.log(`[ts3prof] ${sec.iters} it/s, max ${round(sec.maxIterMs)} ms, hitches ${sec.hitches}, ` +
      `routes ${sec.routes} (${sec.routeFails} failed, ${round(sec.route)} ms), gc ${round(sec.gcMark)} ms ` +
      `(${sec.gcAborted} aborted, ${sec.gcForced} forced ${round(sec.gcForcedMs)} ms), fin ${round(sec.fin)} ms, ` +
      `stages tick ${round(sec.script)} task ${round(sec.taskObjects)} (max ${round(sec.maxTaskObjectsMs)}) ` +
      `oneShot ${round(sec.oneShot)} threaded ${round(sec.threaded)} anim ${round(sec.animSacs)}+${round(sec.animPose)} ` +
      `locomotion ${round(sec.locomotion)} publish ${round(sec.publish)} ms`);
  sec = blankSecond();
  secStart = t;
}

// --- hooks ---
function addr(name) {
  const [a, expect] = HOOKS[name];
  const p = ptr(a);
  const got = Array.from(new Uint8Array(p.readByteArray(8))).map(b => b.toString(16).padStart(2, '0')).join(' ');
  if (got.startsWith('e9')) { // detoured by a known plugin: time its wrapper instead
    const target = p.add(5).add(p.add(1).readS32());
    // ts3native's route detour, or S3PP's SimulatorStageProfiler (needed by its MonoTypeEHashProbe, so it stays on): both wrap
    // the original with the same arguments and result, so timing the wrapper times the stage
    if (['ts3native.asi', 'Sims3Performance.asi'].includes(Process.findModuleByAddress(target)?.name)) return target;
  }
  if (got !== expect) throw new Error(`${name} @ ${p}: bytes ${got}, expected ${expect}. ` +
    (got.startsWith('e9') ? 'Detoured by another plugin (S3PP SimulatorStageProfiler? turn it off).' : 'Different TS3W.exe build?'));
  return p;
}

function timed(name, onDone, onEnterExtra) {
  Interceptor.attach(addr(name), {
    onEnter(args) { this.t0 = now(); if (onEnterExtra) onEnterExtra.call(this, args); },
    onLeave(retval) { onDone.call(this, now() - this.t0, retval); },
  });
}

function install() {
  const mod = Process.getModuleByName('TS3W.exe');
  if (!mod.base.equals(ptr(0x400000))) throw new Error(`TS3W.exe loaded at ${mod.base}, expected 0x400000`);
  for (const name of Object.keys(HOOKS)) addr(name); // check every prologue before the first attach: all or nothing
  // simulate is only verified, never hooked: it loops forever

  // One GC_try_to_collect call per Simulate iteration: use its entry as the iteration boundary.
  Interceptor.attach(addr('gcTryToCollect'), {
    onEnter() {
      if (simThread === null) simThread = Process.getCurrentThreadId();
      if (Process.getCurrentThreadId() !== simThread) return;
      endIteration(now());
      inTryToCollect = true;
      if (clockOn && this.returnAddress.equals(CLOCK_RET)) { // esi = Simulate's host (callee-saved, still the caller's here)
        const host = this.context.esi;
        if (host.add(0xa74).readU8() !== 0) sec.simPaused = 1;
        sec.stepAcc = host.add(0xc14).readFloat();
      }
    },
    onLeave() { inTryToCollect = false; },
  });

  Interceptor.attach(addr('catchUpSteps'), {
    onEnter(args) { if (Process.getCurrentThreadId() === simThread) iter.steps += args[0].toInt32(); },
  });
  if (cfg.stages !== false) installStages();
  else console.log('[ts3prof] stage hooks off (stages: false): iterations, steps and publish gaps only');

  try { // after the all-or-nothing check: a mismatch here costs only the freeze log
    HOOKS.publishFrame = PUBLISH_FRAME;
    Interceptor.attach(addr('publishFrame'), { onLeave() { if (Process.getCurrentThreadId() === simThread) onPublish(now()); } });
    console.log('[ts3prof] publish gaps: FrameDb_PublishFrame 0x593600 timed on the sim thread (freezes.jsonl)');
  } catch (e) {
    console.log(`[ts3prof] publish gaps off (freezes.jsonl stays empty): ${e.message}`);
  }
  installClock();
  console.log('[ts3prof] hooks installed; the sim thread is detected on the first GC_try_to_collect');
}

function installStages() {
  timed('gcStoppedMark', function (ms, retval) {
    if (Process.getCurrentThreadId() !== simThread) return;
    iter.gcMark += ms; iter.gcMarks++;
    sec.maxGcMarkMs = Math.max(sec.maxGcMarkMs, ms);
    if (retval.toInt32() === 0) iter.gcAborted++; // stop function fired: mark work thrown away
    if (!this.perFrame) { iter.gcForced++; iter.gcForcedMs += ms; } // collection triggered by an allocation
  }, function () { this.perFrame = inTryToCollect; });

  timed('gcInvokeFinalizers', function (ms, retval) {
    if (Process.getCurrentThreadId() !== simThread) return;
    iter.fin += ms; iter.finRun += retval.toInt32();
  });

  timed('scriptTick', function (ms) {
    if (Process.getCurrentThreadId() === simThread) iter.script += ms;
  });
  for (const stage of ['taskObjects', 'oneShot', 'threaded']) {
    timed(stage, function (ms) {
      if (Process.getCurrentThreadId() !== simThread) return;
      iter[stage] += ms;
      if (stage === 'taskObjects') sec.maxTaskObjectsMs = Math.max(sec.maxTaskObjectsMs, ms);
    });
  }

  for (const stage of EXTRA_STAGES) {
    timed(stage, function (ms) {
      if (Process.getCurrentThreadId() === simThread) iter[stage] += ms;
    });
  }

  for (const name of ROUTE_ENTRIES) {
    Interceptor.attach(addr(name), {
      onEnter() { entryStack.push(name); },
      onLeave() { entryStack.pop(); },
    });
  }

  timed('routeSearch', function (ms) {
    if (Process.getCurrentThreadId() === simThread) iter.routeSearch += ms;
  });

  timed('routeOuter', function (ms, retval) {
    if (Process.getCurrentThreadId() !== simThread) { sec.routeOtherThreads++; return; }
    const ok = (retval.toInt32() & 0xff) !== 0;
    const entry = entryStack.length ? entryStack[entryStack.length - 1] : 'other';
    iter.route += ms; iter.routes++;
    if (!ok) iter.routeFails++;
    sec.maxRouteMs = Math.max(sec.maxRouteMs, ms);
    const e = iter.byEntry[entry] || (iter.byEntry[entry] = { n: 0, fail: 0, ms: 0 });
    e.n++; if (!ok) e.fail++; e.ms = round(e.ms + ms);
  });
}

// Observe only, no hook of its own: a mismatch leaves simPaused 0 and stepAcc -1 instead of stopping the whole profiler.
function installClock() {
  const hex = (p, n) => Array.from(new Uint8Array(p.readByteArray(n))).map(b => b.toString(16).padStart(2, '0')).join(' ');
  const bad = CLOCK_CHECKS.map(([a, expect]) => [a, hex(ptr(a), expect.split(' ').length), expect]).filter(([, got, expect]) => got !== expect);
  if (bad.length) {
    console.log(`[ts3prof] P-clock off: ${bad.map(([a, got, expect]) => `0x${a.toString(16)} ${got} (expected ${expect})`).join('; ')}`);
    return;
  }
  clockOn = true;
  console.log('[ts3prof] P-clock: simPaused and stepAcc read from Simulate\'s host (esi) at each GC_try_to_collect call from 0xD819AA');
}

let started = false;
function start(parameters) {
  if (started) return;
  started = true;
  cfg = Object.assign(cfg, parameters || {});
  openOutputs();
  try {
    install();
  } catch (e) {
    console.error(`[ts3prof] NOT PROFILING: ${e.message}`); // a wrong build must never be hooked half-way
  }
}
setTimeout(() => start({}), 1000); // fallback when the host does not call init

rpc.exports = {
  init(stage, parameters) { start(parameters); },
  dispose() {
    if (csv) { flushSecond(now()); csv.close(); jsonl.close(); freezes.close(); }
  },
};
