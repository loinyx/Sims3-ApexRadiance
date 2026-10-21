// Daylight probe (see daylight_probe.h). Credits: @loinyx
// Steam 1.67.2 layout (Ghidra, TS3W.exe bytes checked):
//  - the sky update FUN_00C11AD0 stores lightMgr+0xD0 = sky+0x210 and reads lightMgr from sky+0x1F4, so the sky is
//    *(lightMgr+0xD0)-0x210, accepted only when *(sky+0x1F4) == lightMgr
//  - FUN_00C130B0: hour = *(*(sky+0x30)+0x1C)+0x370 / 3600 (FUN_006FAD00, FUN_006E8910); sky+0x780 = smooth night
//    (clock and season only), sky+0x784 = M + (1-M)*clamp(-5*sky+0x6B4 - 0.2, 0, 1) with M = FUN_00C164D0(1) on the
//    weather timeline *(*(sky+0x21C)+0x74)+0x10 (FUN_00580430 is lea eax,[ecx+10h]; ret)
//  - FUN_00C164D0(i) = sum_k w_k * param_k[i] / norm: weights timeline+0x830..+0x840, norm timeline+0x4, the per-type
//    parameter arrays at timeline+0x1A0 + k*0x1A0 (index 1 = ManmadeLightIntensity)
//  - FUN_00D68FE0 copies sunrise and sunset into sky+0x974 / +0x978 (weather manager +0x9EC or +0xA48 by byte +0xB04)
//  - sky+0x6B0 = sun direction (cos t cos p, sin t cos p, sin p); +0x6B4 as the sine of the sun height is inferred
//  - the type order Clear, PartlyCloudy, Overcast, Stormy, Custom is inferred from the string table at 0x01183F9C
//  - the lights' fade at +0x20 (FUN_006BC430: F4, F0 or 1 by the light's channel) is verified; the channel per class is not
// The game writes lightMgr+0xF0 only in steps of 0.2 (FUN_006ADD60) and +0xF4 from sky+0x784 (FUN_006ADE40).
#include "daylight_probe.h"
#include "dev_switches.h"
#include "apex_log.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <utility>

namespace {

bool SafeCopy(void* dst, uintptr_t src, size_t n) {
    if (src < 0x10000) return false;
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <typename T> bool Rd(uintptr_t a, T& v) { return SafeCopy(&v, a, sizeof(T)); }

constexpr const char* kTypeNames[5] = {"clear", "partly", "overcast", "stormy", "custom"}; // order inferred

struct Sample {
    uintptr_t sky = 0;
    float f0 = 0, f4 = 0, mgrHour = 0;
    float smoothNight = 0, manmade = 0;
    bool hourOk = false;
    float hour = 0;
    float sunrise = 0, sunset = 0;
    float rampOff[4] = {}; // sky+0x990, +0x9C0, +0x9C4, +0x9D0
    float sun[4] = {};
    float sunMoon[4] = {}, ambTop[4] = {}, ambBottom[4] = {}, fog[4] = {};
    bool weatherOk = false;
    float w[5] = {}, norm = 0, manmadeType[5] = {};
};

// "" when the sky was found; otherwise why not
const char* ReadSample(uintptr_t lightMgr, Sample& s) {
    uintptr_t back = 0, mgrCheck = 0;
    if (!Rd(lightMgr + 0xF0, s.f0) || !Rd(lightMgr + 0xF4, s.f4)) return "light manager unreadable";
    Rd(lightMgr + 0xB4, s.mgrHour);
    if (!Rd(lightMgr + 0xD0, back) || back < 0x10000 + 0x210) return "no sky pointer at lightMgr+0xD0";
    s.sky = back - 0x210;
    if (!Rd(s.sky + 0x1F4, mgrCheck) || mgrCheck != lightMgr) return "sky+0x1F4 does not point back to the light manager";
    const uintptr_t sky = s.sky;
    if (!Rd(sky + 0x780, s.smoothNight) || !Rd(sky + 0x784, s.manmade) || !SafeCopy(s.sun, sky + 0x6B0, sizeof(s.sun))) return "sky fields unreadable";
    Rd(sky + 0x974, s.sunrise);
    Rd(sky + 0x978, s.sunset);
    Rd(sky + 0x990, s.rampOff[0]);
    Rd(sky + 0x9C0, s.rampOff[1]);
    Rd(sky + 0x9C4, s.rampOff[2]);
    Rd(sky + 0x9D0, s.rampOff[3]);
    SafeCopy(s.sunMoon, sky + 0x800, sizeof(s.sunMoon));
    SafeCopy(s.ambTop, sky + 0x760, sizeof(s.ambTop));
    SafeCopy(s.ambBottom, sky + 0x770, sizeof(s.ambBottom));
    SafeCopy(s.fog, sky + 0x700, sizeof(s.fog));
    uintptr_t t30 = 0, timeObj = 0;
    float secs = 0;
    if (Rd(sky + 0x30, t30) && Rd(t30 + 0x1C, timeObj) && Rd(timeObj + 0x370, secs) && std::isfinite(secs)) {
        s.hourOk = true;
        s.hour = secs / 3600.0f;
    }
    uintptr_t holder = 0, weatherMgr = 0;
    if (Rd(sky + 0x21C, holder) && Rd(holder + 0x74, weatherMgr) && weatherMgr) {
        const uintptr_t tl = weatherMgr + 0x10;
        bool ok = SafeCopy(s.w, tl + 0x830, sizeof(s.w)) && Rd(tl + 0x4, s.norm);
        for (int k = 0; ok && k < 5; k++) ok = Rd(tl + 0x1A0 + k * 0x1A0 + 4, s.manmadeType[k]);
        s.weatherOk = ok;
    }
    return "";
}

float Lum(const float c[4]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
float Clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
float SunHeightDeg(float sunY) { return std::asin(sunY < -1 ? -1 : (sunY > 1 ? 1 : sunY)) * 57.2957795f; }

std::string WeatherText(const Sample& s) {
    if (!s.weatherOk) return "weather unreadable";
    float sum = 0;
    for (float v : s.w) sum += v;
    std::string t = "weights";
    for (int k = 0; k < 5; k++) t += std::format(" {} {:.3f}", kTypeNames[k], s.w[k]);
    t += std::format(" (sum {:.4f}, norm {:.4f}{}), manmade per type", sum, s.norm, s.norm > 0 && std::fabs(sum - s.norm) < 1e-3f * s.norm ? "" : ", MISMATCH");
    for (int k = 0; k < 5; k++) t += std::format(" {:.3f}", s.manmadeType[k]);
    if (s.norm > 0) { // the game's own F4 formula, recomputed: confirms the offsets
        float m = 0;
        for (int k = 0; k < 5; k++) m += s.w[k] * s.manmadeType[k];
        m /= s.norm;
        t += std::format(", M {:.4f} -> F4 predicted {:.4f}", m, m + (1 - m) * Clamp01(-5.0f * s.sun[1] - 0.2f));
    }
    return t;
}

std::string LineText(const Sample& s) {
    return std::format("hour {} (lightMgr+0xB4 {:.3f}), sunrise {:.3f} sunset {:.3f} (ramp offsets {:.3f} {:.3f} {:.3f} {:.3f}), sun ({:.4f} {:.4f} {:.4f}) height {:.2f} deg, "
                       "night F0 {:.3f} smooth {:.4f}, manmade F4 {:.4f} sky {:.4f}, {}, sun/moon light lum {:.4f} ({:.3f} {:.3f} {:.3f}), ambient top {:.4f} bottom {:.4f}, "
                       "fog ({:.4g} {:.4g} {:.4g} {:.4g})",
                       s.hourOk ? std::format("{:.3f}", s.hour) : std::string("?"), s.mgrHour, s.sunrise, s.sunset, s.rampOff[0], s.rampOff[1], s.rampOff[2], s.rampOff[3],
                       s.sun[0], s.sun[1], s.sun[2], SunHeightDeg(s.sun[1]), s.f0, s.smoothNight, s.f4, s.manmade, WeatherText(s), Lum(s.sunMoon), s.sunMoon[0],
                       s.sunMoon[1], s.sunMoon[2], Lum(s.ambTop), Lum(s.ambBottom), s.fog[0], s.fog[1], s.fog[2], s.fog[3]);
}

unsigned long long g_nextLog = 0;
float g_lastF0 = -1.0f;
uintptr_t g_lastMgr = 0;
std::string g_lastFailure;

} // namespace

namespace DaylightProbe {

void OnPresent(uintptr_t lightMgr) {
    if (!DevSwitches::DaylightLog() || !lightMgr) return;
    const unsigned long long now = GetTickCount64();
    Sample s;
    const char* failure = ReadSample(lightMgr, s);
    if (*failure) {
        if (g_lastFailure != failure || lightMgr != g_lastMgr) LOG_WARNING(std::format("[Daylight] Sky not read ({}); lightMgr {:08X}", failure, lightMgr));
        g_lastFailure = failure;
        g_lastMgr = lightMgr;
        return;
    }
    const bool newWorld = lightMgr != g_lastMgr || !g_lastFailure.empty();
    if (newWorld) LOG_INFO(std::format("[Daylight] Sky found at {:08X} (lightMgr {:08X}, back-pointer ok); one line every 5 s and at each step of the night level", s.sky, lightMgr));
    g_lastFailure.clear();
    g_lastMgr = lightMgr;
    const bool step = !newWorld && s.f0 != g_lastF0;
    if (newWorld || step || now >= g_nextLog) {
        LOG_INFO(std::format("[Daylight] {}{}", step ? std::format("night level step {:.3f} -> {:.3f}: ", g_lastF0, s.f0) : std::string(), LineText(s)));
        g_nextLog = now + 5000;
    }
    g_lastF0 = s.f0;
}

std::string SnapshotText(uintptr_t lightMgr, const std::vector<uintptr_t>& lights) {
    if (!DevSwitches::DaylightLog()) return {};
    std::string o = "\n==== DAYLIGHT (development build, daylight_log) ====\n";
    Sample s;
    const char* failure = ReadSample(lightMgr, s);
    if (*failure) return o + std::format("sky not read: {}\n", failure);
    o += std::format("sky {:08X}: {}\n", s.sky, LineText(s));
    // the game's fade per light (+0x20): F4 on channel 2, F0 on channel 3, else 1 (FUN_006BC430); grouped by type and owner
    struct Group {
        int n = 0, on = 0, zero = 0, one = 0, nearF0 = 0, nearF4 = 0;
        float lo = 1e30f, hi = -1e30f;
    };
    std::map<std::pair<int, bool>, Group> groups;
    for (uintptr_t L : lights) {
        int type = -1;
        uint32_t lotLo = 0, lotHi = 0;
        BYTE flags = 0;
        float fade = 0;
        if (!Rd(L + 0xB0, type) || !Rd(L + 0x20, fade)) continue;
        Rd(L + 0xC0, lotLo);
        Rd(L + 0xC4, lotHi);
        Rd(L + 0x100, flags);
        Group& g = groups[{type, (lotLo | lotHi) == 0}];
        g.n++;
        if (flags & 0x20) g.on++;
        if (fade == 0.0f) g.zero++;
        if (fade == 1.0f) g.one++;
        if (std::fabs(fade - s.f0) < 1e-4f) g.nearF0++;
        if (std::fabs(fade - s.f4) < 1e-4f) g.nearF4++;
        if (fade < g.lo) g.lo = fade;
        if (fade > g.hi) g.hi = fade;
    }
    o += std::format("light fade +0x20 by type (F0 {:.4f}, F4 {:.4f}):\n", s.f0, s.f4);
    for (const auto& [key, g] : groups)
        o += std::format("  type {} {}: {} lights, {} on, fade min {:.4f} max {:.4f}, =0 {}, =1 {}, =F0 {}, =F4 {}\n", key.first, key.second ? "street (lot 0)" : "lot", g.n, g.on,
                         g.lo, g.hi, g.zero, g.one, g.nearF0, g.nearF4);
    return o;
}

} // namespace DaylightProbe
