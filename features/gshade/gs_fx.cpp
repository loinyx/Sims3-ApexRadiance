// gs_fx: see gs_fx.h. Mirrors ReShade 6.7.3's D3D9 runtime (GShade 5.2.4) for one technique:
// COLOR copied from the back buffer at technique start and after any pass that wrote the back buffer; render targets
// cleared to zero when created; stencil only for full-size passes, cleared at first use; auto-generated mips; vertex
// ids in a float TEXCOORD0 stream; __TEXEL_SIZE__ at VS c255; uniforms in PS/VS constants from c0.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#include "gs_fx.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace gsfx {

const Ann* FindAnn(const std::vector<Ann>& as, const char* name)
{
    for (const Ann& a : as) if (a.name == name) return &a;
    return nullptr;
}

// ------------------------------------------------------------------------------------------------ serialisation
namespace {
struct Out
{
    std::vector<uint8_t>& b;
    void raw(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
    void u8(uint8_t v) { raw(&v, 1); }
    void u32(uint32_t v) { raw(&v, 4); }
    void i32(int32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
    void str(const std::string& s) { u32((uint32_t)s.size()); raw(s.data(), s.size()); }
    void anns(const std::vector<Ann>& as)
    {
        u32((uint32_t)as.size());
        for (const Ann& a : as) { str(a.name); u8(a.isString); str(a.s); for (float f : a.f) f32(f); }
    }
};
struct In
{
    const uint8_t* p; const uint8_t* e; bool ok = true;
    bool need(size_t n) { if ((size_t)(e - p) < n) ok = false; return ok; }
    void raw(void* d, size_t n) { if (need(n)) { memcpy(d, p, n); p += n; } else memset(d, 0, n); }
    uint8_t u8() { uint8_t v = 0; raw(&v, 1); return v; }
    uint32_t u32() { uint32_t v = 0; raw(&v, 4); return v; }
    int32_t i32() { int32_t v = 0; raw(&v, 4); return v; }
    float f32() { float v = 0; raw(&v, 4); return v; }
    std::string str() { uint32_t n = u32(); std::string s; if (need(n)) { s.assign((const char*)p, n); p += n; } return s; }
    uint32_t count(uint32_t max = 1u << 20) { uint32_t n = u32(); if (n > max) { ok = false; return 0; } return n; }
    std::vector<Ann> anns()
    {
        std::vector<Ann> as(count(4096));
        for (Ann& a : as) { a.name = str(); a.isString = u8() != 0; a.s = str(); for (float& f : a.f) f = f32(); }
        return as;
    }
};
const uint32_t kMagic = 0x58465347; // "GSFX"
const uint32_t kVersion = 1;
}

void Write(const Effect& fx, std::vector<uint8_t>& b)
{
    Out o{ b };
    o.u32(kMagic); o.u32(kVersion);
    o.str(fx.file); o.u32(fx.uniformBytes); o.i32(fx.gsW); o.i32(fx.gsH);
    o.u32((uint32_t)fx.uniforms.size());
    for (const Uniform& u : fx.uniforms)
    {
        o.str(u.name); o.u8(u.base); o.u8(u.rows); o.u8(u.cols); o.u32(u.array); o.u32(u.offset); o.u32(u.size);
        o.u32((uint32_t)u.init.size()); for (float f : u.init) o.f32(f);
        o.anns(u.ann);
    }
    o.u32((uint32_t)fx.textures.size());
    for (const Texture& t : fx.textures)
    {
        o.str(t.name); o.str(t.unique); o.str(t.semantic); o.str(t.wExpr); o.str(t.hExpr); o.str(t.image);
        o.u32(t.w); o.u32(t.h); o.u32(t.levels); o.u32(t.format); o.i32(t.psReg); o.i32(t.semReg); o.anns(t.ann);
    }
    o.u32((uint32_t)fx.samplers.size());
    for (const Sampler& s : fx.samplers)
    {
        o.u32(s.texture); o.u32(s.minF); o.u32(s.magF); o.u32(s.mipF); o.u32(s.addrU); o.u32(s.addrV); o.u32(s.addrW);
        o.u8(s.srgb); o.f32(s.lodBias); o.f32(s.minLod);
    }
    o.u32((uint32_t)fx.techniques.size());
    for (const Technique& t : fx.techniques)
    {
        o.str(t.name); o.anns(t.ann); o.u32((uint32_t)t.passes.size());
        for (const Pass& p : t.passes)
        {
            o.str(p.name); o.i32(p.vs); o.i32(p.ps); for (int32_t r : p.rt) o.i32(r);
            o.u8(p.clear); o.u8(p.mips); o.u8(p.srgbWrite); o.u8(p.blend);
            o.u32(p.src); o.u32(p.dst); o.u32(p.op); o.u32(p.srcA); o.u32(p.dstA); o.u32(p.opA);
            for (uint8_t m : p.mask) o.u8(m);
            o.u8(p.stencil); o.u32(p.sRef); o.u32(p.sRead); o.u32(p.sWrite); o.u32(p.sFunc); o.u32(p.sPass); o.u32(p.sFail); o.u32(p.sZFail);
            o.u32(p.prim); o.u32(p.vertices);
            o.u32((uint32_t)p.bind.size()); for (const Binding& bd : p.bind) { o.u32(bd.sampler); o.u32(bd.reg); o.u8(bd.srgb); }
        }
    }
    o.u32((uint32_t)fx.shaders.size());
    for (const Shader& s : fx.shaders) { o.str(s.entry); o.u8(s.vs); o.u32((uint32_t)s.code.size()); o.raw(s.code.data(), s.code.size()); }
}

bool Read(const void* data, size_t size, Effect& fx)
{
    In in{ (const uint8_t*)data, (const uint8_t*)data + size };
    if (in.u32() != kMagic || in.u32() != kVersion) return false;
    fx.file = in.str(); fx.uniformBytes = in.u32(); fx.gsW = in.i32(); fx.gsH = in.i32();
    fx.uniforms.resize(in.count(65536));
    for (Uniform& u : fx.uniforms)
    {
        u.name = in.str(); u.base = in.u8(); u.rows = in.u8(); u.cols = in.u8(); u.array = in.u32(); u.offset = in.u32(); u.size = in.u32();
        u.init.resize(in.count(1 << 16)); for (float& f : u.init) f = in.f32();
        u.ann = in.anns();
    }
    fx.textures.resize(in.count(4096));
    for (Texture& t : fx.textures)
    {
        t.name = in.str(); t.unique = in.str(); t.semantic = in.str(); t.wExpr = in.str(); t.hExpr = in.str(); t.image = in.str();
        t.w = in.u32(); t.h = in.u32(); t.levels = in.u32(); t.format = in.u32(); t.psReg = in.i32(); t.semReg = in.i32(); t.ann = in.anns();
    }
    fx.samplers.resize(in.count(4096));
    for (Sampler& s : fx.samplers)
    {
        s.texture = in.u32(); s.minF = in.u32(); s.magF = in.u32(); s.mipF = in.u32(); s.addrU = in.u32(); s.addrV = in.u32(); s.addrW = in.u32();
        s.srgb = in.u8(); s.lodBias = in.f32(); s.minLod = in.f32();
    }
    fx.techniques.resize(in.count(4096));
    for (Technique& t : fx.techniques)
    {
        t.name = in.str(); t.ann = in.anns(); t.passes.resize(in.count(4096));
        for (Pass& p : t.passes)
        {
            p.name = in.str(); p.vs = in.i32(); p.ps = in.i32(); for (int32_t& r : p.rt) r = in.i32();
            p.clear = in.u8(); p.mips = in.u8(); p.srgbWrite = in.u8(); p.blend = in.u8();
            p.src = in.u32(); p.dst = in.u32(); p.op = in.u32(); p.srcA = in.u32(); p.dstA = in.u32(); p.opA = in.u32();
            for (uint8_t& m : p.mask) m = in.u8();
            p.stencil = in.u8(); p.sRef = in.u32(); p.sRead = in.u32(); p.sWrite = in.u32(); p.sFunc = in.u32(); p.sPass = in.u32(); p.sFail = in.u32(); p.sZFail = in.u32();
            p.prim = in.u32(); p.vertices = in.u32();
            p.bind.resize(in.count(256)); for (Binding& bd : p.bind) { bd.sampler = in.u32(); bd.reg = in.u32(); bd.srgb = in.u8(); }
        }
    }
    fx.shaders.resize(in.count(4096));
    for (Shader& s : fx.shaders)
    {
        s.entry = in.str(); s.vs = in.u8(); s.code.resize(in.count(1u << 24));
        if (!s.code.empty()) in.raw(s.code.data(), s.code.size());
    }
    if (!in.ok) return false;
    // reference checks, so a damaged blob never indexes out of range
    for (const Sampler& s : fx.samplers) if (s.texture >= fx.textures.size()) return false;
    for (const Technique& t : fx.techniques)
        for (const Pass& p : t.passes)
        {
            if (p.vs < 0 || p.ps < 0 || (size_t)p.vs >= fx.shaders.size() || (size_t)p.ps >= fx.shaders.size()) return false;
            for (int32_t r : p.rt) if (r >= (int32_t)fx.textures.size()) return false;
            for (const Binding& bd : p.bind) if (bd.sampler >= fx.samplers.size() || bd.reg >= 16) return false;
            if (p.vertices > (1u << 24)) return false; // float ids stay exact up to 2^24
        }
    for (const Uniform& u : fx.uniforms) if (u.offset / 4 + 4 > 224 * 4) return false;
    return true;
}

// ------------------------------------------------------------------------------------------------ size expressions
namespace {
struct Val { double v = 0; bool isInt = true; };
struct Eval
{
    const char* p; int W, H; bool ok = true;
    void ws() { while (*p && isspace((unsigned char)*p)) ++p; }
    bool eat(const char* t) { ws(); size_t n = strlen(t); if (strncmp(p, t, n) == 0) { p += n; return true; } return false; }
    static Val arith(Val a, Val b, char op)
    {
        Val r; r.isInt = a.isInt && b.isInt;
        switch (op) {
        case '+': r.v = a.v + b.v; break;
        case '-': r.v = a.v - b.v; break;
        case '*': r.v = a.v * b.v; break;
        case '/': r.v = r.isInt ? (b.v != 0 ? std::trunc(a.v / b.v) : 0) : a.v / b.v; break;
        case '%': r.v = r.isInt ? (b.v != 0 ? std::fmod(a.v, b.v) : 0) : std::fmod(a.v, b.v); break;
        }
        return r;
    }
    Val primary()
    {
        ws();
        if (*p == '(')
        {
            ++p; ws();
            // casts: (int) (uint) (float)
            const char* save = p;
            for (const char* c : { "int", "uint", "float" })
            {
                size_t n = strlen(c);
                if (strncmp(p, c, n) == 0) { const char* q = p + n; while (isspace((unsigned char)*q)) ++q; if (*q == ')') { p = q + 1; Val v = unary(); if (c[0] == 'f') v.isInt = false; else { v.v = std::trunc(v.v); v.isInt = true; } return v; } }
            }
            p = save;
            Val v = ternary(); if (!eat(")")) ok = false; return v;
        }
        if (isdigit((unsigned char)*p) || *p == '.')
        {
            char* end = nullptr;
            const char* s = p;
            bool isF = false;
            while (isalnum((unsigned char)*p) || *p == '.' || ((*p == '+' || *p == '-') && (p[-1] == 'e' || p[-1] == 'E') && !(s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))))
            { if (*p == '.' || ((*p == 'e' || *p == 'E') && !(s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) || *p == 'f' || *p == 'F') isF = true; ++p; }
            std::string tok(s, p);
            while (!tok.empty() && (tok.back() == 'f' || tok.back() == 'F' || tok.back() == 'u' || tok.back() == 'U' || tok.back() == 'l' || tok.back() == 'L' || tok.back() == 'h' || tok.back() == 'H'))
            { if (tok.size() > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X') && (tok.back() == 'f' || tok.back() == 'F')) break; tok.pop_back(); }
            if (tok.size() > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) isF = false;
            Val v; v.isInt = !isF; v.v = isF ? strtod(tok.c_str(), &end) : (double)strtoll(tok.c_str(), &end, 0);
            return v;
        }
        if (isalpha((unsigned char)*p) || *p == '_')
        {
            const char* s = p; while (isalnum((unsigned char)*p) || *p == '_') ++p;
            std::string id(s, p);
            if (id == "GS_BW_U") return Val{ (double)W, true };
            if (id == "GS_BH_U") return Val{ (double)H, true };
            if (id == "true") return Val{ 1, true };
            if (id == "false") return Val{ 0, true };
            if (!eat("(")) { ok = false; return Val{}; }
            std::vector<Val> args;
            if (!eat(")")) { do args.push_back(ternary()); while (eat(",")); if (!eat(")")) ok = false; }
            auto a = [&](size_t i) { return i < args.size() ? args[i] : (ok = false, Val{}); };
            if (id == "int" || id == "uint") { Val v = a(0); v.v = std::trunc(v.v); v.isInt = true; return v; }
            if (id == "float") { Val v = a(0); v.isInt = false; return v; }
            if (id == "min") { Val x = a(0), y = a(1); return Val{ std::min(x.v, y.v), x.isInt && y.isInt }; }
            if (id == "max") { Val x = a(0), y = a(1); return Val{ std::max(x.v, y.v), x.isInt && y.isInt }; }
            if (id == "floor") { Val v = a(0); return Val{ std::floor(v.v), v.isInt }; }
            if (id == "ceil") { Val v = a(0); return Val{ std::ceil(v.v), v.isInt }; }
            if (id == "round") { Val v = a(0); return Val{ std::round(v.v), v.isInt }; }
            if (id == "abs") { Val v = a(0); return Val{ std::fabs(v.v), v.isInt }; }
            if (id == "sqrt") { Val v = a(0); return Val{ std::sqrt(v.v), false }; }
            if (id == "log2") { Val v = a(0); return Val{ std::log2(v.v), false }; }
            if (id == "exp2") { Val v = a(0); return Val{ std::exp2(v.v), false }; }
            if (id == "pow") { Val x = a(0), y = a(1); return Val{ std::pow(x.v, y.v), false }; }
            ok = false; return Val{};
        }
        ok = false; return Val{};
    }
    Val unary()
    {
        if (eat("-")) { Val v = unary(); v.v = -v.v; return v; }
        if (eat("+")) return unary();
        if (eat("!")) { Val v = unary(); return Val{ v.v == 0 ? 1.0 : 0.0, true }; }
        if (eat("~")) { Val v = unary(); return Val{ (double)~(long long)std::trunc(v.v), true }; }
        return primary();
    }
    Val mul() { Val v = unary(); for (;;) { ws(); char c = *p; if (c == '*' || c == '/' || c == '%') { ++p; v = arith(v, unary(), c); } else return v; } }
    Val add() { Val v = mul(); for (;;) { ws(); char c = *p; if ((c == '+' || c == '-') && p[1] != c) { ++p; v = arith(v, mul(), c); } else return v; } }
    Val shift()
    {
        Val v = add();
        for (;;)
        {
            if (eat("<<")) { Val b = add(); v.v = std::trunc(v.v) * std::exp2(std::trunc(b.v)); }
            else if (eat(">>")) { Val b = add(); v.v = std::floor(std::trunc(v.v) / std::exp2(std::trunc(b.v))); }
            else return v;
        }
    }
    Val rel()
    {
        Val v = shift();
        for (;;)
        {
            if (eat("<=")) { Val b = shift(); v = Val{ v.v <= b.v ? 1.0 : 0.0, true }; }
            else if (eat(">=")) { Val b = shift(); v = Val{ v.v >= b.v ? 1.0 : 0.0, true }; }
            else if (ws(), *p == '<' && p[1] != '<') { ++p; Val b = shift(); v = Val{ v.v < b.v ? 1.0 : 0.0, true }; }
            else if (ws(), *p == '>' && p[1] != '>') { ++p; Val b = shift(); v = Val{ v.v > b.v ? 1.0 : 0.0, true }; }
            else return v;
        }
    }
    Val eq()
    {
        Val v = rel();
        for (;;)
        {
            if (eat("==")) { Val b = rel(); v = Val{ v.v == b.v ? 1.0 : 0.0, true }; }
            else if (eat("!=")) { Val b = rel(); v = Val{ v.v != b.v ? 1.0 : 0.0, true }; }
            else return v;
        }
    }
    static long long I(const Val& v) { return (long long)std::trunc(v.v); }
    bool single(char c) { ws(); if (*p == c && p[1] != c && p[1] != '=') { ++p; return true; } return false; }
    Val band() { Val v = eq(); while (single('&')) { Val b = eq(); v = Val{ (double)(I(v) & I(b)), true }; } return v; }
    Val bxor() { Val v = band(); while (single('^')) { Val b = band(); v = Val{ (double)(I(v) ^ I(b)), true }; } return v; }
    Val bor() { Val v = bxor(); while (single('|')) { Val b = bxor(); v = Val{ (double)(I(v) | I(b)), true }; } return v; }
    Val land() { Val v = bor(); while (eat("&&")) { Val b = bor(); v = Val{ (v.v != 0 && b.v != 0) ? 1.0 : 0.0, true }; } return v; }
    Val lor() { Val v = land(); while (eat("||")) { Val b = land(); v = Val{ (v.v != 0 || b.v != 0) ? 1.0 : 0.0, true }; } return v; }
    Val ternary()
    {
        Val c = lor();
        if (eat("?")) { Val a = ternary(); if (!eat(":")) ok = false; Val b = ternary(); Val r = c.v != 0 ? a : b; r.isInt = a.isInt && b.isInt; return r; }
        return c;
    }
};
}

double EvalSize(const std::string& expr, int W, int H, bool* ok)
{
    Eval e{ expr.c_str(), W, H };
    Val v = e.ternary();
    e.ws();
    if (*e.p) e.ok = false;
    if (ok) *ok = e.ok;
    return v.v;
}

// ------------------------------------------------------------------------------------------------ uniforms
void SetUniform(const Effect& fx, size_t index, const float* comps, size_t count, std::vector<float>& regs)
{
    if (index >= fx.uniforms.size()) return;
    const Uniform& u = fx.uniforms[index];
    const bool mat = u.cols > 1;
    const unsigned rows = mat ? u.rows : 1u, cols = mat ? u.cols : u.Comps();
    const unsigned per = u.Comps(), elems = std::max(1u, u.array);
    const size_t base = u.offset / 4; // offset is in bytes of the SM3 register file
    for (unsigned e = 0; e < elems; ++e)
        for (unsigned r = 0; r < rows; ++r)
            for (unsigned c = 0; c < cols; ++c)
            {
                const size_t src = (size_t)e * per + r * cols + c;
                const size_t dst = base + ((size_t)e * rows + r) * 4 + c;
                if (src < count && dst < regs.size()) regs[dst] = comps[src];
            }
}

void DefaultValues(const Effect& fx, std::vector<float>& regs)
{
    regs.assign(std::max<size_t>(4, (fx.uniformBytes + 15) / 16 * 4), 0.0f);
    for (size_t i = 0; i < fx.uniforms.size(); ++i)
        SetUniform(fx, i, fx.uniforms[i].init.data(), fx.uniforms[i].init.size(), regs);
}

void UpdateSources(const Effect& fx, std::vector<float>& regs, SourceState& st)
{
    if (st.pingpong.size() != fx.uniforms.size() * 2) st.pingpong.assign(fx.uniforms.size() * 2, 0.0f);
    for (size_t i = 0; i < fx.uniforms.size(); ++i)
    {
        const Uniform& u = fx.uniforms[i];
        const Ann* src = FindAnn(u.ann, "source");
        if (!src || !src->isString) continue;
        const std::string& n = src->s;
        float v[4] = { 0, 0, 0, 0 };
        if (n == "timer") v[0] = st.timerMs;
        else if (n == "frametime") v[0] = st.frametimeMs;
        else if (n == "framecount") v[0] = (float)st.frame;
        else if (st.still) {}
        else if (n == "random")
        {
            const Ann* mn = FindAnn(u.ann, "min"); const Ann* mx = FindAnn(u.ann, "max");
            const int lo = mn ? (int)mn->f[0] : 0, hi = mx ? (int)mx->f[0] : 32767;
            st.rng = st.rng * 1664525u + 1013904223u;
            v[0] = (float)(lo + (hi > lo ? (int)((st.rng >> 8) % (uint32_t)(hi - lo + 1)) : 0));
        }
        else if (n == "pingpong")
        {
            const Ann* mn = FindAnn(u.ann, "min"); const Ann* mx = FindAnn(u.ann, "max"); const Ann* sp = FindAnn(u.ann, "step");
            const float lo = mn ? mn->f[0] : 0.0f, hi = mx ? mx->f[0] : 1.0f, step = sp ? sp->f[0] : 1.0f;
            float& val = st.pingpong[i * 2]; float& dir = st.pingpong[i * 2 + 1];
            if (dir == 0) { dir = 1; val = lo; }
            val += dir * step * st.frametimeMs * 0.001f;
            if (val >= hi) { val = hi; dir = -1; } else if (val <= lo) { val = lo; dir = 1; }
            v[0] = val; v[1] = dir;
        }
        else if (n == "date")
        {
            SYSTEMTIME t; GetLocalTime(&t);
            v[0] = t.wYear; v[1] = t.wMonth; v[2] = t.wDay; v[3] = (float)(t.wHour * 3600 + t.wMinute * 60 + t.wSecond);
        }
        else if (n == "bufready_depth") v[0] = st.depthReady ? 1.0f : 0.0f;
        SetUniform(fx, i, v, std::min<size_t>(4, u.Comps()), regs);
    }
}

// ------------------------------------------------------------------------------------------------ pak
bool ReadPakIndex(const wchar_t* path, PakIndex& out)
{
    FILE* f = _wfopen(path, L"rb");
    if (!f) return false;
    uint32_t hdr[4] = {};
    bool ok = fread(hdr, 4, 4, f) == 4 && hdr[0] == 0x4B505347 /* GSPK */ && hdr[1] == 1 && hdr[2] < 100000 && hdr[3] < 100000;
    auto readList = [&](std::vector<PakEntry>& list, uint32_t n) {
        list.resize(n);
        for (PakEntry& e : list)
        {
            uint32_t len = 0;
            if (!ok || fread(&len, 4, 1, f) != 1 || len > 4096) { ok = false; return; }
            e.name.resize(len);
            if (len && fread(&e.name[0], 1, len, f) != len) { ok = false; return; }
            if (fread(&e.offset, 8, 1, f) != 1 || fread(&e.size, 4, 1, f) != 1) { ok = false; return; }
        }
    };
    if (ok) { readList(out.effects, hdr[2]); readList(out.images, hdr[3]); }
    fclose(f);
    return ok;
}

bool ReadPakBlob(const wchar_t* path, const PakEntry& e, std::vector<uint8_t>& out)
{
    if (e.size > (64u << 20)) return false;
    FILE* f = _wfopen(path, L"rb");
    if (!f) return false;
    out.resize(e.size);
    bool ok = _fseeki64(f, (long long)e.offset, SEEK_SET) == 0 && (e.size == 0 || fread(out.data(), 1, e.size, f) == e.size);
    fclose(f);
    if (!ok) out.clear();
    return ok;
}

// ------------------------------------------------------------------------------------------------ host helpers
std::vector<float> ParseIniValue(const Uniform& u, const std::string& text)
{
    std::vector<float> v = u.init;
    if (v.empty()) v.assign((size_t)u.Comps() * std::max(1u, u.array), 0.0f);
    size_t pos = 0, c = 0;
    while (c < v.size() && pos <= text.size())
    {
        size_t e = text.find(',', pos); if (e == std::string::npos) e = text.size();
        v[c++] = (float)atof(text.substr(pos, e - pos).c_str()); pos = e + 1;
    }
    return v;
}

static bool WicDecode(IWICImagingFactory* f, IWICBitmapDecoder* dec, UINT wantW, UINT wantH, UINT& w, UINT& h, std::vector<uint8_t>& bgra)
{
    IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* conv = nullptr; IWICBitmapScaler* sc = nullptr;
    bool ok = SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&conv))
        && SUCCEEDED(conv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))
        && SUCCEEDED(conv->GetSize(&w, &h));
    IWICBitmapSource* src = conv;
    if (ok && wantW && wantH && (wantW != w || wantH != h))
    {
        ok = SUCCEEDED(f->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(conv, wantW, wantH, WICBitmapInterpolationModeFant));
        src = sc; w = wantW; h = wantH;
    }
    if (ok) { bgra.resize((size_t)w * h * 4); ok = SUCCEEDED(src->CopyPixels(nullptr, w * 4, (UINT)bgra.size(), bgra.data())); }
    if (sc) sc->Release();
    if (conv) conv->Release();
    if (fr) fr->Release();
    return ok;
}

bool DecodeWic(const uint8_t* bytes, size_t size, UINT wantW, UINT wantH, UINT& w, UINT& h, std::vector<uint8_t>& bgra)
{
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    IWICStream* st = nullptr; IWICBitmapDecoder* dec = nullptr;
    bool ok = SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((BYTE*)bytes, (DWORD)size))
        && SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && WicDecode(f, dec, wantW, wantH, w, h, bgra);
    if (dec) dec->Release();
    if (st) st->Release();
    f->Release();
    return ok;
}

static SetRenderTargetFn g_setRT = nullptr;
static SetDepthStencilFn g_setDS = nullptr;
void SetBindFunctions(SetRenderTargetFn rt, SetDepthStencilFn ds) { g_setRT = rt; g_setDS = ds; }
static void BindRT(IDirect3DDevice9* dev, DWORD i, IDirect3DSurface9* s) { if (g_setRT) g_setRT(dev, i, s); else dev->SetRenderTarget(i, s); }
static void BindDS(IDirect3DDevice9* dev, IDirect3DSurface9* s) { if (g_setDS) g_setDS(dev, s); else dev->SetDepthStencilSurface(s); }

// ------------------------------------------------------------------------------------------------ GPU instance
void Instance::Release()
{
    for (IDirect3DTexture9* t : tex_) if (t && t != colorTex_) t->Release();
    tex_.clear(); texW_.clear(); texH_.clear();
    for (IUnknown* s : shaders_) if (s) s->Release();
    shaders_.clear();
    if (colorSurf_) colorSurf_->Release(), colorSurf_ = nullptr;
    if (colorTex_) colorTex_->Release(), colorTex_ = nullptr;
    if (stencil_) stencil_->Release(), stencil_ = nullptr;
    if (decl_) decl_->Release(), decl_ = nullptr;
    if (vb_) vb_->Release(), vb_ = nullptr;
    fx_ = nullptr; W_ = H_ = 0;
}

bool Instance::Create(IDirect3DDevice9* dev, const Effect& fx, UINT W, UINT H, ImageFetchFn fetch, void* fetchCtx, ImageDecodeFn decode, std::string* err)
{
    Release();
    auto fail = [&](const std::string& m) { if (err) *err = m; Release(); return false; };
    W_ = W; H_ = H;
    if (FAILED(dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &colorTex_, nullptr)))
        return fail("colour texture");
    colorTex_->GetSurfaceLevel(0, &colorSurf_);

    tex_.assign(fx.textures.size(), nullptr); texW_.assign(fx.textures.size(), 1); texH_.assign(fx.textures.size(), 1);
    for (size_t i = 0; i < fx.textures.size(); ++i)
    {
        const Texture& t = fx.textures[i];
        if (t.semantic == "COLOR") { tex_[i] = colorTex_; texW_[i] = W; texH_[i] = H; continue; }
        if (!t.semantic.empty()) { texW_[i] = W; texH_[i] = H; continue; } // DEPTH (set at Run) / unsupported semantic: none
        UINT tw = t.w, th = t.h;
        bool ok1 = true, ok2 = true;
        if (!t.wExpr.empty()) tw = (UINT)std::max(1.0, EvalSize(t.wExpr, (int)W, (int)H, &ok1));
        if (!t.hExpr.empty()) th = (UINT)std::max(1.0, EvalSize(t.hExpr, (int)W, (int)H, &ok2));
        if (!ok1 || !ok2) return fail("size of " + t.name);
        tw = std::min<UINT>(tw, 16384); th = std::min<UINT>(th, 16384);
        texW_[i] = tw; texH_[i] = th;
        const DWORD usage = D3DUSAGE_RENDERTARGET | (t.levels > 1 ? D3DUSAGE_AUTOGENMIPMAP : 0);
        if (FAILED(dev->CreateTexture(tw, th, t.levels > 1 ? 0 : 1, usage, (D3DFORMAT)t.format, D3DPOOL_DEFAULT, &tex_[i], nullptr)))
            return fail("texture " + t.name);
        IDirect3DSurface9* s0 = nullptr;
        tex_[i]->GetSurfaceLevel(0, &s0);
        dev->ColorFill(s0, nullptr, 0); // ReShade clears new textures to zero
        if (!t.image.empty())
        {
            std::vector<uint8_t> bytes, img;
            UINT iw = 0, ih = 0;
            if (!fetch || !decode || !fetch(fetchCtx, t.image, bytes) || !decode(bytes.data(), bytes.size(), tw, th, iw, ih, img) || iw != tw || ih != th)
            { s0->Release(); return fail("image " + t.image); }
            IDirect3DSurface9* sys = nullptr;
            if (SUCCEEDED(dev->CreateOffscreenPlainSurface(iw, ih, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)))
            {
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(sys->LockRect(&lr, nullptr, 0)))
                {
                    for (UINT y = 0; y < ih; ++y) memcpy((char*)lr.pBits + (size_t)y * lr.Pitch, &img[(size_t)y * iw * 4], iw * 4);
                    sys->UnlockRect();
                    // non-RGBA8 image targets: D3DX-free conversion through StretchRect is not allowed from system memory,
                    // so those are uploaded via UpdateSurface only when the format matches (the generator flags others)
                    if (t.format == D3DFMT_A8R8G8B8) dev->UpdateSurface(sys, nullptr, s0, nullptr);
                }
                sys->Release();
            }
            if (t.levels > 1) { tex_[i]->SetAutoGenFilterType(D3DTEXF_LINEAR); tex_[i]->GenerateMipSubLevels(); }
        }
        s0->Release();
    }

    shaders_.assign(fx.shaders.size(), nullptr);
    for (size_t i = 0; i < fx.shaders.size(); ++i)
    {
        const Shader& s = fx.shaders[i];
        if (s.code.size() < 8) return fail("shader " + s.entry);
        HRESULT hr;
        if (s.vs) { IDirect3DVertexShader9* v = nullptr; hr = dev->CreateVertexShader((const DWORD*)s.code.data(), &v); shaders_[i] = v; }
        else { IDirect3DPixelShader9* p = nullptr; hr = dev->CreatePixelShader((const DWORD*)s.code.data(), &p); shaders_[i] = p; }
        if (FAILED(hr)) return fail("shader " + s.entry);
    }

    bool needStencil = false;
    UINT maxVerts = 3;
    for (const Technique& t : fx.techniques)
        for (const Pass& p : t.passes) { needStencil |= p.stencil != 0; maxVerts = std::max(maxVerts, p.vertices); }
    if (needStencil && FAILED(dev->CreateDepthStencilSurface(W, H, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &stencil_, nullptr)))
        return fail("stencil");
    const D3DVERTEXELEMENT9 el[] = { { 0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 }, D3DDECL_END() };
    if (FAILED(dev->CreateVertexDeclaration(el, &decl_))) return fail("vertex declaration");
    if (FAILED(dev->CreateVertexBuffer(maxVerts * sizeof(float), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb_, nullptr))) return fail("vertex buffer");
    {
        void* data = nullptr;
        if (FAILED(vb_->Lock(0, 0, &data, 0))) return fail("vertex buffer lock");
        float* f = (float*)data;
        for (UINT i = 0; i < maxVerts; ++i) f[i] = (float)i;
        vb_->Unlock();
    }
    { D3DCAPS9 caps{}; maxPrims_ = SUCCEEDED(dev->GetDeviceCaps(&caps)) ? std::max<UINT>(1, std::min<UINT>(caps.MaxPrimitiveCount, 0x7FFFFF)) : 0xFFFF; }
    fx_ = &fx;
    return true;
}

bool Instance::Run(IDirect3DDevice9* dev, size_t technique, IDirect3DSurface9* target, IDirect3DTexture9* depth, const std::vector<float>& regs)
{
    if (!fx_ || technique >= fx_->techniques.size() || !target) return false;
    const Effect& fx = *fx_;
    const Technique& tech = fx.techniques[technique];

    // constants: uniforms + screen size + pixel sizes (PS c0..223, VS c0..255 with __TEXEL_SIZE__ at c255)
    // States, samplers, textures, render targets, shaders and constants are sent only when they change
    // within the technique (the RTX Remix bridge forwards every D3D9 call to another process). The picture is the same.
    std::vector<float>& c = c_;
    c.assign(256 * 4, 0.0f);
    std::copy(regs.begin(), regs.begin() + std::min(regs.size(), (size_t)224 * 4), c.begin());
    if (fx.gsW >= 0) { const float v = (float)W_; SetUniform(fx, (size_t)fx.gsW, &v, 1, c); }
    if (fx.gsH >= 0) { const float v = (float)H_; SetUniform(fx, (size_t)fx.gsH, &v, 1, c); }
    for (size_t i = 0; i < fx.textures.size(); ++i)
    {
        const Texture& t = fx.textures[i];
        if (t.semReg >= 0 && t.semReg < 224) { c[t.semReg * 4 + 0] = 1.0f / W_; c[t.semReg * 4 + 1] = 1.0f / H_; }
        if (t.psReg >= 0 && t.psReg < 224) { c[t.psReg * 4 + 0] = 1.0f / texW_[i]; c[t.psReg * 4 + 1] = 1.0f / texH_[i]; }
    }

    auto texOf = [&](size_t i) { return fx.textures[i].semantic == "DEPTH" ? depth : tex_[i]; };

    // what this Run has already set (unknown at the start, so everything is sent once)
    std::vector<std::pair<DWORD, DWORD>> rsCache; // (state, value)
    rsCache.reserve(40);
    auto RS = [&](D3DRENDERSTATETYPE st, DWORD v) {
        for (auto& e : rsCache) if (e.first == (DWORD)st) { if (e.second != v) { e.second = v; dev->SetRenderState(st, v); } return; }
        rsCache.push_back({ (DWORD)st, v }); dev->SetRenderState(st, v);
    };
    DWORD ssVal[16][14]; bool ssSet[16][14] = {};
    auto SS = [&](DWORD stage, D3DSAMPLERSTATETYPE st, DWORD v) {
        if (ssSet[stage][st] && ssVal[stage][st] == v) return;
        ssSet[stage][st] = true; ssVal[stage][st] = v; dev->SetSamplerState(stage, st, v);
    };
    IDirect3DBaseTexture9* bound[16] = {};
    for (DWORD st = 0; st < 16; ++st) dev->SetTexture(st, nullptr); // all 16 once per technique, then only changes
    IDirect3DSurface9* boundRt[4] = { (IDirect3DSurface9*)1, nullptr, nullptr, nullptr }; // RT0 unknown; RT1-3 cleared here
    for (DWORD r = 1; r < 4; ++r) BindRT(dev, r, nullptr);
    bool dsKnown = false; IDirect3DSurface9* boundDs = nullptr;
    bool constsSent = false;
    IUnknown* curVs = nullptr; IUnknown* curPs = nullptr;
    dev->SetVertexDeclaration(decl_);

    dev->StretchRect(target, nullptr, colorSurf_, nullptr, D3DTEXF_NONE); // technique start: back buffer -> COLOR
    bool bbDirty = false, stencilCleared = false;
    for (const Pass& pass : tech.passes)
    {
        if (bbDirty) { dev->StretchRect(target, nullptr, colorSurf_, nullptr, D3DTEXF_NONE); bbDirty = false; }

        IDirect3DSurface9* rts[4] = {};
        IDirect3DTexture9* rtTex[4] = {};
        const bool toBB = pass.rt[0] < 0;
        UINT vpw = W_, vph = H_;
        if (toBB) { rts[0] = target; target->AddRef(); }
        else
        {
            for (int r = 0; r < 4 && pass.rt[r] >= 0; ++r)
            {
                rtTex[r] = texOf(pass.rt[r]);
                if (rtTex[r]) rtTex[r]->GetSurfaceLevel(0, &rts[r]);
            }
            if (!rts[0]) { for (auto* s : rts) if (s) s->Release(); continue; }
            vpw = texW_[pass.rt[0]]; vph = texH_[pass.rt[0]];
        }
        for (DWORD r = 0; r < 4; ++r) if (boundRt[r] != rts[r]) { BindRT(dev, r, rts[r]); boundRt[r] = rts[r]; }
        if (pass.clear) for (auto* s : rts) if (s) dev->ColorFill(s, nullptr, 0);
        D3DVIEWPORT9 vp{ 0, 0, vpw, vph, 0.0f, 1.0f };
        dev->SetViewport(&vp); // always: a SetRenderTarget(0) resets it

        const bool useStencil = pass.stencil && stencil_ && vpw == W_ && vph == H_;
        IDirect3DSurface9* wantDs = useStencil ? stencil_ : nullptr;
        if (!dsKnown || boundDs != wantDs) { BindDS(dev, wantDs); boundDs = wantDs; dsKnown = true; }
        if (useStencil && !stencilCleared) { dev->Clear(0, nullptr, D3DCLEAR_STENCIL, 0, 1.0f, 0); stencilCleared = true; }
        RS(D3DRS_ZENABLE, FALSE);
        RS(D3DRS_ZWRITEENABLE, FALSE);
        RS(D3DRS_ALPHATESTENABLE, FALSE);
        RS(D3DRS_FOGENABLE, FALSE);
        RS(D3DRS_STENCILENABLE, useStencil);
        if (useStencil)
        {
            RS(D3DRS_STENCILFUNC, pass.sFunc);
            RS(D3DRS_STENCILPASS, pass.sPass);
            RS(D3DRS_STENCILFAIL, pass.sFail);
            RS(D3DRS_STENCILZFAIL, pass.sZFail);
            RS(D3DRS_STENCILREF, pass.sRef);
            RS(D3DRS_STENCILMASK, pass.sRead);
            RS(D3DRS_STENCILWRITEMASK, pass.sWrite);
        }
        RS(D3DRS_ALPHABLENDENABLE, pass.blend);
        RS(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        RS(D3DRS_SRCBLEND, pass.src);
        RS(D3DRS_DESTBLEND, pass.dst);
        RS(D3DRS_BLENDOP, pass.op);
        RS(D3DRS_SRCBLENDALPHA, pass.srcA);
        RS(D3DRS_DESTBLENDALPHA, pass.dstA);
        RS(D3DRS_BLENDOPALPHA, pass.opA);
        RS(D3DRS_COLORWRITEENABLE, pass.mask[0]);
        RS(D3DRS_COLORWRITEENABLE1, pass.mask[1]);
        RS(D3DRS_COLORWRITEENABLE2, pass.mask[2]);
        RS(D3DRS_COLORWRITEENABLE3, pass.mask[3]);
        RS(D3DRS_SRGBWRITEENABLE, pass.srgbWrite);
        RS(D3DRS_CULLMODE, D3DCULL_NONE);
        RS(D3DRS_SCISSORTESTENABLE, FALSE);
        RS(D3DRS_CLIPPLANEENABLE, 0);

        // textures: this pass's bindings; a stage bound earlier and unused now is cleared (as clearing all 16 did)
        IDirect3DBaseTexture9* want[16] = {};
        for (const Binding& bd : pass.bind)
        {
            const Sampler& s = fx.samplers[bd.sampler];
            IDirect3DTexture9* t = texOf(s.texture);
            for (auto* rt : rtTex) if (rt && rt == t) t = nullptr; // a render target is never sampled in the same pass
            want[bd.reg] = t;
            SS(bd.reg, D3DSAMP_MINFILTER, s.minF);
            SS(bd.reg, D3DSAMP_MAGFILTER, s.magF);
            SS(bd.reg, D3DSAMP_MIPFILTER, s.mipF);
            SS(bd.reg, D3DSAMP_ADDRESSU, s.addrU);
            SS(bd.reg, D3DSAMP_ADDRESSV, s.addrV);
            SS(bd.reg, D3DSAMP_ADDRESSW, s.addrW);
            SS(bd.reg, D3DSAMP_BORDERCOLOR, 0);
            SS(bd.reg, D3DSAMP_SRGBTEXTURE, bd.srgb);
            float bias = s.lodBias; DWORD biasBits; memcpy(&biasBits, &bias, 4);
            SS(bd.reg, D3DSAMP_MIPMAPLODBIAS, biasBits);
            SS(bd.reg, D3DSAMP_MAXMIPLEVEL, s.minLod > 0 ? (DWORD)s.minLod : 0);
        }
        for (DWORD st = 0; st < 16; ++st) if (bound[st] != want[st]) { dev->SetTexture(st, want[st]); bound[st] = want[st]; }

        // constants: the uniforms are the same in every pass, so they go once; only the VS pixel size (c255) follows the viewport
        const float px0 = -1.0f / vpw, px1 = 1.0f / vph;
        if (!constsSent)
        {
            dev->SetPixelShaderConstantF(0, c.data(), 224);
            c[255 * 4 + 0] = px0; c[255 * 4 + 1] = px1;
            dev->SetVertexShaderConstantF(0, c.data(), 256);
            constsSent = true;
        }
        else if (c[255 * 4 + 0] != px0 || c[255 * 4 + 1] != px1)
        {
            c[255 * 4 + 0] = px0; c[255 * 4 + 1] = px1;
            dev->SetVertexShaderConstantF(255, &c[255 * 4], 1);
        }

        if (curVs != shaders_[pass.vs]) { curVs = shaders_[pass.vs]; dev->SetVertexShader((IDirect3DVertexShader9*)curVs); }
        if (curPs != shaders_[pass.ps]) { curPs = shaders_[pass.ps]; dev->SetPixelShader((IDirect3DPixelShader9*)curPs); }
        UINT prims = pass.vertices / 3;
        switch (pass.prim) {
        case D3DPT_POINTLIST: prims = pass.vertices; break;
        case D3DPT_LINELIST: prims = pass.vertices / 2; break;
        case D3DPT_LINESTRIP: prims = pass.vertices > 1 ? pass.vertices - 1 : 0; break;
        case D3DPT_TRIANGLESTRIP: prims = pass.vertices > 2 ? pass.vertices - 2 : 0; break;
        default: break;
        }
        if (prims)
        {
            // one draw where the device allows it; lists split into chunks otherwise (ids stay absolute through StartVertex)
            const UINT maxPrims = maxPrims_;
            const UINT perPrim = pass.prim == D3DPT_POINTLIST ? 1 : pass.prim == D3DPT_LINELIST ? 2 : pass.prim == D3DPT_TRIANGLELIST ? 3 : 0;
            dev->SetStreamSource(0, vb_, 0, sizeof(float));
            if (prims <= maxPrims || !perPrim) dev->DrawPrimitive((D3DPRIMITIVETYPE)pass.prim, 0, std::min(prims, maxPrims));
            else for (UINT first = 0; first < prims; first += maxPrims) dev->DrawPrimitive((D3DPRIMITIVETYPE)pass.prim, first * perPrim, std::min(maxPrims, prims - first));
        }

#ifdef GSFX_DEBUG_DUMP // bench only: raw dump of each pass's first render target to %GS_DUMP_DIR%\passN.bin
        if (const char* dir = getenv("GS_DUMP_DIR"))
        {
            D3DSURFACE_DESC d; rts[0]->GetDesc(&d);
            IDirect3DSurface9* sys = nullptr;
            if (SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)))
            {
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(dev->GetRenderTargetData(rts[0], sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
                {
                    char path[512]; sprintf_s(path, "%s\\pass%02d.bin", dir, (int)(&pass - &tech.passes[0]));
                    if (FILE* f = fopen(path, "wb"))
                    {
                        const uint32_t hdr[4] = { d.Width, d.Height, (uint32_t)d.Format, (uint32_t)lr.Pitch };
                        fwrite(hdr, 4, 4, f);
                        fwrite(lr.pBits, 1, (size_t)lr.Pitch * d.Height, f);
                        fclose(f);
                    }
                    sys->UnlockRect();
                }
                sys->Release();
            }
        }
#endif
        if (toBB) bbDirty = true;
        else if (pass.mips)
            for (auto* t : rtTex) if (t && t->GetLevelCount() > 1) { t->SetAutoGenFilterType(D3DTEXF_LINEAR); t->GenerateMipSubLevels(); }
        for (auto* s : rts) if (s) s->Release();
    }
    dev->SetStreamSource(0, nullptr, 0, 0);
    for (DWORD r = 1; r < 4; ++r) if (boundRt[r]) BindRT(dev, r, nullptr);
    if (boundRt[0] != target) BindRT(dev, 0, target);
    if (!dsKnown || boundDs) BindDS(dev, nullptr);
    for (DWORD st = 0; st < 16; ++st) if (bound[st]) dev->SetTexture(st, nullptr);
    return true;
}

} // namespace gsfx
