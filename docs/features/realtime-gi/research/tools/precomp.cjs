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
