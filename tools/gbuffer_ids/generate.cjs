// G-buffer shader table (docs/features/realtime-gi/research/14-gbuffer-motion-vectors.md §3.2).
// Usage: node generate.cjs <Shaders_Win32.precomp> [output-header] [families-file] [rule-classes-file]
// Read-only over the precomp; writes only the header and the families file when paths are given. No game bytecode is
// written: per ps_3_0 and ps_2_0 pixel shader of a technique its hash, the sampler a pass names as the albedo texture,
// the sampler of the diffuse light probe, a material id and a motion class. The sampler names exist only in the pass
// records, not in the bytecode. The families file (one line per technique: name, then the hashes of its pixel shaders)
// is for the offline check's counts per family (tools/gbuffer_check); it is not committed.
const fs = require('fs');
const { albedoRule, samplerClasses } = require('./albedo_rule.cjs');
if (!process.argv[2]) throw Error('Usage: node generate.cjs Shaders_Win32.precomp [output-header] [families-file] [rule-classes-file]');
const ALBEDO = new Set(['DiffuseMap', 'colorMap', 'RoadTexture', 'TwigDiffuseMap']);
const PROBE = new Set(['DiffuseLightProbe', 'ExteriorDiffuseLightProbe']);
// Albedo composited from several textures (a skin or clothing overlay, terrain layers by a blend map): no albedo sampler;
// the family rule of albedo_rule.cjs (research 14 WP10) finds the register instead. By the textures a pass binds, and by
// family for the two big ones.
const NO_ALBEDO = ['SimSkin', 'TerrainHigh'];
const COMPOSITED = new Set(['OverlayTexture', 'blendMap', 'colorMap2', 'colorMap3', 'colorMap4', 'compositeMap']);
// Material ids: the first rule a technique name matches, rules in priority order (a shader shared by several techniques
// takes the lowest id). The names and roughness values go to the header as they are.
const MATERIALS = [
  ['Skin', 0.6, /^(SimSkin|SimHorse|GhostHorse|SimRobot|SimpleSim|CASSkin)/],
  ['Hair', 0.5, /^(SimHair|SimEyelashes|SimPetFur)/],
  ['Eyes', 0.2, /^SimEyes/],
  ['Glass', 0.1, /Glass|Window/],
  ['Water', 0.1, /Water|Sea|Pond|puddle|FluidEffect/],
  ['Foliage', 0.8, /^(leaf|frond|branchMain|Foliage|LeavesTile)/],
  ['Terrain', 0.9, /Terrain|^LotImposter|^InitialPaints|^InitialCausticsPaints/],
  ['Road', 0.8, /^Road/],
  ['Roof', 0.7, /^Roof/],
  ['Wall', 0.9, /Wall/],
  ['Floor', 0.5, /Floor|Ceiling|^Rug|^BurntTile/],
  ['Counter', 0.3, /^Counters/],
  ['Metal', 0.3, /^(BrushedMetal|Gemstone|Jade)/],
  ['Landmark', 0.7, /^Landmark/],
  ['Object', 0.6, /^(Day|Night|Phong|SingleObject|InstancedObject|OutdoorProp|Sculpture|Hideable|Painting)/],
  ['Other', 0.7, /./],
];
// Motion class 3 (unknown mover, research 14 §2c): skinned, wind-animated and water families
const MOVER = /^(Sim|GhostHorse|leaf|frond|branchMain|Foliage)|Water|Sea|Pond/;

const b = fs.readFileSync(process.argv[2]);
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
  if (begin < 0 || size > 100000) throw Error('Bad blob ' + at);
  const code = b.subarray(begin, at + 8 + size);
  if (code.readUInt32LE(code.length - 4) !== 0xffff) throw Error('No END ' + at);
  let hash = 2166136261;
  for (let j = 0; j < code.length; j += 4) hash = Math.imul(hash ^ code.readUInt32LE(j), 16777619) >>> 0;
  blobs.push({ code, hash, version: code.readUInt32LE(0) });
}
const pop = v => { let c = 0; while (v) { c += v & 1; v >>>= 1; } return c; };
// per pixel shader: its techniques and, per texture name, the samplers the passes bind it to
const techs = [...text.matchAll(/TECH/g)].filter(m => m.index > 36000000 && m.index < k && names.has(b.readUInt32LE(m.index + 4)));
const per = new Map();
for (let i = 0; i < techs.length; i++) {
  const at = techs[i].index, end = i + 1 < techs.length ? techs[i + 1].index : k;
  const name = names.get(b.readUInt32LE(at + 4));
  const passes = [...text.slice(at + 12, end).matchAll(/PASS/g)].map(m => at + 12 + m.index);
  passes.forEach((off, j) => {
    const pend = j + 1 < passes.length ? passes[j + 1] : end, pi = b.readUInt32LE(off + 16) - 1;
    if (pi < 0) return;
    const blob = blobs[pi];
    if (!blob || (blob.version >>> 16) !== 0xffff) throw Error('Bad PS index');
    if (!per.has(blob.hash)) per.set(blob.hash, { blob, tech: new Set(), bind: new Map() });
    const e = per.get(blob.hash);
    if (e.blob.code.length !== blob.code.length) throw Error('Hash collision ' + blob.hash.toString(16));
    e.tech.add(name);
    // the sampler bit mask and one (dword, name) pair per set bit, after the pass's render states (research 02 §0)
    let found = null;
    for (let q = off + 24; q + 8 <= pend && !found; q += 4) {
      const mask = b.readUInt32LE(q - 4);
      if (mask === 0 || mask > 0xffff) continue;
      const list = [];
      while (q + list.length * 8 + 8 <= pend && names.has(b.readUInt32LE(q + list.length * 8 + 4)) && b.readUInt32LE(q + list.length * 8) < 64)
        list.push(names.get(b.readUInt32LE(q + list.length * 8 + 4)));
      if (list.length && list.length === pop(mask)) found = { mask, list };
    }
    if (!found) return;
    let z = 0;
    for (let s = 0; s < 16; s++)
      if ((found.mask >>> s) & 1) {
        const n = found.list[z++];
        if (!e.bind.has(n)) e.bind.set(n, new Set());
        e.bind.get(n).add(s);
      }
  });
}
// texture reads of a sampler: [count, the coordinate is a temp, the coordinate is an input register (v#, t#) that no
// other instruction reads] (texld, texldl, texldd)
const regType = r => ((r >>> 28) & 7) | ((r >>> 8) & 0x18);
function reads(code, sampler) {
  let n = 0, temp = false, coord = -1;
  const uses = new Map(); // input register (type * 4096 + number) -> source operands that read it
  for (let i = 4; i < code.length;) {
    const t = code.readUInt32LE(i), op = t & 0xffff;
    if (op === 0xffff) break;
    if (op === 0xfffe) { i += 4 + ((t >>> 16) & 0x7fff) * 4; continue; }
    const len = (t >>> 24) & 15;
    if (op !== 31 && op !== 81 && op !== 47 && op !== 48) // not dcl / def: operand 1 is the destination
      for (let k = 2; k <= len; k++) {
        const r = code.readUInt32LE(i + 4 * k), y = regType(r);
        if ((r & 0x80000000) && (y === 1 || y === 3)) uses.set(y * 4096 + (r & 0x7ff), (uses.get(y * 4096 + (r & 0x7ff)) || 0) + 1);
      }
    if (op === 0x42 || op === 0x5f || op === 0x5d) {
      const c = code.readUInt32LE(i + 8), s = code.readUInt32LE(i + 12);
      if ((s & 0x7ff) === sampler) { n++; temp = regType(c) === 0; coord = regType(c) === 1 || regType(c) === 3 ? regType(c) * 4096 + (c & 0x7ff) : -1; }
    }
    i += 4 + len * 4;
  }
  return [n, temp, coord >= 0 && uses.get(coord) === 1];
}
// the one sampler every pass binds one of these names to, read exactly once; -1 otherwise. probe: also -1 when the read's
// coordinate is an input register nothing else reads: 48 Counters ps_2_0 shadow variants read the probe sampler with the
// light-space position the vertex shader passes (research 14 §3.4; docs/features/realtime-gi/wp/GB-04.md); a shading
// normal from the vertex shader is also used for the light (verified on the 204 other raw-input probe reads)
function single(e, wanted, probe = false) {
  const s = new Set();
  for (const [n, set] of e.bind) if (wanted.has(n)) for (const x of set) s.add(x);
  if (s.size !== 1) return [-1, false];
  const [sampler] = s, [n, temp, rawOnly] = reads(e.blob.code, sampler);
  return n === 1 && !(probe && rawOnly) ? [sampler, temp] : [-1, false];
}
const rows = [], rules = [], stats = { 3: { n: 0, albedo: 0, probe: 0, rule: 0, composited: 0 }, 2: { n: 0, albedo: 0, probe: 0, rule: 0, composited: 0 } };
for (const e of per.values()) {
  const v = e.blob.version === 0xffff0300 ? 3 : e.blob.version === 0xffff0200 ? 2 : 0;
  if (!v) continue; // (ps_1_1: no copy)
  const id = 1 + Math.min(...[...e.tech].map(t => MATERIALS.findIndex(m => m[2].test(t))));
  const cls = [...e.tech].some(t => MOVER.test(t)) ? 3 : 0;
  const composited = NO_ALBEDO.some(f => e.tech.has(f)) || [...e.bind.keys()].some(n => COMPOSITED.has(n));
  const [albedo] = composited ? [-1] : single(e, ALBEDO), [probe] = single(e, PROBE, true);
  stats[v].n++; stats[v].albedo += albedo >= 0; stats[v].probe += probe >= 0;
  if (composited) {
    const r = albedoRule(e.blob.code, e.bind);
    stats[v].composited++;
    if (r.status === 'ok' || r.status === 'unlit') rules.push({ hash: e.blob.hash, at: r.at, reg: r.reg, cls: samplerClasses(e.bind) }), stats[v].rule++;
  }
  rows.push({ hash: e.blob.hash, albedo: albedo < 0 ? 255 : albedo, probe: probe < 0 ? 255 : probe, id, cls, tech: e.tech });
}
rows.sort((x, y) => x.hash - y.hash);
rules.sort((x, y) => x.hash - y.hash);
const hex = (v, n) => '0x' + v.toString(16).toUpperCase().padStart(n, '0');
const header = `#pragma once
// G-buffer shader table (tools/gbuffer_ids/generate.cjs, Steam Shaders_Win32.precomp): per ps_3_0 and ps_2_0 pixel
// shader of a technique, the sampler a pass names DiffuseMap / colorMap / RoadTexture (albedo, read by exactly one texld),
// the sampler of the diffuse light probe (read once: its coordinate is the world shading normal), a material id and
// the motion class (3 = moves by itself, no vector). 255 = no such sampler. Identifiers only, no game bytecode.
// Generated; do not edit. Sorted by hash (FNV-1a 32 over the bytecode's DWORDs, ShaderHash in shader_ids.h).
#include <algorithm>
#include <cstdint>
#include <iterator>

struct GBufferShaderId {
    uint32_t hash;
    uint8_t albedo, probe, material;
    uint8_t motionClass; // 0 static .. 3 moves by itself
};

// material ids 1.. (0 = nothing written, 255 = albedo unknown) and the roughness written for each
inline constexpr const char* kGBufferMaterials[] = {"None", ${MATERIALS.map(m => `"${m[0]}"`).join(', ')}};
inline constexpr float kGBufferRoughness[] = {0.0f, ${MATERIALS.map(m => m[1].toFixed(1) + 'f').join(', ')}};

inline constexpr GBufferShaderId kGBufferShaders[] = {
${rows.map(r => `    {${hex(r.hash, 8)}u, ${r.albedo}, ${r.probe}, ${r.id}, ${r.cls}},`).join('\n')}
};

// Composited albedo (family rule, tools/gbuffer_ids/albedo_rule.cjs): for a shader whose row has no albedo sampler, the
// albedo is r<reg>.xyz right after its at-th instruction counted without dcl / def, or with reg 255 a copy of that
// instruction (an unlit pass's oC0 write). ShaderPatches::GBufferInfo::albedoAt / albedoReg.
struct GBufferAlbedoRule {
    uint32_t hash;
    uint16_t at;
    uint8_t reg, unused;
};
inline constexpr GBufferAlbedoRule kGBufferAlbedoRules[] = {
${rules.map(r => `    {${hex(r.hash, 8)}u, ${r.at}, ${r.reg}, 0},`).join('\n')}
};
inline const GBufferAlbedoRule* FindGBufferAlbedoRule(uint32_t hash) {
    const auto it = std::lower_bound(std::begin(kGBufferAlbedoRules), std::end(kGBufferAlbedoRules), hash, [](const GBufferAlbedoRule& r, uint32_t h) { return r.hash < h; });
    return it != std::end(kGBufferAlbedoRules) && it->hash == hash ? &*it : nullptr;
}

// the row of a shader, or null (a shader of another family, another game version or a mod)
inline const GBufferShaderId* FindGBufferShader(uint32_t hash) {
    const auto it = std::lower_bound(std::begin(kGBufferShaders), std::end(kGBufferShaders), hash, [](const GBufferShaderId& r, uint32_t h) { return r.hash < h; });
    return it != std::end(kGBufferShaders) && it->hash == hash ? &*it : nullptr;
}
`;
if (process.argv[3]) fs.writeFileSync(process.argv[3], header);
if (process.argv[5]) // for the offline check: per albedo rule, the class of samplers 0..15 (A albedo, M material, L light, - none)
  fs.writeFileSync(process.argv[5], rules.map(r => `${r.hash.toString(16).toUpperCase()} ${[...Array(16).keys()].map(s => r.cls.get(s) || '-').join('')}`).join('\n') + '\n');
if (process.argv[4]) {
  const fam = new Map();
  for (const r of rows) for (const t of r.tech) (fam.get(t) || fam.set(t, []).get(t)).push(r.hash.toString(16).toUpperCase());
  fs.writeFileSync(process.argv[4], [...fam].map(([t, h]) => `${t} ${h.join(' ')}`).join('\n') + '\n');
}
console.log(`Pixel shaders in techniques ${per.size}; table rows ${rows.length} (${rows.length * 8} bytes); albedo rules ${rules.length} (${rules.length * 8} bytes)`);
for (const v of [3, 2])
  console.log(`ps_${v}_0: ${stats[v].n} shaders, albedo sampler ${stats[v].albedo}, probe sampler ${stats[v].probe}, composited ${stats[v].composited} of which albedo rule ${stats[v].rule}`);
