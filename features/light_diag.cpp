// Light Diagnostics (part of Night Lighting)
#include "light_diag.h"
#include "hotkeys.h"
// One button (or its shortcut, F8 with the F-key set) writes the lighting snapshot,
// Captures\<date time> Lighting snapshot\Lighting snapshot.txt (features/captures.h), with:
//  - every light in the world (FUN_006acf70 enumerator): type, lot id, room, flags, on/off, colour, intensity, position
//  - every loaded lot lighting manager (light update tree walk) and every room in it (room hash at mgr+0x230):
//    solve state, LOD class, outdoor flag and the lights in the room's light list (+0xC8..+0xCC)
// Read-only. Runs on the render thread from Present.

#include "patch_base.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include "apex_log.h"
#include "d3d9_hooks.h"
#include "apex_paths.h"
#include "level_light_share.h"
#include "captures.h"
#include "ui/i18n.h"
#include "imgui.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// Addresses: the fixed Steam 1.67.2 ones, or found by signature on other builds (game_addresses.h); set by Init.
uintptr_t kRootGetter = 0; // 0x006E97B0: A1 <imm32> 85 C0 75 01 C3 8B 80 C0 01 00 00
constexpr BYTE kRootGetterTail[] = {0x85, 0xC0, 0x75, 0x01, 0xC3, 0x8B, 0x80, 0xC0, 0x01, 0x00, 0x00};
uintptr_t kEnumLights = 0; // 0x006ACF70: stdcall(visitor*), visitor vtable[0] = thiscall(visitor, Light*) ret 4
constexpr BYTE kEnumLightsBytes[] = {0xE8, 0x2B, 0x36, 0x00, 0x00, 0x8B, 0x4C, 0x24, 0x04, 0x51, 0x68, 0x40, 0xCF, 0x6A, 0x00}; // Steam (checked on Steam)
using EnumLights_t = void(__stdcall*)(void* visitor);

uintptr_t g_rootPtrAddr = 0;
std::atomic<bool> g_requested{false};
bool g_keyWasDown = false;
std::string g_status = "Ready";
std::vector<uintptr_t> g_lights;


void __fastcall VisitLight(void* /*self*/, void* /*edx*/, uintptr_t light) {
    if (g_lights.size() < 200000) g_lights.push_back(light);
}
void* g_visitorVtbl[1] = {reinterpret_cast<void*>(&VisitLight)};
struct Visitor {
    void** vtbl;
} g_visitor{g_visitorVtbl};

bool SafeCopy(void* dst, uintptr_t src, size_t n) {
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <typename T> T Rd(uintptr_t a, T def = T{}) {
    T v{};
    return SafeCopy(&v, a, sizeof(T)) ? v : def;
}

bool EnumerateLights() {
    g_lights.clear();
    __try {
        reinterpret_cast<EnumLights_t>(kEnumLights)(&g_visitor);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string Vec4(uintptr_t a) {
    float v[4] = {};
    if (!SafeCopy(v, a, sizeof(v))) return "?";
    return std::format("({:.2f} {:.2f} {:.2f} {:.2f})", v[0], v[1], v[2], v[3]);
}

std::string LightLine(uintptr_t L) {
    const uint32_t vt = Rd<uint32_t>(L);
    const int type = Rd<int>(L + 0xB0, -1);
    const uint32_t lotLo = Rd<uint32_t>(L + 0xC0), lotHi = Rd<uint32_t>(L + 0xC4);
    const int room = Rd<int>(L + 0x08, -1);
    const BYTE f = Rd<BYTE>(L + 0x100);
    float pos[4] = {};
    SafeCopy(pos, L + 0x120, sizeof(pos));
    const float radius = Rd<float>(L + 0x130);
    return std::format("L{:08X} vt={:08X} tipo={} lote={:08X}{:08X} comodo={} d0={} flags={:02X}[{}{}{}{}{}] pos=({:.1f} {:.1f} {:.1f}) raio={:.2f} intens={} cor={} efetiva={}", L, vt, type,
        lotHi, lotLo, room, Rd<int>(L + 0xD0), f, (f & 1) ? "viva " : "", (f & 2) ? "celulas " : "", (f & 4) ? "comodo-conhecido " : "", (f & 0x20) ? "ACESA " : "apagada ",
        (f & 0x40) ? "habilitada" : "desabilitada", pos[0], pos[1], pos[2], radius, Vec4(L + 0x10), Vec4(L + 0xF0), Vec4(L + 0xE0));
}

std::string Floats(uintptr_t a, int n) {
    std::string s = "(";
    for (int i = 0; i < n; i++) s += std::format("{}{:.4g}", i ? " " : "", Rd<float>(a + i * 4));
    return s + ")";
}

// Step 3, increment 0 (PASSO3-PLANO.md section 6): the values the per-pixel lamp term must copy from the game's own
// wall/floor light solve. Read-only; nothing changes on screen.
std::string PixelLampDiag(const std::unordered_map<uintptr_t, size_t>& index) {
    std::string o = "\n==== LUZ POR PIXEL (passo 3, incremento 0) ====\n";
    if (GameAddr::IsFixed()) { // Steam 1.67.2 globals (not looked up on other builds)
        o += std::format("k1 [0x11D0A60]={:.6g} k2 [0x11D0A68]={:.6g} Cmax [0x11D1160]={} poste [0x1158DA8]={:.6g} tipo5 s [0x11D11A0]={:.6g}\n", Rd<float>(0x011D0A60),
                         Rd<float>(0x011D0A68), Floats(0x011D1160, 4), Rd<float>(0x01158DA8), Rd<float>(0x011D11A0));
        o += std::format("desfoque das paredes: passadas [0x1158B1C]={} modo [0x11D02E4]={}\n", Rd<uint32_t>(0x01158B1C), static_cast<int>(Rd<BYTE>(0x011D02E4)));
    } else
        o += "(constantes globais: so na versao Steam 1.67.2)\n";
    const uintptr_t root = Rd<uintptr_t>(g_rootPtrAddr);
    const uintptr_t lightMgr = root ? Rd<uintptr_t>(root + 0x1C0) : 0;
    const uintptr_t tree = lightMgr ? Rd<uintptr_t>(lightMgr + 0xD4) : 0;
    const uintptr_t buckets = Rd<uintptr_t>(tree + 0x58);
    const uint32_t bucketCount = Rd<uint32_t>(tree + 0x5C);
    if (!tree || !buckets || !bucketCount || bucketCount >= (1u << 20)) return o + "(sem arvore de lotes)\n";
    std::unordered_map<uintptr_t, int> outdoor; // light -> lists it is in (room 0 of any level)
    const uintptr_t endNode = Rd<uintptr_t>(buckets + bucketCount * 4);
    uintptr_t slot = buckets, node = Rd<uintptr_t>(slot);
    int guard = 0;
    while (node == 0 && guard++ < (1 << 20)) node = Rd<uintptr_t>(slot += 4);
    guard = 0;
    while (node && node != endNode && guard++ < 100000) {
        const uintptr_t tracker = Rd<uintptr_t>(node + 8);
        for (int level = -4; tracker && level <= 7; level++) {
            const uintptr_t mgr = Rd<uintptr_t>(tracker + 0x6A0 + static_cast<intptr_t>(level) * 0x1A4);
            if (!mgr) continue;
            const uintptr_t rb = Rd<uintptr_t>(mgr + 0x234);
            const uint32_t rc = Rd<uint32_t>(mgr + 0x238);
            for (uint32_t b = 0; rb && rc < 100000 && b < rc; b++) {
                int g2 = 0;
                for (uintptr_t rn = Rd<uintptr_t>(rb + b * 4); rn && g2++ < 10000; rn = Rd<uintptr_t>(rn + 0x80)) {
                    if (Rd<int>(rn) != 0) continue; // room 0 = outdoors
                    const uintptr_t room = Rd<uintptr_t>(rn + 0x10);
                    if (!room) continue;
                    const uintptr_t m = Rd<uintptr_t>(room + 0xF8);
                    o += std::format("-- lote {:08X}{:08X} andar {} comodo0={:08X} classe={} limiar[+0x63C]={:.6g} matriz[+0xF8]={}\n", Rd<uint32_t>(mgr + 0x94), Rd<uint32_t>(mgr + 0x90),
                                     Rd<int>(mgr + 0x88), room, Rd<int>(room + 0xF4), Rd<float>(room + 0x63C), m ? Floats(m, 16) : "nula");
                    const uintptr_t lb = Rd<uintptr_t>(room + 0xC8), le = Rd<uintptr_t>(room + 0xCC);
                    const size_t n = (le >= lb && le - lb < 40000) ? (le - lb) / 4 : 0;
                    for (size_t k = 0; k < n; k++) outdoor[Rd<uintptr_t>(lb + k * 4)]++;
                }
            }
        }
        node = Rd<uintptr_t>(node + 0x10);
        int g3 = 0;
        while (node == 0 && g3++ < (1 << 20)) node = Rd<uintptr_t>(slot += 4);
    }
    o += std::format("\n-- luzes nas listas do comodo 0 ({}): alcance +0x130, forca +0x10.x, cores E0/F0, cone\n", outdoor.size());
    for (const auto& [L, lists] : outdoor) {
        const uint32_t vt = Rd<uint32_t>(L);
        auto it = index.find(L);
        std::string cone;
        if (vt == GameAddr::Get(GameAddr::Id::LightVtable4)) // type 4 spot (0x00FF4570 on Steam)
            cone = std::format(" cone: eixo[+0x170]={} desloc[+0x158]={:.4g} escala[+0x154]={:.4g}", Floats(L + 0x170, 3), Rd<float>(L + 0x158), Rd<float>(L + 0x154));
        else if (vt == GameAddr::Get(GameAddr::Id::LightVtable5)) // type 5 (0x00FF4350 on Steam)
            cone = std::format(" cone: a1[+0x1A0]={} o1[+0x174]={:.4g} a2[+0x190]={} o2[+0x170]={:.4g} S[+0x150]={}", Floats(L + 0x1A0, 3), Rd<float>(L + 0x174),
                               Floats(L + 0x190, 3), Rd<float>(L + 0x170), Floats(L + 0x150, 3));
        o += std::format("L{:08X} {} vt={:08X} tipo={} listas={} pos={} alcance={:.4g} forca={} E0={} F0={}{}\n", L, it != index.end() ? std::format("#{}", it->second) : "-", vt,
                         Rd<int>(L + 0xB0, -1), lists, Floats(L + 0x120, 3), Rd<float>(L + 0x130), Floats(L + 0x10, 4), Floats(L + 0xE0, 4), Floats(L + 0xF0, 4), cone);
    }
    return o;
}

void WriteDiag() {
    const uintptr_t root = Rd<uintptr_t>(g_rootPtrAddr);
    const uintptr_t lightMgr = root ? Rd<uintptr_t>(root + 0x1C0) : 0;
    if (!lightMgr) {
        g_status = "No world loaded";
        Captures::Notify(I18n::Tr("Lighting snapshot: load a world first"), 4, Captures::NoteKind::Warning);
        return;
    }
    // its own folder in Captures\ (never overwritten), with the log and settings (features/captures.h)
    const std::filesystem::path folder = Captures::NewFolder("Lighting snapshot");
    std::ostringstream out;
    const uintptr_t cells = Rd<uintptr_t>(lightMgr + 0x104);
    out << std::format("Apex Radiance lighting snapshot\nnight level={:.2f} lightMgr={:08X} cells={:08X} counter={} / {}\n\n", Rd<float>(lightMgr + 0xF0), lightMgr, cells,
        cells ? Rd<int>(cells + 0x38) : 0, cells ? Rd<int>(cells + 0x3C) : 0);

    // ---- all lights ----
    const bool enumOk = EnumerateLights();
    std::unordered_map<uintptr_t, size_t> index;
    for (size_t i = 0; i < g_lights.size(); i++) index[g_lights[i]] = i;
    int streetLamps = 0, streetOn = 0;
    for (uintptr_t L : g_lights)
        if (Rd<int>(L + 0xB0) == 0xB && (Rd<uint32_t>(L + 0xC0) | Rd<uint32_t>(L + 0xC4)) == 0) {
            streetLamps++;
            if (Rd<BYTE>(L + 0x100) & 0x20) streetOn++;
        }
    out << std::format("==== TODAS AS LUZES ({}{}) | postes da rua (tipo 11, lote 0): {} acesos de {} ====\n", g_lights.size(), enumOk ? "" : ", ENUMERACAO FALHOU", streetOn,
        streetLamps);
    for (size_t i = 0; i < g_lights.size(); i++) out << std::format("#{} ", i) << LightLine(g_lights[i]) << "\n";
    out << "\n";

    // ---- lots and rooms ----
    const uintptr_t tree = Rd<uintptr_t>(lightMgr + 0xD4);
    const uintptr_t buckets = Rd<uintptr_t>(tree + 0x58);
    const uint32_t bucketCount = Rd<uint32_t>(tree + 0x5C);
    out << std::format("==== LOTES (arvore {:08X}, baldes {}) ====\n", tree, bucketCount);
    int managers = 0, rooms = 0;
    if (tree && buckets && bucketCount && bucketCount < (1u << 20)) {
        const uintptr_t endNode = Rd<uintptr_t>(buckets + bucketCount * 4);
        uintptr_t slot = buckets;
        uintptr_t node = Rd<uintptr_t>(slot);
        int guard = 0;
        while (node == 0 && guard++ < (1 << 20)) node = Rd<uintptr_t>(slot += 4);
        guard = 0;
        while (node && node != endNode && guard++ < 100000) {
            const uintptr_t tracker = Rd<uintptr_t>(node + 8);
            for (int level = -4; tracker && level <= 7; level++) {
                const uintptr_t treeLevel = tracker + 0x6A0 + static_cast<intptr_t>(level) * 0x1A4;
                const uintptr_t mgr = Rd<uintptr_t>(treeLevel);
                if (!mgr) continue;
                managers++;
                out << std::format("\n-- LOTE {:08X}{:08X} andar {} (nivel da arvore {}) gerenciador={:08X} sistema={} flag280={} qualidade288={}\n", Rd<uint32_t>(mgr + 0x94),
                    Rd<uint32_t>(mgr + 0x90), Rd<int>(mgr + 0x88), level, mgr, Rd<uintptr_t>(mgr) == lightMgr ? "ok" : "DIFERENTE", static_cast<int>(Rd<BYTE>(mgr + 0x280)),
                    static_cast<int>(Rd<BYTE>(mgr + 0x288)));
                // room hash at mgr+0x230: buckets +0x234, count +0x238; node: key +0, value +0x10, next +0x80
                const uintptr_t rb = Rd<uintptr_t>(mgr + 0x234);
                const uint32_t rc = Rd<uint32_t>(mgr + 0x238);
                for (uint32_t b = 0; rb && rc < 100000 && b < rc; b++) {
                    int g2 = 0;
                    for (uintptr_t rn = Rd<uintptr_t>(rb + b * 4); rn && g2++ < 10000; rn = Rd<uintptr_t>(rn + 0x80)) {
                        const int roomId = Rd<int>(rn);
                        const uintptr_t room = Rd<uintptr_t>(rn + 0x10);
                        if (!room) continue;
                        rooms++;
                        const uintptr_t lb = Rd<uintptr_t>(room + 0xC8), le = Rd<uintptr_t>(room + 0xCC);
                        const size_t n = (le >= lb && le - lb < 40000) ? (le - lb) / 4 : 0;
                        int nStreet = 0, nStreetOn = 0, nLot = 0;
                        std::string ids;
                        for (size_t k = 0; k < n; k++) {
                            const uintptr_t L = Rd<uintptr_t>(lb + k * 4);
                            const bool street = Rd<int>(L + 0xB0) == 0xB && (Rd<uint32_t>(L + 0xC0) | Rd<uint32_t>(L + 0xC4)) == 0;
                            if (street) {
                                nStreet++;
                                if (Rd<BYTE>(L + 0x100) & 0x20) nStreetOn++;
                            } else
                                nLot++;
                            auto it = index.find(L);
                            ids += it != index.end() ? std::format(" #{}", it->second) : std::format(" L{:08X}", L);
                        }
                        out << std::format("   comodo {} ({:08X}) estado={} classe={} x100={} externo={} no_hash24={} luzes={} (postes da rua {} / acesos {}, outras {}):{}\n", roomId, room,
                            Rd<int>(room + 0xF0), Rd<int>(room + 0xF4), Rd<int>(room + 0x100), static_cast<int>(Rd<BYTE>(room + 0x18)), Rd<int>(rn + 0x24), n, nStreet, nStreetOn,
                            nLot, ids);
                    }
                }
            }
            node = Rd<uintptr_t>(node + 0x10);
            int g3 = 0;
            while (node == 0 && g3++ < (1 << 20)) node = Rd<uintptr_t>(slot += 4);
        }
    }
    out << LevelLightShare::DiagText();
    out << PixelLampDiag(index);
    out << std::format("\nEnd: {} lights, {} lot stories, {} rooms\n", g_lights.size(), managers, rooms);
    g_status = std::format("Saved: {} lights, {} lot stories, {} rooms", g_lights.size(), managers, rooms);
    LOG_INFO("[LightDiag] " + g_status);
    Captures::WriteText(folder / L"Lighting snapshot.txt", out.str());
    Captures::Finish(folder, std::format("a snapshot of every light and room ({} lights, {} lot stories, {} rooms)", g_lights.size(), managers, rooms), Captures::CaptureKind::LightingSnapshot);
}

} // namespace

namespace LightDiag {
bool Init() {
    kRootGetter = GameAddr::Get(GameAddr::Id::RootGetter);
    kEnumLights = GameAddr::Get(GameAddr::Id::EnumLights);
    if (!kRootGetter || !kEnumLights) return false;
    if (std::memcmp(reinterpret_cast<const void*>(kRootGetter + 5), kRootGetterTail, sizeof(kRootGetterTail)) != 0 || Rd<BYTE>(kRootGetter) != 0xA1) return false;
    if (GameAddr::IsFixed() && std::memcmp(reinterpret_cast<const void*>(kEnumLights), kEnumLightsBytes, sizeof(kEnumLightsBytes)) != 0) return false;
    g_rootPtrAddr = Rd<uint32_t>(kRootGetter + 1);
    return true;
}
void RequestDump() { g_requested = true; }
void OnPresent() {
    if (!g_rootPtrAddr) return;
    const bool pressed = Hotkeys::Take(Hotkeys::Action::Diagnostics); // Ctrl+Shift+B, 5 or F8 by preset
    if (pressed || g_requested.exchange(false)) WriteDiag();
}
const std::string& Status() { return g_status; }
} // namespace LightDiag
