// Composited albedo, family rule (research 14 WP10; docs/features/realtime-gi/wp/GB-04.md). Used by generate.cjs.
// "Albedo meets light": per temp component, a mark of the rgb channels of an albedo-class texture read it depends on
// and a mark for light (a light-class texture read or an nrm result). The albedo is rR.xyz, the one temp source of the
// first component-wise instruction that writes .xyz, reads rR.xyz unswizzled with the albedo mark and no light mark,
// and has another source with the light mark. Without such an instruction, an oC0 .xyz write with the albedo mark and
// no light mark is an unlit pass: its colour is the albedo. Sampler classes come from the pass's texture names: albedo,
// neutral material (blend weights, detail, tone maps) or light (every other name, also unknown ones: a new material
// texture gives a refusal or an early candidate, never a lit albedo). Inputs and constants are neutral.
// Result: { status, at, reg } with at = the instruction's index counted without dcl / def (the index AddGBufferPs
// takes, the same before and after the Banding Fix copy), reg = the temp after that instruction, or 255 for the oC0 case
// (the instruction itself is copied). status 'ok' / 'unlit' have a value; 'none', 'ambiguous', 'flow' do not.
const ALB = /^(OverlayTexture|DiffuseMap|colorMap[234]?|compositeMap|TerrainSnowDiffuseMap|RoadTexture|TwigDiffuseMap)$/;
const MAT = /^(blendMap|terrain_detail|CutoutMap|SkinToneRampTexture|SkinDetailTexture(Dark)?|DualSkinToneMap|SnowMeltingMap|SnowMeltingNoiseMap|SnowDoorAlphaMap|worldLeafMap|WorldLeavesTexture|CASMap1Texture|BloodMap|GhostNoiseTexture)$/;
const type = r => ((r >>> 28) & 7) | ((r >>> 8) & 0x18), num = r => r & 0x7ff;
const swz = r => [0, 1, 2, 3].map(c => (r >>> (16 + 2 * c)) & 3);
const mask = r => (r >>> 16) & 15;
const DECL = new Set([31, 81, 47, 48]); // dcl, def, defb, defi
const COMPWISE = new Set([1, 2, 3, 4, 5, 10, 11, 12, 13, 18, 19, 34, 35, 86, 87, 80, 88]); // mov add sub mad mul min max slt sge lrp frc abs sgn cmp cnd
const SCALAR = new Set([6, 7, 14, 15, 32, 37]); // rcp rsq exp log pow ...
const FLOW = new Set([0x1b, 0x1d, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x60, 0x19, 0x1a, 0x1c, 0x1e]);
const TEXLD = new Set([0x42, 0x5f, 0x5d, 0x43]);

function parse(code) {
  const ins = [];
  for (let i = 4; i < code.length;) {
    const t = code.readUInt32LE(i), op = t & 0xffff;
    if (op === 0xffff) return ins;
    if (op === 0xfffe) { i += 4 + ((t >>> 16) & 0x7fff) * 4; continue; }
    const len = (t >>> 24) & 15, ops = [];
    for (let k = 1; k <= len; k++) ops.push(code.readUInt32LE(i + 4 * k));
    ins.push({ op, ops });
    i += 4 + len * 4;
  }
  throw Error('no END');
}

// sampler -> 'A' albedo, 'M' neutral material, 'L' light; bind: Map texture name -> Set of samplers (the pass records)
function samplerClasses(bind) {
  const cls = new Map();
  for (const [n, set] of bind) for (const s of set) {
    const c = ALB.test(n) ? 'A' : MAT.test(n) ? 'M' : 'L';
    cls.set(s, cls.has(s) && cls.get(s) !== c ? 'L' : c); // one sampler under two classes: light
  }
  return cls;
}

// code: the shader's bytes
function albedoRule(code, bind) {
  const cls = samplerClasses(bind);
  if (![...cls.values()].includes('A')) return { status: 'none' };
  const ins = parse(code);
  const defs = new Set(ins.filter(q => q.op === 81).map(q => num(q.ops[0])));
  const taint = new Map(), vers = new Map(), none = { a: 0, l: false };
  const get = (r, c) => (type(r) === 0 ? taint.get(num(r) * 4 + c) || none : none);
  let depth = 0, ord = -1, oc0 = null;
  const cand = new Map();
  const ordOf = [], depthAt = [];
  for (let i = 0; i < ins.length; i++) {
    const x = ins[i], op = x.op;
    ordOf.push(DECL.has(op) ? -1 : ++ord);
    depthAt.push(depth);
    if (DECL.has(op) || op === 0 || op === 65) continue; // dcl / def, nop, texkill (counted like AddGBufferPs: all but dcl / def)
    if (FLOW.has(op)) { depth += op === 0x1b || op === 0x26 || op === 0x28 || op === 0x29 ? 1 : op === 0x1d || op === 0x27 || op === 0x2b ? -1 : 0; continue; }
    const dst = x.ops[0], dm = mask(dst), srcs = x.ops.slice(1), out = [];
    if (TEXLD.has(op)) {
      const c = cls.get(num(srcs[1])) || 'L';
      for (let k = 0; k < 4; k++) out[k] = { a: c === 'A' && k < 3 ? 1 << k : 0, l: c === 'L' };
    } else {
      const reads = (si, k) => {
        const s = swz(srcs[si]);
        if (op === 8 || op === 36 || op === 33) return [s[0], s[1], s[2]]; // dp3, nrm, crs
        if (op === 9) return s;                                             // dp4
        if (op === 90) return si < 2 ? [s[0], s[1]] : [s[0]];               // dp2add
        if (SCALAR.has(op)) return [s[0]];
        return [s[k]];
      };
      for (let k = 0; k < 4; k++) {
        if (!(dm >> k & 1)) continue;
        let a = 0, l = op === 36; // an nrm result is a normal: light
        for (let si = 0; si < srcs.length; si++) for (const c of reads(si, k)) { const t = get(srcs[si], c); a |= t.a; l = l || t.l; }
        out[k] = { a, l };
      }
      if (COMPWISE.has(op) && (dm & 7) === 7) {
        const per = srcs.map(r => {
          const s = swz(r), ts = [0, 1, 2].map(k => get(r, s[k]));
          return { r, ident: type(r) === 0 && s[0] === 0 && s[1] === 1 && s[2] === 2, alb: ts.every(t => t.a && !t.l), light: ts.some(t => t.l) };
        });
        if (per.some(p => p.light)) for (const p of per) if (p.ident && p.alb) {
          const R = num(p.r), v = [0, 1, 2].map(k => vers.get(R * 4 + k));
          const key = R + ':' + v.join(',');
          if (!cand.has(key)) cand.set(key, { def: Math.max(...v), reg: R });
        }
      }
    }
    if (type(dst) === 0) for (let k = 0; k < 4; k++) if (dm >> k & 1) { taint.set(num(dst) * 4 + k, out[k]); vers.set(num(dst) * 4 + k, i); }
    if (type(dst) === 8 && num(dst) === 0 && (dm & 7) === 7 && [0, 1, 2].every(k => out[k].a && !out[k].l)) oc0 = { i, flow: depth > 0 };
  }
  const c = [...cand.values()];
  if (c.length > 1) return { status: 'ambiguous' };
  if (c.length === 1) return depthAt[c[0].def] > 0 ? { status: 'flow' } : { status: 'ok', at: ordOf[c[0].def], reg: c[0].reg };
  if (oc0) return oc0.flow ? { status: 'flow' } : { status: 'unlit', at: ordOf[oc0.i], reg: 255 };
  return { status: 'none' };
}

module.exports = { albedoRule, samplerClasses };
