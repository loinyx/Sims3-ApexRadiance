// Exercise the real room updater. Only TS3 services, patch writes and time are fixtures.
// This executable neither attaches to the game nor writes files.
#include <windows.h>
#include <array>
#include <cstdio>
#include <functional>
#include <limits>
#include "../../framework/s3ss_detect.h"
#include "../../framework/s3ss_ambient_policy.h"

static DWORD testTick = 1000;
static DWORD TestTickCount() { return testTick; }
#define GetTickCount TestTickCount
#ifdef APEX_ROOM_REFERENCE_SOURCE
#include APEX_ROOM_REFERENCE_SOURCE
#else
#include "../../features/unlit_rooms.cpp"
#endif
#undef GetTickCount

static_assert(sizeof(uintptr_t) == 4, "Room offsets require the game's x86 layout");
namespace S3SSDetect {
RoomAmbientCorrection g_testCorrection{};
int g_testCorrectionCalls = 0;
RoomAmbientCorrection CorrectRoomAmbientOverride() { ++g_testCorrectionCalls; return g_testCorrection; }
}

namespace Fixture {
int checks = 0, failures = 0, attempts = 0, rigs = 0, warnings = 0;
int writes = 0, failWrite = 0, applyGroups = 0;
bool queueOk = true, stageUnlit = false, stageBase = false, holdsBase = false;
std::vector<BYTE*> rooms;
std::function<void(BYTE*)> beforeAck;
std::array<DWORD, kSites> sites{};
alignas(16) float original[2][4] = {{0.01f, 0.01f, 0.01f, 0}, {0.15f, 0.15f, 0.30f, 0}};
BYTE gate = 1;
alignas(16) float fill[4] = {0.8f, 0.8f, 1, 0.8f};
uintptr_t wm = 1;
float scale = 1;

void Check(bool ok, const char* label) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
}
bool Equal(const float* a, const float* b, float tolerance = 1e-6f) {
    for (int k = 0; k < 4; ++k)
        if (!std::isfinite(a[k]) || std::fabs(a[k] - b[k]) > tolerance) return false;
    return true;
}
struct alignas(16) Room {
    std::array<BYTE, 0x180> data{};
    Room(int id = 1, int slot = 1, bool lit = false) {
        Field<uintptr_t>(0) = 0x12340000;
        Field<int>(0xC) = id;
        Field<uint32_t>(0x10) = static_cast<uint32_t>(slot);
        Field<int>(0xF0) = 4;
        Field<uintptr_t>(0xC8) = lit ? 0x2000 : 0;
        Field<uintptr_t>(0xCC) = lit ? 0x2004 : 0;
        SetAmbient(original[slot], original[slot]);
    }
    template <class T> T& Field(size_t offset) { return *reinterpret_cast<T*>(data.data() + offset); }
    BYTE* Ptr() { return data.data(); }
    float* Ambient(int sample = 0) { return reinterpret_cast<float*>(data.data() + 0x110 + sample * 16); }
    void SetAmbient(const float* first, const float* second) {
        std::memcpy(Ambient(), first, 16); std::memcpy(Ambient(1), second, 16);
    }
};
bool __fastcall LotTest(void*, void*, uint32_t lo, uint32_t) { return lo == 0; }

void Reset(bool enabled = true, float brightness = 0.35f, float blue = 0.2f) {
    // Previous patch fixtures live in this process only; reset them before reseeding.
    Unpatch();
#ifndef APEX_ROOM_REFERENCE_SOURCE
    g_compat = {};
    S3SSDetect::g_testCorrection = {};
    S3SSDetect::g_testCorrectionCalls = 0;
#endif
    attempts = rigs = warnings = writes = failWrite = applyGroups = 0;
    queueOk = true; stageUnlit = stageBase = holdsBase = false;
    rooms.clear(); beforeAck = {};
    testTick = 1000;
    const float first[4] = {0.01f, 0.01f, 0.01f, 0};
    const float second[4] = {0.15f, 0.15f, 0.30f, 0};
    std::memcpy(original[0], first, 16); std::memcpy(original[1], second, 16);
    for (int s = 0; s < kSites; ++s) {
        const uintptr_t ptr = s == Gate ? reinterpret_cast<uintptr_t>(&gate) : s == Fill ? reinterpret_cast<uintptr_t>(fill)
            : reinterpret_cast<uintptr_t>(original[s == ColourB || s == DimB ? 1 : 0]);
        sites[s] = g_orig[s] = static_cast<DWORD>(ptr);
        g_site[s] = reinterpret_cast<uintptr_t>(&sites[s]);
    }
    g_ready = g_baseReady = true; g_on = false;
    g_light = g_blue = 1; g_night = 0; g_drawDark = false;
    g_basePatches.clear(); g_baseRooms.clear(); g_retintSent.clear();
    g_slotOfLot.clear(); g_slotCacheAt = 0;
    g_known[0].clear(); g_known[1].clear();
    g_retintDue = g_restDue = g_retryRetint = false;
    g_lastRetint = g_lastRig = g_rigAgainAt = g_changedAt = 0;
    g_nextReconcile = g_nextBaseCheck = 0;
    g_retinted = g_dimSent = g_relights = 0;
    g_baseAdded.store(0); g_baseMoved.store(0);
    g_wmPtr = reinterpret_cast<uintptr_t>(&wm);
    g_lotTest = reinterpret_cast<uintptr_t>(&LotTest);
    g_scalePtr = reinterpret_cast<uintptr_t>(&scale); scale = 1;
    ReadBases();
    for (int i = 0; i < 2; ++i) g_target[i] = g_base[i];
    UnlitRooms::Set(enabled, brightness, blue);
}
void SeedBase(Room& room, int slot = 1) {
    const float lamps[2][4] = {{0.03f, 0.08f, 0.11f, 0}, {0.02f, 0.06f, 0.09f, 0}};
    for (int sample = 0; sample < 2; ++sample) {
        float result[4];
        for (int k = 0; k < 4; ++k) result[k] = lamps[sample][k] + g_target[slot].v[k];
        std::memcpy(room.Ambient(sample), result, 16);
        NoteBase(room.Ptr(), result, slot, g_target[slot].v, sample);
    }
}
void Frame(DWORD at) { testTick = at; UnlitRooms::OnPresent(); }
} // namespace Fixture

namespace ApexLog {
void Write(Level level, const std::string&, const std::source_location&) noexcept {
    Fixture::warnings += level == Level::Warning;
}
}
namespace GameAddr {
uintptr_t Get(Id) { return 0; }
bool Have(std::initializer_list<Id>, std::string* missing) { if (missing) *missing = "fixture"; return false; }
std::string NotAvailable(const std::string& missing) { return missing; }
}
namespace MemPatch {
bool WriteDWORD(uintptr_t address, DWORD value, std::vector<PatchLocation>* undo, const DWORD* expected) {
    if (++Fixture::writes == Fixture::failWrite) return false;
    auto* ptr = reinterpret_cast<DWORD*>(address);
    if (expected && *ptr != *expected) return false;
    if (undo) {
        PatchLocation item; item.address = address; item.original.resize(4);
        std::memcpy(item.original.data(), ptr, 4); undo->push_back(item);
    }
    *ptr = value; return true;
}
bool RestoreAll(std::vector<PatchLocation>& undo) {
    for (auto it = undo.rbegin(); it != undo.rend(); ++it)
        std::memcpy(reinterpret_cast<void*>(it->address), it->original.data(), it->original.size());
    undo.clear(); return true;
}
}
namespace ObjectLightBridge {
void RequestRigRefresh() { ++Fixture::rigs; }
}
namespace LevelLightShare {
int ForEachRoom(bool (*visit)(unsigned char*, void*), void* ctx, int* queued, void (*ack)(unsigned char*, bool, void*)) {
    int sent = 0;
    for (auto* room : Fixture::rooms) if (visit(room, ctx)) {
        ++Fixture::attempts;
        if (Fixture::beforeAck) Fixture::beforeAck(room);
        sent += Fixture::queueOk;
        if (ack) ack(room, Fixture::queueOk, ctx);
    }
    if (queued) *queued = sent;
    return static_cast<int>(Fixture::rooms.size());
}
bool StageAmbientBaseChange(unsigned char*, const float*, const float*, const float*, const float*) { return Fixture::stageBase; }
bool HoldsAmbientBase(unsigned char*, const float*, const float*) { return Fixture::holdsBase; }
bool StageUnlitAmbientChange(unsigned char*, const float*, bool& changed) { changed = Fixture::stageUnlit; return Fixture::stageUnlit; }
void ApplyAmbientBaseChanges() { ++Fixture::applyGroups; }
}

static void BrightnessReferenceTests() {
    using namespace Fixture;
    // Live 2026-10-03 values: both RGB families were .01 after S3SS's saved debug setting.
    Reset(true, 1, 1);
    original[0][3] = 0.01f;
    const float saved[4] = {0.01f, 0.01f, 0.01f, 0.15f};
    std::memcpy(original[1], saved, 16);
    Frame(1100); Frame(1200);
    Room first(30, 0), second(31, 1); rooms = {first.Ptr(), second.Ptr()};
    for (float brightness : {0.0f, 0.1f, 0.35f, 0.5f, 1.0f})
        for (float blue : {0.0f, 0.2f, 1.0f}) {
            UnlitRooms::Set(true, brightness, blue); Retint(false);
            const float expected[4] = {0.01f * brightness, 0.01f * brightness, 0.01f * brightness, 0.15f * brightness};
            Check(Equal(second.Ambient(), expected), "saved grey base follows brightness exactly once and blue cannot create missing chroma");
            Check(std::fabs(first.Ambient()[0] - 0.01f * brightness) < 1e-6f,
                "first lot family retains its distinct small native grey base");
            UnlitRooms::SetNightLevel(1); UnlitRooms::SetDrawDark(false);
            Check(std::fabs(UnlitRooms::FurnitureAmbient() - brightness) < 1e-6f,
                "saved ambient base does not multiply furniture brightness again");
            float cube[3]; UnlitRooms::FurnitureCubeColour(cube);
            Check(std::fabs(cube[0] - 1) < 1e-6f && std::fabs(cube[1] - 1) < 1e-6f && std::fabs(cube[2] - 1) < 1e-6f,
                "furniture cube retains neutral colour when the inherited base has no blue chroma");
        }
    Check(Equal(g_colour[1].v, saved), "100 percent passes the complete inherited ambient unchanged");
    std::printf("Inherited grey, 100%%: %.5f %.5f %.5f; luma %.5f\n", g_colour[1].v[0], g_colour[1].v[1], g_colour[1].v[2], Luma(g_colour[1].v));
    const float standard[4] = {0.15f, 0.15f, 0.30f, 0.15f};
    std::memcpy(original[1], standard, 16); Frame(3200);
    Check(Equal(g_colour[1].v, standard) && g_base[1].v[0] == 0.15f,
        "restoring the second native base restores the full standard brightness without changing the slider formula");
    Check(std::fabs(g_colour[0].v[0] - 0.01f) < 1e-6f, "restoring blue base never promotes the legitimately grey first family");
    std::printf("Standard blue, 100%%: %.5f %.5f %.5f; luma %.5f\n", g_colour[1].v[0], g_colour[1].v[1], g_colour[1].v[2], Luma(g_colour[1].v));
}

static void RecoveryTests() {
    using namespace Fixture;
    Reset(); Room lit(1, 1, true); SeedBase(lit); rooms = {lit.Ptr()};
    UnlitRooms::Set(false, 0.35f, 0.2f);
    queueOk = false; Retint(true);
    Check(attempts == 1 && g_retryRetint, "failed restoring queue keeps recovery armed");
    Check(g_retintSent.empty(), "failed queue removes provisional dedup record");
    Check(g_baseRooms.contains(reinterpret_cast<uintptr_t>(lit.Ptr())), "failed restoring queue retains lit-room base");
    queueOk = true; Retint(true);
    Check(attempts == 2, "disabled lit room retries restoring solve after queue failure");
    Check(g_baseRooms.empty(), "accepted restoring solve retires old base");
    Retint(true); Check(attempts == 2, "accepted restoration is not repeated");

    Reset(); Room busy(2, 1, true); SeedBase(busy); rooms = {busy.Ptr()};
    UnlitRooms::Set(false, 0.35f, 0.2f); busy.Field<int>(0xF0) = 3;
    Frame(1100); Frame(1700);
    Check(attempts == 0, "disabling never invalidates an active native solve");
    busy.Field<int>(0xF0) = 4; queueOk = false; Frame(2700);
    Check(attempts == 1, "busy to idle restoration reaches native queue");
    queueOk = true; Frame(3700);
    Check(attempts == 2, "periodic recovery retries rejected restoration while disabled");

    Reset(); Room unknown(3); const float foreign[4] = {0.07f, 0.02f, 0.19f, 0};
    unknown.SetAmbient(foreign, foreign); rooms = {unknown.Ptr()};
    Retint(false); Check(attempts == 0, "unknown ambient is preserved while dragging");
    Retint(true); Retint(true); Check(attempts == 1, "unknown room queues once per settled target");
    Check(Equal(unknown.Ambient(), foreign), "unknown ambient is never guessed");
    UnlitRooms::Set(true, 0.6f, 0.7f); Retint(true);
    Check(attempts == 2, "new target rearms native solve");
    unknown.Field<int>(0xC) = 4; Retint(true);
    Check(attempts == 3, "reused address with new room id is not deduplicated");
    unknown.Field<uintptr_t>(0) += 4; Retint(true);
    Check(attempts == 4, "reused address with new manager is not deduplicated");
    UnlitRooms::OnWorldChanged(); Retint(true);
    Check(attempts == 5, "world change discards old solve identities");

    Reset(); Room evicted(20); rooms = {evicted.Ptr()};
    UnlitRooms::Set(true, 0.01f, 0.01f); Retint(false); rooms.clear();
    for (int step = 0; step < 200; ++step)
        UnlitRooms::Set(true, 0.2f + static_cast<float>(step) / 400, static_cast<float>(step % 19) / 19);
    rooms = {evicted.Ptr()}; Retint(true);
    Check(attempts == 1, "room left behind beyond bounded colour history recovers through native solve");

    Reset(); Room settle(21); settle.SetAmbient(foreign, foreign); rooms = {settle.Ptr()};
    for (DWORD at = 1010; at <= 1510; at += 10) {
        testTick = at; UnlitRooms::Set(true, 0.2f + static_cast<float>(at - 1010) / 1000, 0.4f); Frame(at);
    }
    Frame(1900); Check(attempts == 0, "rapid dragging keeps unknown solve deferred until latest change settles");
    Frame(1920); Check(attempts == 1, "latest target schedules one solve after the debounce");

    // An unset deadline must be immediately eligible even after 24.9 days of Windows uptime.
    Reset(); testTick = 0x90000000u; UnlitRooms::OnWorldChanged();
    Room longUptime(22); longUptime.Field<int>(0xF0) = 3; rooms = {longUptime.Ptr()};
    Frame(0x90000100u); Frame(0x90000300u); longUptime.Field<int>(0xF0) = 4; Frame(0x90000600u);
    Check(Equal(longUptime.Ambient(), g_target[1].v), "busy room recovery starts at high Windows uptime after world reset");
    original[1][0] = original[1][1] = 0.2f; original[1][2] = 0.4f; Frame(0x90000a00u);
    Check(g_base[1].v[2] == 0.4f, "late game-base checks start at high Windows uptime");

    Reset(); Room fresh(5, 1, true); SeedBase(fresh); rooms = {fresh.Ptr()};
    fresh.Ambient()[0] += 0.2f; // External/native modification invalidates ownership.
    beforeAck = [&](BYTE*) { SeedBase(fresh); };
    Retint(true);
    Check(attempts == 1, "changed lit ambient uses native recovery");
    Check(g_baseRooms.contains(reinterpret_cast<uintptr_t>(fresh.Ptr())), "queue acknowledgement retains a newly captured base");

    Reset(); Room replaced(6, 1, true); SeedBase(replaced);
    replaced.Field<int>(0xC) = 7; rooms = {replaced.Ptr()}; Retint(true);
    Check(g_baseRooms.empty(), "stale base is discarded on identity mismatch");
    Check(attempts == 1, "replacement lit room receives its own solve");
}

static void NativeBaseTests() {
    using namespace Fixture;
    Reset(true, 1, 1); Room lit(23, 1, true);
    const float strongLamp[4] = {0.8f, 0.6f, 0.3f, 0};
    float result[4]; std::memcpy(result, strongLamp, 16); float ce[4]; int slot = -1;
    Check(AddBase(lit.Ptr(), result, slot, ce) && slot == 1, "native top-up uses the actual lot family");
    const float expected[4] = {0.95f, 0.75f, 0.6f, 0};
    Check(Equal(result, expected) && Equal(ce, original[1]), "strong native lamp light keeps its complete contribution plus the background");
    // Native shortfall: lamp sum .3, base sum .6, original result = lamp + (.6-.3)*base.
    float shortfall[4] = {0.145f, 0.145f, 0.19f, 0};
    const float fullBase[4] = {0.25f, 0.25f, 0.4f, 0};
    Check(AddBase(lit.Ptr(), shortfall, slot, ce) && Equal(shortfall, fullBase), "native shortfall is replaced without double-counting existing top-up");
    std::memcpy(result, original[1], 16);
    Check(AddBase(lit.Ptr(), result, slot, ce) && Equal(result, original[1]), "no-lamp native result does not receive the background twice");
    lit.Field<uint32_t>(0x10) = 0; std::memcpy(result, strongLamp, 16);
    const float firstFamily[4] = {0.81f, 0.61f, 0.31f, 0};
    Check(AddBase(lit.Ptr(), result, slot, ce) && slot == 0 && Equal(result, firstFamily), "native top-up preserves the separate first lot family");
    UnlitRooms::Set(true, 0, 0); std::memcpy(result, strongLamp, 16);
    Check(!AddBase(lit.Ptr(), result, slot, ce) && Equal(result, strongLamp), "zero background never subtracts or alters real native lamp light");
    lit.Field<BYTE>(0x18) = 1;
    Check(!AddBase(lit.Ptr(), result, slot, ce), "native base hook excludes roofless rooms");
    lit.Field<BYTE>(0x18) = 0; lit.Field<int>(0xC) = 0;
    Check(!AddBase(lit.Ptr(), result, slot, ce), "native base hook excludes exterior room zero");
}

static void SliderAndLifecycleTests() {
    using namespace Fixture;
    Reset(); Room first(10, 0), second(11, 1); rooms = {first.Ptr(), second.Ptr()};
    for (int step = 0; step <= 100; ++step) {
        const float brightness = static_cast<float>(step) / 100;
        UnlitRooms::Set(true, brightness, 0.25f); Retint(false);
        Check(Equal(first.Ambient(), g_target[0].v), "first lot family follows full brightness sweep");
        Check(Equal(second.Ambient(), g_target[1].v), "second lot family follows full brightness sweep");
        Check(Equal(second.Ambient(1), g_target[1].v), "both native samples follow an unlit sweep");
    }
    for (int step = 100; step >= 0; --step) {
        const float blue = static_cast<float>(step) / 100;
        UnlitRooms::Set(true, 0.7f, blue); Retint(false);
        const float grey = 0.2126f * 0.15f + 0.7152f * 0.15f + 0.0722f * 0.3f;
        const float expected[4] = {0.7f * (grey + blue * (0.15f - grey)), 0.7f * (grey + blue * (0.15f - grey)),
            0.7f * (grey + blue * (0.3f - grey)), 0};
        Check(Equal(second.Ambient(), expected), "blue tint sweep preserves intended luminance");
        Check(Equal(first.Ambient(), g_target[0].v), "grey game base remains grey throughout blue sweep");
    }
    Check(attempts == 0, "known unlit rooms need no native solve throughout sweeps");
    UnlitRooms::Set(false, 0.7f, 0); Retint(false);
    Check(Equal(first.Ambient(), original[0]) && Equal(second.Ambient(), original[1]), "disabling restores both original lot families");

    Reset(); Room lit(12, 1, true); SeedBase(lit); rooms = {lit.Ptr()};
    float lampShare[2][4];
    for (int sample = 0; sample < 2; ++sample)
        for (int k = 0; k < 4; ++k) lampShare[sample][k] = lit.Ambient(sample)[k] - g_target[1].v[k];
    for (int step = 0; step <= 100; ++step) {
        UnlitRooms::Set(true, static_cast<float>(step) / 100, static_cast<float>(100 - step) / 100); Retint(false);
        for (int sample = 0; sample < 2; ++sample) {
            float expected[4];
            for (int k = 0; k < 4; ++k) expected[k] = lampShare[sample][k] + g_target[1].v[k];
            Check(Equal(lit.Ambient(sample), expected), "lit sweep preserves lamp light independently in both samples");
        }
    }
    Check(attempts == 0, "owned lit sweep does not restart native solves");
    lit.Ambient()[0] += 0.03f; Retint(false);
    Check(attempts == 0 && g_retryRetint, "changed lit ownership defers recovery while dragging");
    Retint(true); Check(attempts == 1, "changed lit ownership recovers at rest");

    Reset(); Room busy(13); busy.Field<int>(0xF0) = 2; rooms = {busy.Ptr()};
    Retint(true); Check(Equal(busy.Ambient(), original[1]) && attempts == 0, "busy ambient is never written or restarted");
    busy.Field<int>(0xF0) = 4; Retint(false);
    Check(Equal(busy.Ambient(), g_target[1].v), "busy unlit room catches up when idle");
    Room late(14); rooms.push_back(late.Ptr()); Frame(2100);
    Check(Equal(late.Ambient(), g_target[1].v), "newly streamed room follows unchanged sliders");
    const int previousRigs = rigs; Frame(3200);
    Check(rigs == previousRigs, "settled periodic reconciliation does not refresh rigs without a change");

    Reset(); Room exterior(0), roofless(15); roofless.Field<BYTE>(0x18) = 1;
    rooms = {exterior.Ptr(), roofless.Ptr()}; Retint(true);
    Check(Equal(exterior.Ambient(), original[1]) && Equal(roofless.Ambient(), original[1]) && attempts == 0,
        "exterior and roofless rooms preserve game lighting");

    Reset(); Room load(16); rooms = {load.Ptr()}; Frame(1100);
    original[1][0] = original[1][1] = 0.2f; original[1][2] = 0.4f; Frame(3200);
    Check(Equal(load.Ambient(), g_target[1].v) && g_base[1].v[2] == 0.4f, "late game-base initialization recomputes room target");
    original[1][0] = std::numeric_limits<float>::quiet_NaN(); Frame(5300);
    Check(g_baseDefault[1] && Equal(g_base[1].v, kDefaultColour), "invalid base falls back to finite game defaults");
    std::memcpy(original[1], kDefaultColour, 16); Frame(7400);
    Check(!g_baseDefault[1], "valid game base replaces temporary fallback");

    Reset(); Room merged(17); const float ambient[4] = {0.09f, 0.11f, 0.12f, 0};
    merged.SetAmbient(ambient, ambient); rooms = {merged.Ptr()}; stageUnlit = true;
    Retint(false); Check(attempts == 0 && applyGroups > 0 && Equal(merged.Ambient(), ambient),
        "connected unlit group delegates to validated group updater without direct writes");
    Reset(); Room mergedLit(18, 1, true); SeedBase(mergedLit); rooms = {mergedLit.Ptr()};
    float before[4]; std::memcpy(before, mergedLit.Ambient(), 16);
    stageBase = true; UnlitRooms::Set(true, 0.6f, 0.4f); Retint(false);
    Check(attempts == 0 && g_baseMoved.load() == 1 && Equal(mergedLit.Ambient(), before),
        "connected lit group stages the delta instead of writing merged ambient individually");

    Reset(false); failWrite = 3; UnlitRooms::Set(true, 0.2f, 0.2f);
    Check(!g_on && !g_patched && warnings == 1, "partial patch failure leaves feature off");
    bool originalsRestored = true;
    for (int s = 0; s < kSites; ++s) originalsRestored &= sites[s] == g_orig[s];
    Check(originalsRestored, "partial patch failure rolls back all writes");
    failWrite = 0; UnlitRooms::Set(true, 0.2f, 0.2f);
    Check(g_on && g_patched, "next frame can retry a failed feature enable");

    Reset(); Room wrap(19); rooms = {wrap.Ptr()}; testTick = 0xffffff00u;
    UnlitRooms::Set(true, 0.7f, 0.3f); Frame(0xffffff50u); Frame(0x100u);
    Check(Equal(wrap.Ambient(), g_target[1].v) && !g_restDue, "settled target deadlines survive tick wrap");
    UnlitRooms::SetDrawDark(true); UnlitRooms::OnWorldChanged();
    Check(!g_drawDark && g_baseRooms.empty() && g_retintSent.empty() && g_slotOfLot.empty(), "world reset clears dark-draw and room ownership state");
}

static void FurnitureTests() {
    using namespace Fixture;
    for (float brightness : {0.0f, 0.01f, 0.35f, 0.99f, 1.0f})
        for (float blue : {0.0f, 0.01f, 0.2f, 0.8f, 1.0f})
            for (float night : {0.0f, 0.4f, 1.0f})
                for (bool dark : {false, true}) {
                    Reset(true, brightness, blue);
                    UnlitRooms::SetNightLevel(night); UnlitRooms::SetDrawDark(dark);
                    const float direction[4] = {0.09535f, 0.95346f, 0.28604f, 0};
                    const float source[4] = {0.15f, 0.2f, 0.3f, 0};
                    float light[4]; std::memcpy(light, source, 16);
                    const float weight = dark ? 1.0f : night;
                    const float share = 1.0f + (brightness - 1.0f) * weight;
                    const float tint = 1.0f + (blue - 1.0f) * weight;
                    const float luminance = 0.2126f * source[0] + 0.7152f * source[1] + 0.0722f * source[2];
                    float expected[4] = {0, 0, 0, source[3]};
                    for (int k = 0; k < 3; ++k) expected[k] = share * (luminance + tint * (source[k] - luminance));
                    const bool changed = UnlitRooms::FurnitureColour(light, direction);
                    Check(changed == (weight > 0.001f) && Equal(light, expected),
                        "furniture brightness and tint grid matches independent day/dark/night expectation");
                    Check(std::fabs(0.2126f * light[0] + 0.7152f * light[1] + 0.0722f * light[2] - share * luminance) < 1e-6f,
                        "furniture tint never changes luminance or multiplies brightness twice");
                }
    Reset(true, 0.25f, 0.0f);
    const float lampDir[4] = {0.2f, 0.3f, 0.4f, 0};
    const float noLight[4] = {0.09535f, 0.95346f, 0.28604f, 0};
    for (int step = 0; step <= 100; ++step) {
        const float night = static_cast<float>(step) / 100;
        UnlitRooms::SetNightLevel(night);
        Check(std::fabs(UnlitRooms::FurnitureAmbient() - (1 - 0.75f * night)) < 1e-6f, "furniture brightness interpolates continuously through day and night");
        float lamp[4] = {0.01f, 0.015f, 0.09f, 0}; const float before[4] = {0.01f, 0.015f, 0.09f, 0};
        Check(!UnlitRooms::FurnitureColour(lamp, lampDir) && Equal(lamp, before), "real blue lamp retains its own light and colour at every night level");
    }
    UnlitRooms::SetNightLevel(0); UnlitRooms::SetDrawDark(true);
    Check(UnlitRooms::FurnitureActive() && UnlitRooms::FurnitureAmbient() == 0.25f && UnlitRooms::FurnitureTint() == 0,
        "dark-room override follows both sliders even during daytime");
    float fillLight[4] = {0.15f, 0.15f, 0.3f, 0.8f};
    const float expectedGrey = 0.25f * (0.2126f * 0.15f + 0.7152f * 0.15f + 0.0722f * 0.3f);
    Check(UnlitRooms::FurnitureColour(fillLight, noLight) && std::fabs(fillLight[0] - expectedGrey) < 1e-6f
        && fillLight[0] == fillLight[1] && fillLight[1] == fillLight[2] && fillLight[3] == 0.8f,
        "fill follows brightness and zero blue without altering ownership alpha");
    float cube[3]; UnlitRooms::FurnitureCubeColour(cube);
    Check(cube[0] == 1 && cube[1] == 1 && cube[2] == 1, "zero blue makes furniture cube neutral");
    UnlitRooms::Set(true, 1, 1); UnlitRooms::FurnitureCubeColour(cube);
    Check(cube[2] > cube[0] && std::fabs(0.2126f * cube[0] + 0.7152f * cube[1] + 0.0722f * cube[2] - 1) < 1e-6f,
        "full blue cube keeps normalized luminance");
    const float darkColour[4] = {0.15f, 0.15f, 0.3f, 0};
    Check(UnlitRooms::IsDarkRoomLight(darkColour, noLight) && !UnlitRooms::IsDarkRoomLight(darkColour, lampDir),
        "dark detection uses native NoLight directions rather than a blue colour heuristic");
    UnlitRooms::SetDrawDark(false);
    Check(!UnlitRooms::FurnitureActive() && UnlitRooms::FurnitureAmbient() == 1, "following daytime draw is unaffected after dark override reset");
    UnlitRooms::SetNightLevel(1); UnlitRooms::Set(false, 0, 0);
    Check(!UnlitRooms::FurnitureActive() && UnlitRooms::FurnitureAmbient() == 1 && UnlitRooms::FurnitureTint() == 1,
        "disabled feature leaves furniture unchanged at night");
}

void CompatibilityTests() {
    using Fixture::Check;
    const std::string original = "# keep comment\n[patches.BradyBunchBlue]\nenabled = false\n[settings.'BradyBunchBlue RGB']\nvalue = [0.01, 0.01, 0.01]\n[settings.Other]\nvalue = 42\n";
    auto corrected = S3SSAmbientPolicy::Prepare(original);
    Check(corrected.has_value(), "saved RGB correction prepared");
    Check(corrected && corrected->text == "# keep comment\n[patches.BradyBunchBlue]\nenabled = false\n[settings.Other]\nvalue = 42\n", "only the target section removed, comments and other settings retained");
    Check(corrected && !S3SSAmbientPolicy::Prepare(corrected->text), "correction is idempotent");
    Check(!S3SSAmbientPolicy::Prepare("invalid = ["), "malformed config left untouched");
    Check(!S3SSAmbientPolicy::Prepare("[settings.'BradyBunchBlue RGB']\nvalue = [nan, 0.1, 0.1]"), "nonfinite RGB left untouched");
    Check(!S3SSAmbientPolicy::Prepare("[settings.'BradyBunchBlue RGB']\nvalue = [0.1, 0.1]"), "incomplete RGB left untouched");
    Check(!S3SSAmbientPolicy::Prepare("[settings.'BradyBunchBlue RGB']\nvalue = ['bad', 0.1, 0.1]"), "mistyped RGB left untouched");
    const std::string compact = "settings = { 'BradyBunchBlue RGB' = { value = [0.01, 0.01, 0.01] }, Other = { value = 42 } }\n";
    const auto formatted = S3SSAmbientPolicy::Prepare(compact);
    Check(formatted && toml::parse(formatted->text)["settings"]["Other"]["value"].value_or(0) == 42,
        "inline TOML retains unrelated setting values");
    const auto crlf = S3SSAmbientPolicy::Prepare("# preserved\r\n[settings.\"BradyBunchBlue RGB\"]\r\nvalue=[0.01,0.01,0.01]\r\n");
    Check(crlf && crlf->text == "# preserved\r\n", "last section and CRLF removed without changing retained bytes");

}

int main(int argc, char** argv) {
    BrightnessReferenceTests();
    if (argc != 2 || std::strcmp(argv[1], "--brightness-reference") != 0) {
        RecoveryTests(); SliderAndLifecycleTests(); NativeBaseTests(); FurnitureTests(); CompatibilityTests();
    }
    std::printf("Production room updater: %d checks, %d failures\n", Fixture::checks, Fixture::failures);
    return Fixture::failures ? 1 : 0;
}


namespace HookGuard { void Note(const char*) noexcept {} }
