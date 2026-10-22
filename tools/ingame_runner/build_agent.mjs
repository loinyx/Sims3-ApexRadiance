// Builds the in-game test agent (agent/*.cs) into ApexTestAgent.package for run.ps1.
// Adapted from sims3-modernization tools/harness/build-agent.ts, tools/dbpf.ts and tools/extract-assemblies.ts (MIT, see
// LICENSE-sims3-modernization); the package writer ports Sims3-MCP tools/dbpf.py (MIT, see agent/LICENSE-Sims3-MCP).
//
//   node tools/ingame_runner/build_agent.mjs -GameBin "<Game>\Bin"   extract the game's script assemblies, then build
//   node tools/ingame_runner/build_agent.mjs -Refs <folder>          build against assemblies extracted earlier
//   node tools/ingame_runner/build_agent.mjs -SelfTest               package writer and reader round trip, no game files
//
// The game's packages are only read. The extracted assemblies stay under the ignored outputs/ingame/agent/refs and are
// never committed. The compiler is the .NET Framework C# compiler that ships with Windows (csc.exe, C# 5).
import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, '..', '..');
const OUT = join(REPO, 'outputs', 'ingame', 'agent');
const CSC = join(process.env.WINDIR || 'C:\\Windows', 'Microsoft.NET', 'Framework', 'v4.0.30319', 'csc.exe');
const REFS = ['mscorlib', 'System', 'System.Xml', 'ScriptCore', 'SimIFace', 'Sims3Metadata', 'Sims3GameplaySystems',
              'Sims3GameplayObjects', 'Sims3StoreObjects', 'UI'];
const PACKAGES = ['gameplay.package', 'scripts.package', 'simcore.package', join('UI', 'UI.package')];
const ASSEMBLY = 'ApexTestAgent';
const TYPE_S3SA = 0x073faa07, TYPE_XML = 0x0333406c;
// Tuning XML for the instantiator class: loading it runs the static constructor that starts the agent.
const TUNING_XML = '<?xml version="1.0" encoding="utf-8"?>\n<base>\n  <Current_Tuning>\n    <kInstantiator value="True" />\n  </Current_Tuning>\n</base>\n';

// FNV-1 64-bit of the lower-case name: the instance id the game derives from a resource name.
export function fnv64(text) {
  let h = 0xcbf29ce484222325n;
  for (const b of Buffer.from(text.toLowerCase(), 'ascii')) { h = (h * 0x100000001b3n) & 0xffffffffffffffffn; h ^= BigInt(b); }
  return h;
}

// Version-1 S3SA with an all-zero key table: every XOR key is 0, so the blocks are the padded assembly.
export function s3saEncode(assembly) {
  const count = Math.ceil(assembly.length / 512);
  const head = Buffer.alloc(1 + 4 + 64 + 2 + count * 8);
  head[0] = 1;
  head.writeUInt32LE(0x2bc4f79f, 1);
  head.writeUInt16LE(count, 69);
  const body = Buffer.alloc(count * 512);
  assembly.copy(body);
  return Buffer.concat([head, body]);
}

// DBPF 2.0 package, uncompressed resources.
export function writePackage(resources) {
  const HEADER = 96, offsets = [];
  let pos = HEADER;
  for (const r of resources) { offsets.push(pos); pos += r.data.length; }
  const index = Buffer.alloc(4 + resources.length * 32); // flags 0: no constant fields
  resources.forEach((r, i) => {
    const o = 4 + i * 32;
    index.writeUInt32LE(r.type, o);
    index.writeUInt32LE(r.group, o + 4);
    index.writeUInt32LE(Number(r.instance >> 32n), o + 8);
    index.writeUInt32LE(Number(r.instance & 0xffffffffn), o + 12);
    index.writeUInt32LE(offsets[i], o + 16);
    index.writeUInt32LE((r.data.length | 0x80000000) >>> 0, o + 20);
    index.writeUInt32LE(r.data.length, o + 24);
    index.writeUInt16LE(0, o + 28); // not compressed
    index.writeUInt16LE(1, o + 30); // committed
  });
  const header = Buffer.alloc(HEADER);
  header.write('DBPF', 0, 'latin1');
  header.writeUInt32LE(2, 4);
  header.writeUInt32LE(resources.length, 0x24);
  header.writeUInt32LE(index.length, 0x2c);
  header.writeUInt32LE(3, 0x3c);
  header.writeUInt32LE(pos, 0x40);
  return Buffer.concat([header, ...resources.map(r => r.data), index]);
}

export function readIndex(pkg) {
  if (pkg.toString('latin1', 0, 4) !== 'DBPF') throw new Error('not a DBPF package');
  const count = pkg.readUInt32LE(36);
  let p = pkg.readUInt32LE(64);
  const flags = pkg.readUInt32LE(p); p += 4;
  const next = () => { p += 4; return pkg.readUInt32LE(p - 4); };
  const cType = flags & 1 ? next() : 0, cGroup = flags & 2 ? next() : 0, cHi = flags & 4 ? next() : 0;
  const entries = [];
  for (let i = 0; i < count; i++) {
    const type = flags & 1 ? cType : next(), group = flags & 2 ? cGroup : next(), hi = flags & 4 ? cHi : next();
    const lo = pkg.readUInt32LE(p), offset = pkg.readUInt32LE(p + 4), fileSize = pkg.readUInt32LE(p + 8) & 0x7fffffff;
    const compressed = pkg.readUInt16LE(p + 16);
    p += 20;
    entries.push({ type, group, inst: (BigInt(hi) << 32n) | BigInt(lo), offset, fileSize, compressed });
  }
  return entries;
}

// RefPack (EA QFS) decompression as used by DBPF 2.0 resources.
function refpack(src) {
  let p = 0;
  const flags = src[p++];
  if (src[p++] !== 0xfb) throw new Error('not RefPack');
  const sizeBytes = flags & 0x80 ? 4 : 3;
  if (flags & 0x01) p += sizeBytes;
  let size = 0;
  for (let i = 0; i < sizeBytes; i++) size = size * 256 + src[p++];
  const out = Buffer.alloc(size);
  let o = 0;
  for (;;) {
    const b0 = src[p++];
    let plain, copy = 0, off = 0;
    if (b0 < 0x80) { const b1 = src[p++]; plain = b0 & 3; copy = ((b0 & 0x1c) >> 2) + 3; off = ((b0 & 0x60) << 3) + b1 + 1; }
    else if (b0 < 0xc0) { const b1 = src[p++], b2 = src[p++]; plain = b1 >> 6; copy = (b0 & 0x3f) + 4; off = ((b1 & 0x3f) << 8) + b2 + 1; }
    else if (b0 < 0xe0) {
      const b1 = src[p++], b2 = src[p++], b3 = src[p++];
      plain = b0 & 3; copy = ((b0 & 0x0c) << 6) + b3 + 5; off = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1;
    } else if (b0 < 0xfc) plain = ((b0 & 0x1f) << 2) + 4;
    else { plain = b0 & 3; src.copy(out, o, p, p + plain); o += plain; break; }
    src.copy(out, o, p, p + plain); o += plain; p += plain;
    for (let i = 0; i < copy; i++, o++) out[o] = out[o - off]; // byte-wise: ranges may overlap
  }
  if (o !== size) throw new Error(`RefPack size mismatch ${o} != ${size}`);
  return out;
}

// S3SA body: header + XOR-chained 512-byte blocks (after s3pi ScriptResource).
export function decryptS3SA(body) {
  let p = 0;
  const version = body[p++];
  if (version > 1) p += body.readInt32LE(p) * 2 + 4;
  p += 4 + 64;
  const count = body.readUInt16LE(p); p += 2;
  const table = body.subarray(p, p + count * 8); p += count * 8;
  const data = body.subarray(p, p + count * 512);
  let sum = 0n;
  for (let i = 0; i < table.length; i += 8) sum = (sum + table.readBigUInt64LE(i)) & 0xffffffffffffffffn;
  let seed = Number(BigInt(table.length - 1) & sum);
  const out = Buffer.alloc(count * 512);
  for (let i = 0, blk = 0, r = 0; i < table.length; i += 8, blk++) {
    if (table[i] & 1) continue;
    for (let j = 0; j < 512; j++, r++) { const v = data[r]; out[blk * 512 + j] = v ^ table[seed]; seed = (seed + v) % table.length; }
  }
  return out;
}

function peImage(body) {
  for (let i = body.indexOf('MZ'); i >= 0; i = body.indexOf('MZ', i + 1)) {
    if (i + 0x40 > body.length) break;
    const pe = body.readUInt32LE(i + 0x3c);
    if (i + pe + 4 <= body.length && body.toString('latin1', i + pe, i + pe + 4) === 'PE\0\0') return body.subarray(i);
  }
  return null;
}

// Module name from the .NET metadata (Module table row 0), such as "UI.dll".
function moduleName(img) {
  const pe = img.readUInt32LE(0x3c), nSec = img.readUInt16LE(pe + 6), opt = pe + 24, secs = opt + img.readUInt16LE(pe + 20);
  const rva2off = rva => {
    for (let i = 0; i < nSec; i++) {
      const s = secs + i * 40, va = img.readUInt32LE(s + 12), size = img.readUInt32LE(s + 8);
      if (rva >= va && rva < va + size) return rva - va + img.readUInt32LE(s + 20);
    }
    throw new Error(`rva ${rva} outside sections`);
  };
  const cliRva = img.readUInt32LE(opt + 96 + 14 * 8);
  if (!cliRva) return null;
  const md = rva2off(img.readUInt32LE(rva2off(cliRva) + 8));
  let p = md + 16 + img.readUInt32LE(md + 12) + 2;
  const nStreams = img.readUInt16LE(p); p += 2;
  let tables = 0, strings = 0;
  for (let i = 0; i < nStreams; i++) {
    const off = img.readUInt32LE(p), end = img.indexOf(0, p + 8), name = img.toString('latin1', p + 8, end);
    if (name === '#~') tables = md + off;
    if (name === '#Strings') strings = md + off;
    p = p + 8 + ((end - (p + 8) + 4) & ~3);
  }
  const wide = img[tables + 6] & 1, valid = img.readBigUInt64LE(tables + 8);
  const rows = tables + 24 + 4 * [...valid.toString(2)].filter(b => b === '1').length;
  const nameIdx = wide ? img.readUInt32LE(rows + 2) : img.readUInt16LE(rows + 2);
  return img.toString('utf8', strings + nameIdx, img.indexOf(0, strings + nameIdx));
}

function extract(gameBin, refs) {
  mkdirSync(refs, { recursive: true });
  for (const name of PACKAGES) {
    const file = join(gameBin, name);
    if (!existsSync(file)) continue;
    const pkg = readFileSync(file);
    for (const e of readIndex(pkg).filter(x => x.type === TYPE_S3SA)) {
      let body = pkg.subarray(e.offset, e.offset + e.fileSize);
      if (e.compressed === 0xffff) body = refpack(body);
      const img = peImage(body) ?? peImage(decryptS3SA(body));
      const dll = img && moduleName(img);
      if (dll && REFS.includes(dll.replace(/\.dll$/i, ''))) writeFileSync(join(refs, dll), img);
    }
  }
}

function selfTest() {
  const asm = Buffer.from('MZ test assembly '.repeat(70));
  const pkg = writePackage([{ type: TYPE_S3SA, group: 0, instance: fnv64('Test'), data: s3saEncode(asm) }]);
  const [e] = readIndex(pkg);
  assert.equal(e.type, TYPE_S3SA);
  assert.equal(e.inst, fnv64('Test'));
  assert.ok(decryptS3SA(pkg.subarray(e.offset, e.offset + e.fileSize)).subarray(0, asm.length).equals(asm), 'S3SA round trip');
  assert.equal(fnv64(''), 0xcbf29ce484222325n);
  console.log('build_agent self-test: 4 checks, 0 failures');
}

const args = process.argv.slice(2);
const opt = name => { const i = args.findIndex(a => a.toLowerCase() === name.toLowerCase()); return i >= 0 ? args[i + 1] : undefined; };
if (args.some(a => a.toLowerCase() === '-selftest')) selfTest();
else {
  const refs = opt('-Refs') ?? join(OUT, 'refs');
  if (opt('-GameBin')) extract(opt('-GameBin'), refs);
  const missing = REFS.filter(r => !existsSync(join(refs, `${r}.dll`)));
  if (missing.length) throw new Error(`Missing game assemblies in ${refs}: ${missing.join(', ')}. Pass -GameBin "<Game>\\Bin".`);
  if (!existsSync(CSC)) throw new Error(`C# compiler not found: ${CSC}`);
  mkdirSync(OUT, { recursive: true });
  const dll = join(OUT, `${ASSEMBLY}.dll`);
  const sources = readdirSync(join(HERE, 'agent')).filter(f => f.endsWith('.cs')).map(f => join(HERE, 'agent', f));
  execFileSync(CSC, ['/nologo', '/nostdlib+', '/noconfig', '/target:library', '/optimize+', '/langversion:5', '/warn:4',
    '/nowarn:414,169,649', `/out:${dll}`, ...REFS.map(r => `/reference:${join(refs, `${r}.dll`)}`), ...sources], { stdio: 'inherit' });
  const pkg = writePackage([
    { type: TYPE_S3SA, group: 0, instance: fnv64(ASSEMBLY), data: s3saEncode(readFileSync(dll)) },
    { type: TYPE_XML, group: 0, instance: fnv64(`${ASSEMBLY}.Instantiator`), data: Buffer.from(TUNING_XML, 'utf8') },
  ]);
  const out = join(OUT, `${ASSEMBLY}.package`);
  writeFileSync(out, pkg);
  console.log(`${basename(out)}: ${pkg.length} bytes in ${OUT}`);
}
