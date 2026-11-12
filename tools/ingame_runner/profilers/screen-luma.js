// What the game draws, read inside the game (independent of the mod and of the window: a borderless window or the
// DXVK swap chain gives a window capture black frames). Hooks the game's Device_EndSceneAndPresent (0x611760, Steam
// 1.67.2: push 0 x3, call 0x611680; thiscall, the device is [this+0xC] as BeginScene 0x611620 reads it), so the picture
// is the finished scene and the game's UI, before the mod's Present-time overlay. Every `everyMs` (default 50) it copies
// the back buffer into a 160x90 render target (StretchRect), reads it back (GetRenderTargetData: a short GPU wait) and
// writes:
//   luma.csv  qpcMs,clock,frame,c0..c143  (the mean luma of a 16 x 9 grid, for flash_scan.py)
//   NNNNNN.bmp the 160x90 picture
// init(stage, {out, everyMs}) / close() as the other profilers (attach.py).
'use strict';

const W = 160, H = 90, GX = 16, GY = 9;
let out = null, everyMs = 50, csv = null, frame = 0, last = 0, rt = null, sys = null, dev0 = null, busy = false, closed = false;

const k32 = Process.getModuleByName('kernel32.dll');
const QPC = new NativeFunction(k32.getExportByName('QueryPerformanceCounter'), 'int', ['pointer']);
const QPF = new NativeFunction(k32.getExportByName('QueryPerformanceFrequency'), 'int', ['pointer']);
const qbuf = Memory.alloc(8);
QPF(qbuf);
const qpf = qbuf.readU64().toNumber();
function qpcMs() { QPC(qbuf); return qbuf.readU64().toNumber() * 1000 / qpf; }
function clock() {
    const d = new Date(), p = (n, w) => String(n).padStart(w, '0');
    return p(d.getHours(), 2) + ':' + p(d.getMinutes(), 2) + ':' + p(d.getSeconds(), 2) + '.' + p(d.getMilliseconds(), 3);
}

// COM call: obj's vtable slot i, stdcall with obj first
function vcall(obj, i, ret, args) {
    const fn = obj.readPointer().add(i * Process.pointerSize).readPointer();
    return new NativeFunction(fn, ret, ['pointer'].concat(args), 'stdcall');
}
function release(obj) { if (obj && !obj.isNull()) vcall(obj, 2, 'uint32', [])(obj); }

function bmp(path, px) { // px: BGRA 160x90 top-down -> 24-bit bottom-up BMP
    const row = W * 3, size = 54 + row * H, b = new Uint8Array(size), v = new DataView(b.buffer);
    b[0] = 0x42; b[1] = 0x4D; v.setUint32(2, size, true); v.setUint32(10, 54, true); v.setUint32(14, 40, true);
    v.setInt32(18, W, true); v.setInt32(22, H, true); v.setUint16(26, 1, true); v.setUint16(28, 24, true); v.setUint32(34, row * H, true);
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
        const s = (y * W + x) * 4, d = 54 + (H - 1 - y) * row + x * 3;
        b[d] = px[s]; b[d + 1] = px[s + 1]; b[d + 2] = px[s + 2];
    }
    const f = new File(path, 'wb'); f.write(b.buffer); f.close();
}

function sample(dev) {
    if (dev0 && !dev.equals(dev0)) { release(rt); release(sys); rt = sys = null; } // another device (a reset made a new one)
    dev0 = dev;
    const surfOut = Memory.alloc(4);
    if (!rt) {
        if (vcall(dev, 28, 'int32', ['uint32', 'uint32', 'uint32', 'uint32', 'uint32', 'int', 'pointer', 'pointer'])(dev, W, H, 21, 0, 0, 0, surfOut, NULL) < 0) return; // A8R8G8B8 render target
        rt = surfOut.readPointer();
        if (vcall(dev, 36, 'int32', ['uint32', 'uint32', 'uint32', 'uint32', 'pointer', 'pointer'])(dev, W, H, 21, 2, surfOut, NULL) < 0) return; // SYSTEMMEM
        sys = surfOut.readPointer();
    }
    if (vcall(dev, 18, 'int32', ['uint32', 'uint32', 'uint32', 'pointer'])(dev, 0, 0, 0, surfOut) < 0) return; // GetBackBuffer
    const bb = surfOut.readPointer();
    const ok = vcall(dev, 34, 'int32', ['pointer', 'pointer', 'pointer', 'pointer', 'uint32'])(dev, bb, NULL, rt, NULL, 2) >= 0 && // StretchRect, linear
               vcall(dev, 32, 'int32', ['pointer', 'pointer'])(dev, rt, sys) >= 0;                                         // GetRenderTargetData
    release(bb);
    if (!ok) return;
    const lr = Memory.alloc(8); // D3DLOCKED_RECT: Pitch, pBits
    if (vcall(sys, 13, 'int32', ['pointer', 'pointer', 'uint32'])(sys, lr, NULL, 0x10) < 0) return; // LockRect, READONLY
    const pitch = lr.readS32(), bits = lr.add(4).readPointer();
    const px = new Uint8Array(W * H * 4);
    for (let y = 0; y < H; y++) px.set(new Uint8Array(bits.add(y * pitch).readByteArray(W * 4)), y * W * 4);
    vcall(sys, 14, 'int32', [])(sys);
    const cells = new Array(GX * GY).fill(0), n = new Array(GX * GY).fill(0);
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
        const i = (y * W + x) * 4, c = Math.floor(y * GY / H) * GX + Math.floor(x * GX / W);
        cells[c] += 0.0722 * px[i] + 0.7152 * px[i + 1] + 0.2126 * px[i + 2]; n[c]++;
    }
    csv.write(qpcMs().toFixed(1) + ',' + clock() + ',' + frame + ',' + cells.map((v, c) => (v / n[c]).toFixed(1)).join(',') + '\n');
    bmp(out + '/' + String(frame).padStart(6, '0') + '.bmp', px);
    frame++;
}

rpc.exports = {
    init(stage, params) {
        out = params.out; everyMs = params.everyMs || 50;
        csv = new File(out + '/luma.csv', 'w');
        csv.write('qpcMs,clock,frame,' + Array.from({ length: GX * GY }, (_, i) => 'c' + i).join(',') + '\n');
        const site = ptr(0x611760), want = '6a006a006a00e8';
        const have = Array.from(new Uint8Array(site.readByteArray(7))).map(b => b.toString(16).padStart(2, '0')).join('');
        if (have !== want) throw new Error('0x611760 is ' + have + ', not ' + want + ': not Steam 1.67.2');
        Interceptor.attach(site, {
            onEnter() {
                if (closed || busy) return;
                const t = qpcMs();
                if (t - last < everyMs) return;
                last = t; busy = true;
                try { sample(this.context.ecx.add(0xC).readPointer()); } catch (e) { send('[screen-luma] ' + e); } finally { busy = false; }
            }
        });
        send('[screen-luma] hooked 0x611760, a sample every ' + everyMs + ' ms -> ' + out);
    },
    close() { closed = true; if (csv) { csv.flush(); csv.close(); csv = null; } send('[screen-luma] closed after ' + frame + ' samples'); }
};
