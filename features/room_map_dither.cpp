// Smooth light maps: a fine dither on the room light maps' texels (see room_map_dither.h).
//
// ---- The game side (Steam 1.67.2, TS3W.exe; read with dumpbin, 13/10) ----
//   FUN_006a31d0 (batch solve of wall and floor samples), per sample: "mov edi,[ebp+8]; mov esi,[edi]; add esi,[esp+0Ch]"
//   (the sample), then at 0x006A330C "mov eax,[ebp+0Ch]; movzx edx,word [esi+22h]; imul edx,[eax+4]; movzx ecx,word
//   [esi+20h]; lea edi,[edx+ecx*4]; mov edx,[ebp+10h]; add edi,[eax]": edi = the texel's address in its map ({base, pitch}
//   at [ebp+0Ch]), (x, y) = the sample's words +0x20 / +0x22. At 0x006A34C2 "minps xmm0,[esp+70h]" (the colour clamped
//   to 1), stored at [esp+90h], then "lea eax,[esp+90h]; push eax; lea ecx,[esp+58h]; ... call 0x0060CD10" (0x006A34DB),
//   "mov ecx,[esp+54h]; add dword [esp+0Ch],30h; sub dword [esp+18h],1; mov [edi],ecx". VERIFIED.
//   0x0060CD10 thiscall(DWORD* out, const float4* c) ret 4: c x [0x00FA312C] (255.0, built once into 0x011CECB0), + 1.5 x
//   2^23 ([0x00FE774C], round to nearest), the low bytes packed A8R8G8B8 into *out. VERIFIED. A channel under -0.5 / 255
//   would wrap (the game never passes one: light is >= 0); Apex clamps to [0, 1] after its offset.
// Threads: the solve runs on the light tree thread or the render thread; the thunk reads only the caller's registers, the
// map descriptor and the room's roofless flag (under SEH) and keeps no state. ebx is the room ("mov ebx,ecx" at 0x006A31E8,
// unchanged up to the call; the solve reads [ebx+18h] at 0x006A333B). VERIFIED (capstone, Steam 1.67.2).
//
// Part of Apex Radiance. Credits: @loinyx

#include "room_map_dither.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include "apex_log.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

namespace RoomMapDither {
namespace {

// Game addresses (Steam 1.67.2 values), from GameAddr when Start runs (every build)
uintptr_t kPackCall = 0;  // 0x006A34DB: CALL 0x0060CD10 in FUN_006a31d0 (TexelPackCall)
uintptr_t kPack = 0;      // 0x0060CD10 (TexelPack)
uintptr_t kSolveFn = 0;   // 0x006A31D0 (WallSolve)
uintptr_t kTexelAddr = 0; // 0x006A330C: the texel address code (above) (TexelAddrSite)
uintptr_t kStore = 0;     // 0x006A34E0: after the call: "mov ecx,[esp+54h]; add dword [esp+0Ch],30h; sub dword [esp+18h],1; mov [edi],ecx"

std::mutex g_ctrl;
std::vector<MemPatch::PatchLocation> g_patches;
std::atomic<bool> g_running{false};
std::atomic<bool> g_on{true};
std::atomic<long long> c_texels{0}, c_skipped{0}, c_floored{0};
constexpr float kRoofedAlphaMin = 5.0f / 255.0f; // the smoothed read's "beyond a wall" mark is alpha < 4/255
std::string g_why;

using Pack_t = void(__fastcall*)(void* out, void* edx, const float* c); // = thiscall ret 4

// The texel's (x, y) in its map from its address and the map's {base, pitch} ([frame + 0xC]); POD (SEH)
bool TexelXY(uintptr_t frame, uintptr_t texel, unsigned& x, unsigned& y) {
    __try {
        const uintptr_t desc = *reinterpret_cast<const uintptr_t*>(frame + 0xC);
        if (!desc) return false;
        const uintptr_t base = *reinterpret_cast<const uintptr_t*>(desc);
        const uintptr_t pitch = *reinterpret_cast<const uint32_t*>(desc + 4);
        if (!base || texel < base || pitch < 4 || pitch > (1u << 16) || (pitch & 3)) return false;
        const uintptr_t off = texel - base;
        if (off / pitch >= 8192 || (off % pitch) & 3) return false;
        y = static_cast<unsigned>(off / pitch);
        x = static_cast<unsigned>((off % pitch) / 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The room being solved is roofed ([room+0x18] == 0, the test the solve itself makes at 0x006A333B); POD (SEH)
bool RoofedRoom(uintptr_t room) {
    __try {
        return room && *reinterpret_cast<const uint8_t*>(room + 0x18) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __stdcall DitherPack(DWORD* out, const float* colour, uintptr_t frame, uintptr_t texel, uintptr_t room) {
    alignas(16) float c[4];
    std::memcpy(c, colour, sizeof c);
    // A roofed room's texel keeps an ambient weight of at least 5/255: the smoothed read tells the texels beyond a wall
    // (room 0, alpha 0) by alpha under 4/255, and a brightly lit texel of the room itself (ambient weight near 0) was taken
    // for one, so a dimmer texel beside it (under a table, behind a plant) dropped its light: a dark square one texel wide.
    // The added light is 5/255 of the room's ambient colour on the brightest texels, under half an 8-bit level.
    if (std::isfinite(c[3]) && c[3] < kRoofedAlphaMin && RoofedRoom(room)) {
        c[3] = kRoofedAlphaMin;
        c_floored.fetch_add(1, std::memory_order_relaxed);
    }
    unsigned x = 0, y = 0;
    if (g_on.load(std::memory_order_relaxed) && TexelXY(frame, texel, x, y)) {
        const float d = Offset(x, y) / 255.0f;
        for (int i = 0; i < 3; i++) {
            const float v = c[i] + d;
            c[i] = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : c[i];
        }
        c_texels.fetch_add(1, std::memory_order_relaxed);
    } else
        c_skipped.fetch_add(1, std::memory_order_relaxed);
    reinterpret_cast<Pack_t>(kPack)(out, nullptr, c);
}

// thiscall(ecx = out, [esp+4] = colour) ret 4, with the caller's ebp (its frame), edi (the texel) and ebx (the room: the
// solve's this, kept in ebx from 0x006A31E8 to the call) as they are at the call
__declspec(naked) void PackThunk() {
    __asm {
        mov eax, [esp + 4]
        push ebx
        push edi
        push ebp
        push eax
        push ecx
        call DitherPack
        ret 4
    }
}

bool CodeIs(uintptr_t at, std::initializer_list<BYTE> want) {
    BYTE got[32] = {};
    if (want.size() > sizeof got || !MemPatch::ReadBytes(at, got, want.size())) return false;
    return std::equal(want.begin(), want.end(), got);
}

} // namespace

bool Start(std::string& error) {
    std::lock_guard<std::mutex> lk(g_ctrl);
    if (g_running.load()) return true;
    using GameAddr::Id;
    if (std::string missing; !GameAddr::Have({Id::TexelPackCall, Id::TexelPack, Id::WallSolve, Id::TexelAddrSite, Id::PackConst255, Id::PackMagic}, &missing)) {
        g_why = error = "Smooth light maps (dither): " + GameAddr::NotAvailable(missing);
        return false;
    }
    kPackCall = GameAddr::Get(Id::TexelPackCall);
    kPack = GameAddr::Get(Id::TexelPack);
    kSolveFn = GameAddr::Get(Id::WallSolve);
    kTexelAddr = GameAddr::Get(Id::TexelAddrSite);
    kStore = kPackCall + 5;
    const uintptr_t k255At = GameAddr::Get(Id::PackConst255), magicAt = GameAddr::Get(Id::PackMagic);
    const BYTE* k255b = reinterpret_cast<const BYTE*>(&k255At);
    const BYTE* magicb = reinterpret_cast<const BYTE*>(&magicAt);
    int32_t rel = 0;
    const bool callOk = CodeIs(kPackCall, {0xE8}) && MemPatch::ReadBytes(kPackCall + 1, &rel, 4) && kPackCall + 5 + rel == kPack;
    const bool around = CodeIs(kPackCall - 0x19, {0x0F, 0x5D, 0x44, 0x24, 0x70}) &&                                      // minps xmm0,[esp+70h]
                        CodeIs(kStore, {0x8B, 0x4C, 0x24, 0x54, 0x83, 0x44, 0x24, 0x0C, 0x30, 0x83, 0x6C, 0x24, 0x18, 0x01, 0x89, 0x0F}) &&
                        CodeIs(kTexelAddr, {0x8B, 0x45, 0x0C, 0x0F, 0xB7, 0x56, 0x22, 0x0F, 0xAF, 0x50, 0x04, 0x0F, 0xB7, 0x4E, 0x20, 0x8D, 0x3C, 0x8A, 0x8B,
                                            0x55, 0x10, 0x03, 0x38}) &&
                        CodeIs(kSolveFn, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0}) &&
                        CodeIs(kSolveFn + 0x18, {0x8B, 0xD9}); // mov ebx,ecx: the room the thunk reads
    const bool packOk = CodeIs(kPack, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0xB8, 0x01, 0x00, 0x00, 0x00}) && CodeIs(kPack + 0x16, {0xF3, 0x0F, 0x10, 0x05, k255b[0], k255b[1], k255b[2], k255b[3]}) &&
                        CodeIs(kPack + 0x5A, {0xF3, 0x0F, 0x10, 0x05, magicb[0], magicb[1], magicb[2], magicb[3]}); // Steam: 0x00FA312C, 0x00FE774C
    float k255 = 0.0f, magic = 0.0f;
    const bool constsOk = MemPatch::ReadBytes(k255At, &k255, 4) && MemPatch::ReadBytes(magicAt, &magic, 4) && k255 == 255.0f && magic == 12582912.0f;
    if (!callOk || !around || !packOk || !constsOk) {
        g_why = error = "Smooth light maps (dither): the game code differs";
        return false;
    }
    const DWORD orig = static_cast<DWORD>(rel);
    const DWORD to = static_cast<DWORD>(reinterpret_cast<uintptr_t>(&PackThunk) - (kPackCall + 5));
    if (!MemPatch::WriteDWORD(kPackCall + 1, to, &g_patches, &orig)) {
        g_why = error = "Smooth light maps (dither): could not patch the texel pack";
        return false;
    }
    g_running.store(true);
    g_why.clear();
    LOG_INFO(std::format("[RoomMapDither] On: the room maps' texel pack {:#010x} -> {:#010x} goes through Apex", kPackCall, kPack));
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> lk(g_ctrl);
    if (!g_running.load()) return;
    MemPatch::RestoreAll(g_patches);
    g_patches.clear();
    g_running.store(false);
    LOG_INFO("[RoomMapDither] Off: the game packs the room maps' texels alone");
}

bool Running() { return g_running.load(); }
void SetEnabled(bool on) { g_on.store(on, std::memory_order_relaxed); } // the relight is LevelLightShare::SetSoftLightCut's (the same option)
bool Enabled() { return g_on.load(std::memory_order_relaxed); }

std::string Status() {
    if (!g_running.load()) return g_why.empty() ? "dither off" : g_why;
    return std::format("dither {}: {} texels, {} left exact; {} roofed texels kept at alpha 5/255", g_on.load() ? "on" : "switched off", c_texels.load(), c_skipped.load(), c_floored.load());
}

} // namespace RoomMapDither
