// Turns `node precomp.cjs <precomp> census` JSON into the S0-04 markdown tables.
// Usage: node census-report.cjs <census.json>
const fs = require('fs');
const r = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const fam = new Map();
for (const x of r.rows) {
  if (!fam.has(x.tech)) fam.set(x.tech, { passes: 0, shaders: 0, lm: 0, lsp: 0, lspPs: 0, lspVsRegs: new Set(), lspUses: new Set(), coordDirect: 0, coordOther: 0, basis: 0, basisKind: new Set(), ps2: 0, ps3: 0, psMax: -1, vsMax: -1, vsBound: -1, psBound: -1, lmNames: new Set() });
  const f = fam.get(x.tech);
  f.passes += x.count; f.shaders++;
  if (x.hasLm) { f.lm += x.count; x.samp.split(',').forEach(s => { const n = s.split(':')[1]; if (/^(LightMap|RoomLightMap|terrainLightMap|CASLightMap)$/.test(n)) f.lmNames.add(n); }); }
  x.lsp.forEach(l => { if (l.stage === 'VS') { f.lsp += x.count; f.lspVsRegs.add(l.reg); l.uses.forEach(u => f.lspUses.add(u.replace(/->.*/, ''))); } else f.lspPs += x.count; });
  for (const c of x.lmCoord) { if (/^(LightMap|RoomLightMap|terrainLightMap|CASLightMap)</.test(c)) { if (/<-(in\d+|in\+mov)\./.test(c)) f.coordDirect++; else f.coordOther++; } }
  if (x.hasBasis) {
    f.basis += x.count;
    const k = x.basisW.map(w => w.split('*')[1]).join(' ');
    f.basisKind.add(/LUM/.test(k) ? 'luminance -> dominant direction' : /dp2add/.test(k) ? 'N.d per map' : 'other');
  }
  if (x.hasLm || x.hasBasis) {
    if (x.psVer === 2) f.ps2++; else if (x.psVer === 3) f.ps3++;
    f.psMax = Math.max(f.psMax, x.psMax); f.vsMax = Math.max(f.vsMax, x.vsMax); f.vsBound = Math.max(f.vsBound, x.vsBound); f.psBound = Math.max(f.psBound, x.psBound);
  }
}
const rows = [...fam].filter(([, f]) => f.lm || f.basis || f.lsp).sort();
console.log('| Family | Passes (light-map / basis) | LSP in VS (reg: reads) | LSP in PS | Light-map uv in PS | Basis weights | PS ver (2/3) | Highest PS const read | Highest VS const read (bound) |');
console.log('|---|---|---|---|---|---|---|---|---|');
for (const [n, f] of rows) {
  console.log(`| ${n} | ${f.lm} / ${f.basis} | ${f.lsp ? [...f.lspVsRegs].sort((a, b) => a - b).map(x => 'c' + x).join(',') + ': ' + [...f.lspUses].sort().join(', ') + ` (${f.lsp})` : '-'} | ${f.lspPs || 'none'} | ${f.coordDirect || f.coordOther ? (f.coordOther ? f.coordOther + ' computed, ' : '') + f.coordDirect + ' direct' : '-'} | ${f.basis ? [...f.basisKind].join('; ') : '-'} | ${f.ps2}/${f.ps3} | ${f.psMax >= 0 ? 'c' + f.psMax : '-'} | ${f.vsMax >= 0 ? 'c' + f.vsMax : '-'} (c${f.vsBound}) |`);
}
console.log('\nTOTAL passes in census', r.passes, 'shader pairs', r.rows.length);
console.log('PS-side LSP reads', rows.reduce((a, [, f]) => a + f.lspPs, 0), '; computed light-map uv', rows.reduce((a, [, f]) => a + f.coordOther, 0), '; direct', rows.reduce((a, [, f]) => a + f.coordDirect, 0));
const all = r.rows.filter(x => x.hasLm || x.hasBasis);
console.log('light-map readers: highest PS const', Math.max(...all.map(x => x.psMax)), ' highest VS const read', Math.max(...all.map(x => x.vsMax)), ' PS bound max', Math.max(...all.map(x => x.psBound)), ' VS bound max', Math.max(...all.map(x => x.vsBound)));
