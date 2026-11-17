// Read-only analysis of Shaders_Win32.precomp. Usage: node precomp.cjs <precomp> <mode> [arg]
const fs = require('fs');
const b = fs.readFileSync(process.argv[2]);
const mode = process.argv[3] || 'summary', arg = process.argv[4] || '';
const text = b.toString('latin1');
const k = b.lastIndexOf('KNM '), names = new Map();
let p = k + 16;
for (let i = 0; i < b.readUInt32LE(k + 12); i++) {
  const h = b.readUInt32LE(p), n = b.readUInt32LE(p + 8);
  names.set(h, b.toString('utf8', p + 12, p + 12 + n)); p += 12 + n;
}
const blobs = [];
for (const m of text.matchAll(/VSHD|PSHD/g)) {
  if (m.index > 36100000) break;
  const at = m.index, size = b.readUInt32LE(at + 4);
  let begin = -1;
  for (let j = at + 8; j <= at + 12; j++) {
    const v = b.readUInt32LE(j);
    if (((v >>> 16) === 0xfffe || (v >>> 16) === 0xffff) && ((v >>> 8) & 255) >= 1 && ((v >>> 8) & 255) <= 3) { begin = j; break; }
  }
  if (begin < 0 || size > 100000) continue;
  const end = at + 8 + size;
  blobs.push({ at, code: b.subarray(begin, end), version: b.readUInt32LE(begin) });
}
// instruction slot cost (SM2/3 rules, approximate for macro ops)
const COST = { 0x14: 4, 0x15: 3, 0x16: 4, 0x17: 3, 0x18: 2, 0x24: 3, 0x20: 3, 0x25: 8, 0x12: 2, 0x21: 2, 0x22: 2 };
function stat(code) {
  const ver = code.readUInt32LE(0), isPs = (ver >>> 16) === 0xffff, major = (ver >>> 8) & 255;
  let i = 4, slots = 0, tex = 0, maxC = -1, maxR = -1; const samp = new Set(), dclS = new Set(), inputs = new Set(), defs = new Set(), cons = new Set();
  let flow = false, tc = new Set();
  while (i < code.length) {
    const t = code.readUInt32LE(i), op = t & 0xffff;
    if (op === 0xffff) break;
    if (op === 0xfffe) { i += 4 + ((t >>> 16) & 0x7fff) * 4; continue; }
    const len = major >= 2 ? (t >>> 24) & 15 : -1;
    if (len < 0) return null;
    const params = [];
    for (let j = 0; j < len; j++) params.push(code.readUInt32LE(i + 4 + j * 4));
    const rtype = v => ((v >>> 28) & 7) | ((v >>> 8) & 0x18), rnum = v => v & 0x7ff;
    if (op === 0x1f) { // dcl
      const r = params[1], ty = rtype(r);
      if (ty === 10) dclS.add(rnum(r));
      else if (isPs && (ty === 1 || ty === 3)) { inputs.add(ty + ':' + rnum(r)); const u = params[0] & 31, ui = (params[0] >>> 16) & 15; if (u === 5) tc.add(ui); }
      else if (!isPs && ty === 6) { const u = params[0] & 31, ui = (params[0] >>> 16) & 15; if (u === 5) tc.add(ui); inputs.add('o' + rnum(r)); }
    } else if (op === 0x51 || op === 0x30 || op === 0x2f) { defs.add(rnum(params[0])); }
    else {
      slots += COST[op] || 1;
      if (op === 0x42 || op === 0x5d || op === 0x5f) tex++;
      if ([0x1b, 0x26, 0x28, 0x29, 0x2d, 0x19].includes(op)) flow = true;
      params.forEach((v, j) => {
        const ty = rtype(v);
        if (ty === 2) { maxC = Math.max(maxC, rnum(v)); cons.add(rnum(v)); }
        if (ty === 0) maxR = Math.max(maxR, rnum(v));
        if (ty === 10) samp.add(rnum(v));
      });
    }
    i += 4 + len * 4;
  }
  const rt = [...cons].filter(c => !defs.has(c));
  return { isPs, major, minor: ver & 255, bytes: code.length, slots, tex, samp, nS: Math.max(samp.size, dclS.size), maxS: Math.max(-1, ...samp, ...dclS), maxC, nC: cons.size, nRt: rt.length, maxRt: Math.max(-1, ...rt), temps: maxR + 1, nIn: inputs.size, flow, tc };
}
const techs = [...text.matchAll(/TECH/g)].filter(m => m.index > 36000000 && m.index < k && names.has(b.readUInt32LE(m.index + 4)));
const fam = new Map();
for (let i = 0; i < techs.length; i++) {
  const at = techs[i].index, end = i + 1 < techs.length ? techs[i + 1].index : k;
  const name = names.get(b.readUInt32LE(at + 4));
  if (!fam.has(name)) fam.set(name, { ps: new Map(), vs: new Map(), refs: new Map(), passes: 0, texAt: new Map() });
  const f = fam.get(name);
  const ps = [...text.slice(at + 12, end).matchAll(/PASS/g)].map(m => at + 12 + m.index);
  ps.forEach((off, j) => {
    const pend = j + 1 < ps.length ? ps[j + 1] : end;
    f.passes++;
    const vi = b.readUInt32LE(off + 12) - 1, pi = b.readUInt32LE(off + 16) - 1;
    if (vi >= 0 && blobs[vi]) f.vs.set(vi, blobs[vi]);
    if (pi >= 0 && blobs[pi]) f.ps.set(pi, blobs[pi]);
    for (let q = off + 20; q + 4 <= pend; q += 4) { const n = names.get(b.readUInt32LE(q)); if (n) f.refs.set(n, (f.refs.get(n) || 0) + 1); }
    if (mode === 'pass' && name === arg && j === 0 && !f.dumped) {
      f.dumped = 1; const out = [];
      for (let q = off; q + 4 <= Math.min(pend, off + 1400); q += 4) { const v = b.readUInt32LE(q); out.push(names.get(v) ? `<${names.get(v)}>` : v.toString(16)); }
      console.log(name, 'pass bytes', pend - off, '\n' + out.join(' '));
    }
  });
}
function agg(map) {
  const s = [...map.values()].map(x => stat(x.code)).filter(Boolean);
  const mx = f => s.length ? Math.max(...s.map(f)) : -1, md = f => { const a = s.map(f).sort((x, y) => x - y); return a.length ? a[a.length >> 1] : -1; };
  return { n: s.length, s, mx, md };
}
if (mode === 'summary') {
  const re = new RegExp(arg || '.', 'i');
  console.log('tech|passes|PS n (v2/v3)|slots med/max|samplers med/max (maxIdx)|>=15samp|>=14samp|rtConst max idx|inputs max|PS flow|VS n|VS slots max|VS const max idx|VS texcoord out max|light refs');
  for (const [name, f] of [...fam].sort()) {
    const lightRefs = [...f.refs.keys()].filter(n => /light|basis|probe|shadow|ambient|spill/i.test(n));
    if (!re.test(name)) continue;
    const P = agg(f.ps), V = agg(f.vs);
    const v2 = P.s.filter(x => x.major === 2).length, v3 = P.s.filter(x => x.major === 3).length;
    console.log([name, f.passes, `${P.n} (${v2}/${v3})`, `${P.md(x => x.slots)}/${P.mx(x => x.slots)}`, `${P.md(x => x.nS)}/${P.mx(x => x.nS)} (${P.mx(x => x.maxS)})`, P.s.filter(x => x.nS >= 15).length, P.s.filter(x => x.nS >= 14).length, P.mx(x => x.maxRt), P.mx(x => x.nIn), P.s.filter(x => x.flow).length, V.n, V.mx(x => x.slots), V.mx(x => x.maxRt), V.mx(x => x.tc.size ? Math.max(...x.tc) : -1), lightRefs.join(',')].join('|'));
  }
} else if (mode === 'names') {
  console.log([...names.values()].filter(n => new RegExp(arg || '.', 'i').test(n)).sort().join('\n'));
} else if (mode === 'refs') {
  const f = fam.get(arg); console.log([...f.refs].map(([n, c]) => n + ':' + c).join(' '));
} else if (mode === 'global') {
  const all = new Map(); for (const f of fam.values()) for (const [i, x] of f.ps) all.set(i, x);
  const s = [...all.values()].map(x => stat(x.code)).filter(Boolean);
  const h = (f, edges) => edges.map((e, i) => `${i ? '>' + edges[i - 1] : ''}<=${e}:${s.filter(x => f(x) <= e && (i === 0 || f(x) > edges[i - 1])).length}`).join(' ');
  console.log('PS in techniques', s.length, 'v3', s.filter(x => x.major === 3).length, 'v2', s.filter(x => x.major === 2).length);
  console.log('samplers', h(x => x.nS, [4, 8, 10, 12, 13, 14, 15, 16]));
  const s3 = s.filter(x => x.major === 3), s2 = s.filter(x => x.major === 2);
  console.log('v3 slots', (f => [64, 128, 200, 256, 384, 448, 512, 9999].map((e, i, a) => `<=${e}:${s3.filter(x => f(x) <= e && (i === 0 || f(x) > a[i - 1])).length}`).join(' '))(x => x.slots));
  console.log('v2 slots(arith+tex; limit 64+32 for ps_2_0, 512 ps_2_x)', [32, 64, 96, 128, 9999].map((e, i, a) => `<=${e}:${s2.filter(x => x.slots <= e && (i === 0 || x.slots > a[i - 1])).length}`).join(' '), 'minor!=0:', s2.filter(x => x.minor).length);
  console.log('v3 inputs', [6, 8, 9, 10, 12].map((e, i, a) => `<=${e}:${s3.filter(x => x.nIn <= e && (i === 0 || x.nIn > a[i - 1])).length}`).join(' '));
  console.log('rt const max idx', [15, 31, 63, 127, 223, 9999].map((e, i, a) => `<=${e}:${s.filter(x => x.maxRt <= e && (i === 0 || x.maxRt > a[i - 1])).length}`).join(' '));
  console.log('technique count', fam.size, 'names', names.size);
}
if (mode === 'lm') {
  // per technique: passes binding a light texture; sampler index histogram; stats of the PS of those passes
  const want = /^(LightMap|RoomLightMap|CASLightMap|terrainLightMap|LightBasisMap[0-3]|terrainLightProbeMap|ShadowMap|shadowTextureData|DiffuseLightProbe|SpecularLightProbe|ExteriorDiffuseLightProbe|ExteriorSpecularLightProbe|CASDiffuseProbeTexture|BlurrySpecularLightProbe|SharpSpecularLightProbe|ShadowMainMap|simDropShadowTexture|AmbientOcclusionMap)$/;
  const re = new RegExp(arg || '.', 'i');
  for (let i = 0; i < techs.length; i++) {
    const at = techs[i].index, end = i + 1 < techs.length ? techs[i + 1].index : k;
    const name = names.get(b.readUInt32LE(at + 4)); if (!re.test(name)) continue;
    const ps = [...text.slice(at + 12, end).matchAll(/PASS/g)].map(m => at + 12 + m.index);
    const bind = new Map(), sel = new Map(), selV = new Map(); let nLm = 0;
    ps.forEach((off, j) => {
      const pend = j + 1 < ps.length ? ps[j + 1] : end; let has = false;
      for (let q = off + 24; q + 4 <= pend; q += 4) {
        const n = names.get(b.readUInt32LE(q)), idx = b.readUInt32LE(q - 4);
        if (n && want.test(n) && idx < 16) { const key = n + '@s' + idx; bind.set(key, (bind.get(key) || 0) + 1); if (/^(LightMap|RoomLightMap|terrainLightMap|CASLightMap)$/.test(n)) has = true; }
      }
      if (has) { nLm++; const vi = b.readUInt32LE(off + 12) - 1, pi = b.readUInt32LE(off + 16) - 1; if (blobs[pi]) sel.set(pi, blobs[pi]); if (blobs[vi]) selV.set(vi, blobs[vi]); }
    });
    if (!nLm) continue;
    const s = [...sel.values()].map(x => stat(x.code)).filter(Boolean), v = [...selV.values()].map(x => stat(x.code)).filter(Boolean);
    const mx = (a, f) => a.length ? Math.max(...a.map(f)) : -1, md = (a, f) => { const z = a.map(f).sort((x, y) => x - y); return z.length ? z[z.length >> 1] : -1; };
    const s3 = s.filter(x => x.major === 3), s2 = s.filter(x => x.major === 2);
    console.log(`## ${name}: passes ${ps.length}, lightmap passes ${nLm}; PS ${s.length} (ps_2_0 ${s2.length}, ps_3_0 ${s3.length})`);
    console.log(`  v3: slots med/max ${md(s3, x => x.slots)}/${mx(s3, x => x.slots)}; samplers med/max ${md(s3, x => x.nS)}/${mx(s3, x => x.nS)}; n>=13 ${s3.filter(x => x.nS >= 13).length}, >=15 ${s3.filter(x => x.nS >= 15).length}, =16 ${s3.filter(x => x.nS >= 16).length}; inputs max ${mx(s3, x => x.nIn)} (n=10: ${s3.filter(x => x.nIn >= 10).length}, n=9: ${s3.filter(x => x.nIn === 9).length}); rt const max idx ${mx(s3, x => x.maxRt)}; temps max ${mx(s3, x => x.temps)}; flow ${s3.filter(x => x.flow).length}`);
    console.log(`  v2: slots med/max ${md(s2, x => x.slots)}/${mx(s2, x => x.slots)} (tex max ${mx(s2, x => x.tex)}); samplers max ${mx(s2, x => x.nS)}; rt const max idx ${mx(s2, x => x.maxRt)}; nconst max ${mx(s2, x => x.nC)}; temps max ${mx(s2, x => x.temps)}`);
    const v3 = v.filter(x => x.major === 3);
    console.log(`  VS ${v.length} (vs_3_0 ${v3.length}): slots max ${mx(v, x => x.slots)}; rt const max idx ${mx(v, x => x.maxRt)}; outputs max ${mx(v3, x => x.nIn)}; texcoord idx max ${mx(v3, x => x.tc.size ? Math.max(...x.tc) : -1)}`);
    console.log('  bind: ' + [...bind].sort().map(([n, c]) => n + ':' + c).join(' '));
  }
}
if (mode === 'lm2') {
  const pop = v => { let c = 0; while (v) { c += v & 1; v >>>= 1; } return c; };
  const isLm = n => /^(LightMap|RoomLightMap|terrainLightMap|CASLightMap)$/.test(n);
  const re = new RegExp(arg || '.', 'i');
  const T = new Map(); let parsed = 0, unparsed = 0, cubeOk = 0, cubeBad = 0;
  function samplerTypes(code) { // sampler idx -> texture type (2=2D,3=cube,4=volume)
    const m = new Map(); let i = 4;
    while (i < code.length) { const t = code.readUInt32LE(i), op = t & 0xffff; if (op === 0xffff) break; if (op === 0xfffe) { i += 4 + ((t >>> 16) & 0x7fff) * 4; continue; } const len = (t >>> 24) & 15; if (op === 0x1f) { const r = code.readUInt32LE(i + 8); if ((((r >>> 28) & 7) | ((r >>> 8) & 0x18)) === 10) m.set(r & 0x7ff, (code.readUInt32LE(i + 4) >>> 27) & 15); } i += 4 + len * 4; }
    return m;
  }
  for (let i = 0; i < techs.length; i++) {
    const at = techs[i].index, end = i + 1 < techs.length ? techs[i + 1].index : k;
    const name = names.get(b.readUInt32LE(at + 4));
    if (!T.has(name)) T.set(name, { passes: 0, lmPasses: 0, bind: new Map(), ps: new Map(), vs: new Map(), allPs: new Map() });
    const R = T.get(name);
    const ps = [...text.slice(at + 12, end).matchAll(/PASS/g)].map(m => at + 12 + m.index);
    ps.forEach((off, j) => {
      const pend = j + 1 < ps.length ? ps[j + 1] : end; R.passes++;
      const vi = b.readUInt32LE(off + 12) - 1, pi = b.readUInt32LE(off + 16) - 1;
      if (blobs[pi]) R.allPs.set(pi, blobs[pi]);
      let found = null;
      for (let q = off + 24; q + 8 <= pend && !found; q += 4) {
        const mask = b.readUInt32LE(q - 4); if (mask === 0 || mask > 0xffff) continue;
        let n = 0; const list = [];
        while (q + n * 8 + 8 <= pend && names.has(b.readUInt32LE(q + n * 8 + 4)) && b.readUInt32LE(q + n * 8) < 64) { list.push(names.get(b.readUInt32LE(q + n * 8 + 4))); n++; }
        if (n && n === pop(mask)) found = { mask, list };
      }
      if (!found) { unparsed++; return; }
      parsed++;
      const idx = []; for (let s = 0; s < 16; s++) if (found.mask >>> s & 1) idx.push(s);
      const st = blobs[pi] ? samplerTypes(blobs[pi].code) : new Map();
      let has = false;
      found.list.forEach((n, z) => {
        if (/Probe/.test(n) && st.has(idx[z])) { if (st.get(idx[z]) === 3) cubeOk++; else { cubeBad++; if (mode === "lm2" && process.env.DBG && cubeBad % 700 === 1) console.log("BAD", name, found.mask.toString(16), found.list.join(","), [...st].map(([a, t]) => "s" + a + ":" + t).join(" ")); } }
        if (isLm(n) || /LightBasisMap|ShadowMap|shadowTex|Probe|AmbientOcc|simDropShadow/.test(n)) { const key = n + '@s' + idx[z]; R.bind.set(key, (R.bind.get(key) || 0) + 1); }
        if (isLm(n)) has = true;
      });
      if (has) { R.lmPasses++; if (blobs[pi]) R.ps.set(pi, blobs[pi]); if (blobs[vi]) R.vs.set(vi, blobs[vi]); }
    });
  }
  console.log(`passes parsed ${parsed}, unparsed ${unparsed}; probe names on cube samplers ${cubeOk}, on non-cube ${cubeBad}`);
  const mx = (a, f) => a.length ? Math.max(...a.map(f)) : '-', md = (a, f) => { const z = a.map(f).sort((x, y) => x - y); return z.length ? z[z.length >> 1] : '-'; };
  for (const [name, R] of [...T].sort()) {
    if (!re.test(name) || (!R.lmPasses && !arg)) continue;
    const src = R.lmPasses ? R.ps : R.allPs;
    const s = [...src.values()].map(x => stat(x.code)).filter(Boolean), v = [...R.vs.values()].map(x => stat(x.code)).filter(Boolean);
    const s3 = s.filter(x => x.major === 3), s2 = s.filter(x => x.major === 2), v3 = v.filter(x => x.major === 3);
    console.log(`## ${name}: passes ${R.passes}, lightmap passes ${R.lmPasses}; PS ${s.length} (v2 ${s2.length}, v3 ${s3.length})`);
    if (s3.length) console.log(`  ps_3_0: slots med/max ${md(s3, x => x.slots)}/${mx(s3, x => x.slots)}; samplers med/max ${md(s3, x => x.nS)}/${mx(s3, x => x.nS)}; n>=13:${s3.filter(x => x.nS >= 13).length} >=15:${s3.filter(x => x.nS >= 15).length} =16:${s3.filter(x => x.nS >= 16).length}; inputs max ${mx(s3, x => x.nIn)} (n=10:${s3.filter(x => x.nIn >= 10).length}, n=9:${s3.filter(x => x.nIn === 9).length}); rtconst maxidx ${mx(s3, x => x.maxRt)}; temps max ${mx(s3, x => x.temps)}; flow ${s3.filter(x => x.flow).length}`);
    if (s2.length) console.log(`  ps_2_0: slots med/max ${md(s2, x => x.slots)}/${mx(s2, x => x.slots)} (tex max ${mx(s2, x => x.tex)}); samplers max ${mx(s2, x => x.nS)}; rtconst maxidx ${mx(s2, x => x.maxRt)}; consts max ${mx(s2, x => x.nC)}; temps max ${mx(s2, x => x.temps)}`);
    if (v.length) console.log(`  VS ${v.length} (vs_3_0 ${v3.length}): slots max ${mx(v, x => x.slots)}; rtconst maxidx ${mx(v, x => x.maxRt)}; outputs max ${mx(v3, x => x.nIn)}; texcoord idx max ${mx(v3, x => x.tc.size ? Math.max(...x.tc) : -1)}`);
    const g = new Map(); for (const [key, c] of R.bind) { const [n, sIdx] = key.split('@'); if (!g.has(n)) g.set(n, []); g.get(n).push(sIdx + 'x' + c); }
    console.log('  bind: ' + [...g].sort().map(([n, a]) => n + '[' + a.sort().join(',') + ']').join(' '));
  }
}
if (mode === 'full') {
  const pop = v => { let c = 0; while (v) { c += v & 1; v >>>= 1; } return c; };
  const per = new Map(); // ps idx -> {tech set, light count max, names}
  for (let i = 0; i < techs.length; i++) {
    const at = techs[i].index, end = i + 1 < techs.length ? techs[i + 1].index : k;
    const name = names.get(b.readUInt32LE(at + 4));
    const ps = [...text.slice(at + 12, end).matchAll(/PASS/g)].map(m => at + 12 + m.index);
    ps.forEach((off, j) => {
      const pend = j + 1 < ps.length ? ps[j + 1] : end; const pi = b.readUInt32LE(off + 16) - 1; if (!blobs[pi]) return;
      let found = null;
      for (let q = off + 24; q + 8 <= pend && !found; q += 4) {
        const mask = b.readUInt32LE(q - 4); if (mask === 0 || mask > 0xffff) continue;
        let n = 0; const list = [];
        while (q + n * 8 + 8 <= pend && names.has(b.readUInt32LE(q + n * 8 + 4)) && b.readUInt32LE(q + n * 8) < 64) { list.push(names.get(b.readUInt32LE(q + n * 8 + 4))); n++; }
        if (n && n === pop(mask)) found = { mask, list };
      }
      if (!found) return;
      const room = found.list.filter(n => /^(LightMap|RoomLightMap|LightBasisMap[0-3])$/.test(n)).length, terr = found.list.filter(n => /^terrainLightMap$/.test(n)).length;
      if (!room && !terr) return;
      const e = per.get(pi) || { tech: new Set(), room: 0, terr: 0, bound: 0 }; e.tech.add(name); e.room = Math.max(e.room, room); e.terr = Math.max(e.terr, terr); e.bound = Math.max(e.bound, found.list.length); per.set(pi, e);
    });
  }
  const rows = [...per].map(([pi, e]) => ({ ...e, st: stat(blobs[pi].code) })).filter(r => r.st);
  const v3 = rows.filter(r => r.st.major === 3), v2 = rows.filter(r => r.st.major === 2);
  console.log('PS that bind a room/terrain light map:', rows.length, 'ps_3_0', v3.length, 'ps_2_0', v2.length, '| room-map', rows.filter(r => r.room).length, 'terrain-map only', rows.filter(r => !r.room).length);
  const roomV3 = v3.filter(r => r.room);
  for (const lim of [16, 15, 14, 13]) console.log(`room-map ps_3_0 with >=${lim} samplers: ${roomV3.filter(r => r.st.nS >= lim).length}; after dropping LightMap+basis: ${roomV3.filter(r => r.st.nS - r.room >= lim).length}`);
  console.log('room-map ps_3_0: free samplers after dropping light maps, min', Math.min(...roomV3.map(r => 16 - (r.st.nS - r.room))), '; PS with <2 free now:', roomV3.filter(r => 16 - r.st.nS < 2).length, 'with <3 free now:', roomV3.filter(r => 16 - r.st.nS < 3).length);
  const full = roomV3.filter(r => r.st.nS >= 15); const byT = new Map(); full.forEach(r => { const t = [...r.tech].join('+'); byT.set(t, (byT.get(t) || 0) + 1); });
  console.log('>=15 samplers by technique:', [...byT].map(([t, c]) => t + ':' + c).join(' '), '| light maps bound in them:', [...new Set(full.map(r => r.room))].join(','));
  const terrV3 = v3.filter(r => !r.room);
  console.log('terrain-map-only ps_3_0:', terrV3.length, '>=15 samplers', terrV3.filter(r => r.st.nS >= 15).length, '=16', terrV3.filter(r => r.st.nS >= 16).length);
  console.log('room-map ps_3_0 inputs: 10 used', roomV3.filter(r => r.st.nIn >= 10).length, '9 used', roomV3.filter(r => r.st.nIn === 9).length, '<=8', roomV3.filter(r => r.st.nIn <= 8).length);
  console.log('room-map ps_3_0 slots max', Math.max(...roomV3.map(r => r.st.slots)), 'p50', roomV3.map(r => r.st.slots).sort((a, b) => a - b)[roomV3.length >> 1], '; rt const max idx', Math.max(...roomV3.map(r => r.st.maxRt)), '; temps max', Math.max(...roomV3.map(r => r.st.temps)));
  const roomV2 = v2.filter(r => r.room);
  console.log('room-map ps_2_0:', roomV2.length, 'slots(arith+tex) max', Math.max(...roomV2.map(r => r.st.slots)), 'arith max', Math.max(...roomV2.map(r => r.st.slots - r.st.tex)), 'with arith>=56:', roomV2.filter(r => r.st.slots - r.st.tex >= 56).length, 'consts(max count incl def)', Math.max(...roomV2.map(r => r.st.nC)), 'temps max', Math.max(...roomV2.map(r => r.st.temps)), 'samplers max', Math.max(...roomV2.map(r => r.st.nS)));
  // all PS in any technique: global limits
  const allPs = new Map(); for (const f of fam.values()) for (const [i, x] of f.ps) allPs.set(i, x);
  const A = [...allPs.values()].map(x => stat(x.code)).filter(Boolean);
  console.log('all technique PS: ps_3_0 slots max', Math.max(...A.filter(x => x.major === 3).map(x => x.slots)), '; rt const max idx', Math.max(...A.map(x => x.maxRt)), '; ps_3_0 with 10 inputs', A.filter(x => x.major === 3 && x.nIn >= 10).length, 'of', A.filter(x => x.major === 3).length);
  const allVs = new Map(); for (const f of fam.values()) for (const [i, x] of f.vs) allVs.set(i, x);
  const V = [...allVs.values()].map(x => stat(x.code)).filter(Boolean);
  console.log('all technique VS:', V.length, 'vs_3_0', V.filter(x => x.major === 3).length, 'vs_2_0', V.filter(x => x.major === 2).length, 'vs_1_1', V.filter(x => x.major === 1).length, '; slots max', Math.max(...V.map(x => x.slots)), '; rt const max idx', Math.max(...V.map(x => x.maxRt)), '; vs_3_0 outputs max', Math.max(...V.filter(x => x.major === 3).map(x => x.nIn)), ', with 12 outputs:', V.filter(x => x.major === 3 && x.nIn >= 12).length, ', 11:', V.filter(x => x.major === 3 && x.nIn === 11).length);
}
// ---- census mode (S0-04): per pass constant tables, LightmapSizeParameters readers, light-map coordinates and
// directional-map weights. node precomp.cjs <precomp> census [techRegex] -> JSON on stdout (census-report.cjs makes the tables).
// Pass layout (verified against decoded shaders): after the 'PASS' header come MRAP blocks (first = VS, second = PS):
// 'MRAP', id, count, then count x (name, w1, w2); w1 = count<<24 | register<<14 | flags, so register = (w1>>>14)&0x3ff.
function decodeShader(code) {
  const ver = code.readUInt32LE(0), isPs = (ver >>> 16) === 0xffff;
  const ins = [], defs = new Map(); let i = 4;
  const P = v => ({ ty: ((v >>> 28) & 7) | ((v >>> 8) & 0x18), n: v & 0x7ff, sw: (v >>> 16) & 255, wm: (v >>> 16) & 15, mod: (v >>> 24) & 15, rel: (v >>> 13) & 1 });
  while (i < code.length) {
    const t = code.readUInt32LE(i), op = t & 0xffff;
    if (op === 0xffff) break;
    if (op === 0xfffe) { i += 4 + ((t >>> 16) & 0x7fff) * 4; continue; }
    const len = (t >>> 24) & 15, ps = [];
    for (let j = 0; j < len; j++) ps.push(code.readUInt32LE(i + 4 + j * 4));
    if (op === 0x51) defs.set(ps[0] & 0x7ff, [1, 2, 3, 4].map(z => Math.round(code.readFloatLE(i + 4 + z * 4) * 1e4) / 1e4));
    else if (op === 0x30 || op === 0x2f) defs.set(ps[0] & 0x7ff, null);
    else if (op !== 0x1f && op !== 0 && len > 0) ins.push({ op, dst: P(ps[0]), src: ps.slice(1).map(P) });
    i += 4 + len * 4;
  }
  return { isPs, ins, defs };
}
const OPN = { 1: 'mov', 2: 'add', 3: 'sub', 4: 'mad', 5: 'mul', 6: 'rcp', 7: 'rsq', 8: 'dp3', 9: 'dp4', 10: 'min', 11: 'max', 12: 'slt', 13: 'sge', 18: 'lrp', 19: 'frc', 0x20: 'pow', 0x23: 'abs', 0x24: 'nrm', 0x41: 'texkill', 0x42: 'texld', 0x58: 'cmp', 0x5a: 'dp2add', 0x5d: 'texldd', 0x5f: 'texldl' };
const opn = o => OPN[o] || '0x' + o.toString(16);
const swc = s => 'xyzw'[s & 3] + 'xyzw'[(s >> 2) & 3] + 'xyzw'[(s >> 4) & 3] + 'xyzw'[(s >> 6) & 3];
if (mode === 'census') {
  const re = new RegExp(arg || '.', 'i'), pop = v => { let c = 0; while (v) { c += v & 1; v >>>= 1; } return c; };
  const dec = new Map(); const D = i => { if (!dec.has(i)) dec.set(i, decodeShader(blobs[i].code)); return dec.get(i); };
  const isLmName = n => /^(LightMap|RoomLightMap|terrainLightMap|CASLightMap)$/.test(n);
  const out = [];
  for (let i = 0; i < techs.length; i++) {
    const at = techs[i].index, end = i + 1 < techs.length ? techs[i + 1].index : k;
    const name = names.get(b.readUInt32LE(at + 4)); if (!re.test(name)) continue;
    const ps = [...text.slice(at + 12, end).matchAll(/PASS/g)].map(m => at + 12 + m.index);
    ps.forEach((off, j) => {
      const pend = j + 1 < ps.length ? ps[j + 1] : end;
      const vi = b.readUInt32LE(off + 12) - 1, pi = b.readUInt32LE(off + 16) - 1;
      const blocks = [];
      for (let q = off + 20; q + 12 <= pend; q += 4) if (b.readUInt32LE(q) === 0x4d524150) {
        const cnt = b.readUInt32LE(q + 8), ents = []; let r = q + 12, okb = true;
        for (let e = 0; e < cnt; e++, r += 12) { const n = names.get(b.readUInt32LE(r)); if (!n || r + 12 > pend) { okb = false; break; } const w = b.readUInt32LE(r + 4); ents.push({ n, cnt: w >>> 24, reg: (w >>> 14) & 0x3ff }); }
        if (okb) { blocks.push(ents); q = r - 4; }
      }
      let found = null;
      for (let s = off + 24; s + 8 <= pend && !found; s += 4) {
        const mask = b.readUInt32LE(s - 4); if (mask === 0 || mask > 0xffff) continue;
        let n = 0; const list = [];
        while (s + n * 8 + 8 <= pend && names.has(b.readUInt32LE(s + n * 8 + 4)) && b.readUInt32LE(s + n * 8) < 64) { list.push(names.get(b.readUInt32LE(s + n * 8 + 4))); n++; }
        if (n && n === pop(mask)) found = { mask, list };
      }
      const samp = []; if (found) { let z = 0; for (let s = 0; s < 16; s++) if (found.mask >>> s & 1) samp.push([s, found.list[z++]]); }
      const hasLsp = blocks.some(bl => bl.some(e => e.n === 'LightmapSizeParameters'));
      const hasLm = samp.some(([, n]) => isLmName(n)), hasBasis = samp.some(([, n]) => /^LightBasisMap/.test(n));
      if (!hasLsp && !hasLm && !hasBasis) return;
      out.push({ tech: name, vi, pi, blocks, samp, hasLsp, hasLm, hasBasis });
    });
  }
  const uniq = new Map();
  for (const p of out) { const key = p.tech + '|' + p.vi + '|' + p.pi + '|' + JSON.stringify(p.samp); if (!uniq.has(key)) uniq.set(key, { ...p, count: 0 }); uniq.get(key).count++; }
  const res = { passes: out.length, rows: [] };
  // tokens that feed operand o (read by instruction `at`): ops in the expression tree, runtime constants, defs, inputs, texture reads
  const desc = (d, at, o, set = new Set(), depth = 0) => {
    const comp = o.sw & 3;
    if (o.ty === 2) { const v = d.defs.get(o.n); set.add(v ? 'def' : `rtc${o.n}${o.rel ? '[a0]' : ''}`); }
    else if (o.ty === 1 || o.ty === 3) set.add('in');
    else if (o.ty === 0 && depth < 6) {
      for (let z = at - 1; z >= 0; z--) {
        const w = d.ins[z]; if (w.dst.ty !== 0 || w.dst.n !== o.n || !((w.dst.wm >> comp) & 1)) continue;
        if (w.op === 0x42 || w.op === 0x5f) { set.add('tex'); break; }
        set.add(opn(w.op)); w.src.forEach(x => desc(d, z, x, set, depth + 1)); break;
      }
    }
    return depth ? set : [...set].sort().join('+');
  };
  const dirOf = (d, at, o, depth = 0) => { // nearest dp2add (N . direction) feeding operand o: "<n swizzle>.<def pair>"
    if (o.ty !== 0 || depth > 6) return null; const comp = o.sw & 3;
    for (let z = at - 1; z >= 0; z--) {
      const w = d.ins[z]; if (w.dst.ty !== 0 || w.dst.n !== o.n || !((w.dst.wm >> comp) & 1)) continue;
      if (w.op === 0x5a) { const c = w.src[1], v = c.ty === 2 && d.defs.get(c.n); return `${swc(w.src[0].sw).slice(0, 2)}.${v ? [v[c.sw & 3], v[(c.sw >> 2) & 3]].join(',') : 'rtc' + c.n}`; }
      for (const x of w.src) { const r = dirOf(d, z, x, depth + 1); if (r) return r; }
      return null;
    }
    return null;
  };
  for (const u of uniq.values()) {
    const row = { tech: u.tech, vi: u.vi, pi: u.pi, count: u.count, hasLm: u.hasLm, hasBasis: u.hasBasis, samp: u.samp.map(([s, n]) => s + ':' + n).join(','), lsp: [], lmCoord: [], basisW: [], basisDir: [], vsMax: -1, psMax: -1, vsBound: -1, psBound: -1, psVer: 0, vsVer: 0 };
    u.blocks.forEach((bl, bi) => {
      const stage = bi === 0 ? 'VS' : 'PS', idx = bi === 0 ? u.vi : u.pi;
      row[stage === 'VS' ? 'vsBound' : 'psBound'] = Math.max(-1, ...bl.map(e => e.reg + e.cnt - 1));
      const e = bl.find(x => x.n === 'LightmapSizeParameters');
      if (e && blobs[idx]) {
        const d = D(idx), uses = [];
        for (const ins of d.ins) ins.src.forEach(s => { if (s.ty === 2 && s.n >= e.reg && s.n < e.reg + e.cnt && !d.defs.has(s.n)) uses.push(`${opn(ins.op)}.${swc(s.sw)}->${['r', 'v', 'c', 'a', 'oT', 'ob', 'o'][ins.dst.ty] || ins.dst.ty}${ins.dst.n}`); });
        row.lsp.push({ stage, reg: e.reg, reads: uses.length, uses: [...new Set(uses)] });
      }
    });
    if (blobs[u.vi]) { const s = stat(blobs[u.vi].code); if (s) { row.vsMax = s.maxRt; row.vsVer = s.major; } }
    if (blobs[u.pi]) { const s = stat(blobs[u.pi].code); if (s) { row.psMax = s.maxRt; row.psVer = s.major; } }
    if (blobs[u.pi] && (u.hasLm || u.hasBasis)) {
      const d = D(u.pi), name = Object.fromEntries(u.samp);
      d.ins.forEach((ins, at) => {
        if (ins.op !== 0x42 && ins.op !== 0x5f) return;
        const sn = name[ins.src[1].n]; if (!sn) return;
        const co = ins.src[0];
        if (isLmName(sn) || /^LightBasisMap/.test(sn)) row.lmCoord.push(`${sn}<-${co.ty === 1 || co.ty === 3 ? 'in' + co.n : desc(d, at, co)}.${swc(co.sw)}`);
        if (/^LightBasisMap/.test(sn)) { // first reader of the sampled colour: a weight (mul/mad) or a luminance dot (dp3)
          const z = d.ins.findIndex((w, q) => q > at && w.src.some(x => x.ty === 0 && x.n === ins.dst.n));
          if (z < 0) { row.basisW.push(`${sn}*none`); return; }
          const w = d.ins[z], si = w.src.findIndex(x => x.ty === 0 && x.n === ins.dst.n);
          if (w.op === 4 || w.op === 5) { const wo = w.src[w.op === 4 ? (si === 0 ? 1 : 0) : 1 - si]; row.basisW.push(`${sn}*W:${desc(d, z, wo)}`); row.basisDir.push(`${sn}=${dirOf(d, z, wo)}`); }
          else if (w.op === 8) row.basisW.push(`${sn}*LUM:${desc(d, z, w.src[1 - si])}`);
          else row.basisW.push(`${sn}*${opn(w.op)}`);
        }
      });
    }
    res.rows.push(row);
  }
  console.log(JSON.stringify(res));
}
if (mode === 'blob') { fs.writeFileSync(process.argv[5], blobs[+arg].code); } // blob <idx> <outfile>: raw shader bytecode for fxc /dumpbin
if (mode === 'parm') {
  // The game's constant parameter tables, decoded as TS3W 1.67's loader does (research 13 §2.5): per pass a 'PARM' table
  // for the vertex shader, then one for the pixel shader; a record is (name hash, w1, w2). Type (w1 >> 11) & 7; type 2
  // records take no float registers. Start and count are bits 8-17 and 0-7 of the record field the loader builds; the
  // loader sizes the staging buffer from max(start + count), so that is the highest register Shader_SetFloatConstants
  // can write. Prints the highest per kind and the techniques past c<arg> (default 227) in the vertex tables.
  const PARM = 0x4d524150, limit = +(arg || 227), field = (w1, w2) => {
    let e = ((w2 & 0x800001ff) | (w1 >>> 15)) >>> 2; e = (e | (w2 & 0x40000000)) >>> 1; e = (e | (w1 & 0xffc000)) >>> 6;
    return (e | (((w1 & 0x600) + 0x200) << 10)) >>> 0;
  };
  const hi = { VS: 0, PS: 0 }, past = new Map(); let passes = 0, missing = 0;
  for (let i = 0; i < techs.length; i++) {
    const at = techs[i].index, end = i + 1 < techs.length ? techs[i + 1].index : k, name = names.get(b.readUInt32LE(at + 4));
    for (const m of text.slice(at + 12, end).matchAll(/PASS/g)) {
      let q = at + 12 + m.index + 20; passes++;
      while (q < at + 12 + m.index + 96 && b.readUInt32LE(q) !== PARM) q += 4; // the pet fur passes carry a longer header
      for (const kind of ['VS', 'PS']) {
        if (b.readUInt32LE(q) !== PARM) { missing++; break; }
        const n = b.readUInt32LE(q + 8); q += 12;
        for (let r = 0; r < n; r++, q += 12) {
          const w1 = b.readUInt32LE(q + 4), f = field(w1, b.readUInt32LE(q + 8)), last = ((f >>> 8) & 0x3ff) + (f & 0xff) - 1;
          if (((w1 >>> 11) & 7) === 2) continue;
          hi[kind] = Math.max(hi[kind], last);
          if (kind === 'VS' && last > limit) past.set(name, Math.max(past.get(name) || 0, last));
        }
      }
    }
  }
  console.log(`passes ${passes}, tables not found ${missing}; highest register written: VS c${hi.VS}, PS c${hi.PS}`);
  console.log(`VS past c${limit}: ${[...past].map(([t, c]) => `${t} c${c}`).join(', ') || 'none'}`);
}
