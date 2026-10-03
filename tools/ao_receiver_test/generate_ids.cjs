const fs = require('fs');
if (!process.argv[2]) throw Error('Usage: node generate_ids.cjs Shaders_Win32.precomp [output-header]');
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
  const end = at + 8 + size;
  if (b.readUInt32LE(end - 4) !== 0xffff) throw Error('No END ' + at);
  const code = b.subarray(begin,end);
  let hash = 2166136261;
  for (let j=0;j<code.length;j+=4) hash = Math.imul(hash ^ code.readUInt32LE(j),16777619) >>> 0;
  blobs.push({size:code.length,hash,version:code.readUInt32LE(0),code});
}
const techs = [...text.matchAll(/TECH/g)].filter(m=>m.index>36000000 && m.index<k && names.has(b.readUInt32LE(m.index+4)));
const families = new Map();
for(let i=0;i<techs.length;i++) {
  const at=techs[i].index,end=i+1<techs.length?techs[i+1].index:k;
  const name=names.get(b.readUInt32LE(at+4));
  for(const pass of text.slice(at+12,end).matchAll(/PASS/g)) {
    const off=at+12+pass.index, idx=b.readUInt32LE(off+16)-1;
    if(idx<0) continue;
    const blob=blobs[idx]; if(!blob || (blob.version>>>16)!==0xffff) throw Error('Bad PS index');
    const key=blob.size+':'+blob.hash;
    if(!families.has(key)) families.set(key,{...blob,names:new Set()});
    families.get(key).names.add(name);
  }
}
const allowed = new Set(['SimSkin','SimHair','SimEyes','SimEyelashes','SimAlphaTested','SimAlphaBlended','SimpleSim','SimRobot']);
const selected=[...families.values()].filter(x=>[...x.names].every(n=>allowed.has(n)));
selected.sort((a,b) => a.size-b.size || a.hash-b.hash);
const lines = selected.map(x => `    {${x.size}, 0x${x.hash.toString(16).padStart(8,'0').toUpperCase()}u},`);
// A hair fingerprint must belong exclusively to SimHair. Shared Sim materials
// remain body receivers: never guess hair from colour, animation or opacity.
const hair = selected.filter(x => x.names.size === 1 && x.names.has('SimHair'));
const hairLines = hair.map(x => `    {${x.size}, 0x${x.hash.toString(16).padStart(8,'0').toUpperCase()}u},`);
const header = '#pragma once\n// Exclusive Sim material PS identifiers from Steam Shaders_Win32.precomp.\n// Generated from TECH/PASS ownership; shaders shared with non-Sim techniques are excluded.\n// Includes skin, hair, eyes, eyelashes, SimpleSim and robot materials; no game bytecode.\n#include "shader_ids.h"\n\ninline constexpr ShaderId kSimReceiverPs[] = {\n' + lines.join('\n') + '\n};\n\ninline constexpr ShaderId kSimHairReceiverPs[] = {\n' + hairLines.join('\n') + '\n};\n';
if (process.argv[3]) fs.writeFileSync(process.argv[3],header);
console.log(`Blobs ${blobs.length}; exclusive Sim pixel shaders ${selected.length}; exclusive hair ${hair.length}`);
