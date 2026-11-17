// G-buffer shader table (docs/features/realtime-gi/research/14-gbuffer-motion-vectors.md §3.2).
// Usage: node generate.cjs <Shaders_Win32.precomp> [output-header] [family,family,...]
// Read-only over the precomp; writes only the header when a path is given. No game bytecode is written: per ps_3_0
// pixel shader its hash, the sampler a pass names as the albedo texture, the sampler of the diffuse light probe, a
// material id and a motion class. The sampler names exist only in the pass records, not in the bytecode.
const fs = require('fs');
if (!process.argv[2]) throw Error('Usage: node generate.cjs Shaders_Win32.precomp [output-header] [families]');
const FAMILIES = (process.argv[4] || 'SimSkin,Day,TerrainHigh,Phong,Landmark').split(',');
if (FAMILIES.length > 6) throw Error('At most 6 families fit the table byte');
const ALBEDO = new Set(['DiffuseMap', 'colorMap', 'RoadTexture']);
const PROBE = new Set(['DiffuseLightProbe', 'ExteriorDiffuseLightProbe']);
// Families whose albedo is composited from several textures: no albedo sampler until they get a family rule
const NO_ALBEDO = ['SimSkin', 'TerrainHigh'];
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
// texture reads of a sampler: [count, the coordinate is a temp] (texld, texldl, texldd)
function reads(code, sampler) {
  let n = 0, temp = false;
  for (let i = 4; i < code.length;) {
    const t = code.readUInt32LE(i), op = t & 0xffff;
    if (op === 0xffff) break;
    if (op === 0xfffe) { i += 4 + ((t >>> 16) & 0x7fff) * 4; continue; }
    const len = (t >>> 24) & 15;
    if (op === 0x42 || op === 0x5f || op === 0x5d) {
      const c = code.readUInt32LE(i + 8), s = code.readUInt32LE(i + 12);
      if ((s & 0x7ff) === sampler) { n++; temp = (((c >>> 28) & 7) | ((c >>> 8) & 0x18)) === 0; }
    }
    i += 4 + len * 4;
  }
  return [n, temp];
}
// the one sampler every pass binds one of these names to, read exactly once; -1 otherwise
function single(e, wanted) {
  const s = new Set();
  for (const [n, set] of e.bind) if (wanted.has(n)) for (const x of set) s.add(x);
  if (s.size !== 1) return [-1, false];
  const [sampler] = s, [n, temp] = reads(e.blob.code, sampler);
  return n === 1 ? [sampler, temp] : [-1, false];
}
const rows = [], stats = FAMILIES.map(() => ({ n: 0, albedo: 0, probe: 0, probeTemp: 0 }));
for (const e of per.values()) {
  if (e.blob.version !== 0xffff0300) continue;
  let fam = 0;
  FAMILIES.forEach((f, i) => { if (e.tech.has(f)) fam |= 1 << i; });
  if (!fam) continue;
  const id = 1 + Math.min(...[...e.tech].map(t => MATERIALS.findIndex(m => m[2].test(t))));
  const cls = [...e.tech].some(t => MOVER.test(t)) ? 3 : 0;
  const [albedo] = NO_ALBEDO.some(f => e.tech.has(f)) ? [-1] : single(e, ALBEDO), [probe, temp] = single(e, PROBE);
  FAMILIES.forEach((f, i) => {
    if (!((fam >>> i) & 1)) return;
    stats[i].n++; stats[i].albedo += albedo >= 0; stats[i].probe += probe >= 0; stats[i].probeTemp += probe >= 0 && temp;
  });
  rows.push({ hash: e.blob.hash, albedo: albedo < 0 ? 255 : albedo, probe: probe < 0 ? 255 : probe, id, cls, fam });
}
rows.sort((x, y) => x.hash - y.hash);
const hex = (v, n) => '0x' + v.toString(16).toUpperCase().padStart(n, '0');
const header = `#pragma once
// G-buffer shader table (tools/gbuffer_ids/generate.cjs, Steam Shaders_Win32.precomp): per ps_3_0 pixel shader of the
// families below, the sampler a pass names DiffuseMap / colorMap / RoadTexture (albedo, read by exactly one texld),
// the sampler of the diffuse light probe (read once: its coordinate is the world shading normal), a material id and
// the motion class (3 = moves by itself, no vector). 255 = no such sampler. Identifiers only, no game bytecode.
// Generated; do not edit. Sorted by hash (FNV-1a 32 over the bytecode's DWORDs, ShaderHash in shader_ids.h).
#include <algorithm>
#include <cstdint>
#include <iterator>

struct GBufferShaderId {
    uint32_t hash;
    uint8_t albedo, probe, material;
    uint8_t classAndFamily; // bits 0-1 the motion class, bits 2-7 the families (kGBufferFamilies order) it is used by
};

inline constexpr const char* kGBufferFamilies[] = {${FAMILIES.map(f => `"${f}"`).join(', ')}};
// material ids 1.. (0 = nothing written, 255 = albedo unknown) and the roughness written for each
inline constexpr const char* kGBufferMaterials[] = {"None", ${MATERIALS.map(m => `"${m[0]}"`).join(', ')}};
inline constexpr float kGBufferRoughness[] = {0.0f, ${MATERIALS.map(m => m[1].toFixed(1) + 'f').join(', ')}};

inline constexpr GBufferShaderId kGBufferShaders[] = {
${rows.map(r => `    {${hex(r.hash, 8)}u, ${r.albedo}, ${r.probe}, ${r.id}, ${hex(r.cls | (r.fam << 2), 2)}},`).join('\n')}
};

// the row of a shader, or null (a shader of another family, another game version or a mod)
inline const GBufferShaderId* FindGBufferShader(uint32_t hash) {
    const auto it = std::lower_bound(std::begin(kGBufferShaders), std::end(kGBufferShaders), hash, [](const GBufferShaderId& r, uint32_t h) { return r.hash < h; });
    return it != std::end(kGBufferShaders) && it->hash == hash ? &*it : nullptr;
}
`;
if (process.argv[3]) fs.writeFileSync(process.argv[3], header);
console.log(`Pixel shaders in techniques ${per.size}; table rows ${rows.length} (${rows.length * 8} bytes)`);
FAMILIES.forEach((f, i) => console.log(`${f}: ${stats[i].n} shaders, albedo sampler ${stats[i].albedo}, probe sampler ${stats[i].probe} (temp coordinate ${stats[i].probeTemp})`));
