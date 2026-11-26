// patches/looks_patch.cpp compiled as is, with Looks::Fx (the player's .fx compiler) and Looks::Packs (downloads)
// replaced by fake data: a look of five runnable shaders (three built in, two of the player's, one with every kind of
// parameter control and quality settings, the other added in the menu and switched off), the second built-in one removed
// in the menu (Restore), and one shader of a pack that is not downloaded.
#include "rec.h"
#include "patches/looks_patch.cpp"
#include "fixture.h"
#include "features/looks_packs_index.h"

namespace {
const LooksParam kEvery[] = {
    { "Strength", "Strength", 1, 0, LooksParamKind::Float, { 0.6f }, 0.0f, 1.0f, nullptr, "How strongly the shader mixes into the picture" },
    { "UseDepth", "Use the depth buffer to skip the sky", 1, 0, LooksParamKind::Bool, { 1 }, 0, 1, nullptr, "Off also shades the sky and far mountains" },
    { "Quality", "Quality", 1, 0, LooksParamKind::Combo, { 1 }, 0, 2, "Low\0Medium\0High\0", "More samples look smoother" },
    { "BlendMode", "Blend mode", 1, 0, LooksParamKind::Combo, { 0 }, 0, 5, "Normal\0Multiply\0Screen\0Overlay\0Soft light\0Linear dodge (add)\0", nullptr },
    { "Tint", "Tint colour", 3, 0, LooksParamKind::Color, { 1.0f, 0.9f, 0.8f }, 0, 1, nullptr, "Multiplied into the highlights" },
    { "Offset", "Centre offset (x, y)", 2, 0, LooksParamKind::Float, { 0.5f, 0.5f }, 0, 1, nullptr, "Where the effect is centred on the screen" },
    { "Weights", "Channel weights", 3, 0, LooksParamKind::Float, { 0.299f, 0.587f, 0.114f }, -1, 1, nullptr, nullptr },
    { "Samples", "Sample count", 1, 0, LooksParamKind::Int, { 16 }, 0, 0, nullptr, "Any whole number", nullptr, "Each sample costs a little speed; most looks use between 8 and 32 samples.", 0, kParamField },
    { "Radius", "Radius", 1, 0, LooksParamKind::Int, { 4 }, 1, 16, nullptr, nullptr, "Advanced", nullptr, 1 },
    { "VeryLongParameterName", "An unusually long parameter label that an author wrote for this control", 1, 0, LooksParamKind::Float, { 2.0f }, 0, 10, nullptr,
      "A long tooltip from the .fx file that explains every detail of what this value changes in the picture", "Advanced" },
    { "Notes", "Notes", 1, 0, LooksParamKind::Combo, { 0 }, 0, 0, nullptr, nullptr, "Advanced", "Made by the author of this shader. Higher values of the radius cost more speed." },
    { "DebugView", "Show the debug view", 1, 0, LooksParamKind::Bool, { 0 }, 0, 1, nullptr, nullptr, "Debug", nullptr, 0, kParamClosed },
    { "DebugScale", "Debug scale (min / max)", 2, 0, LooksParamKind::Float, { 0.0f, 1.0f }, 0, 4, nullptr, nullptr, "Debug", nullptr, 0, kParamClosed },
    { "Timer", "Timer", 1, 0, LooksParamKind::Source, { 0 }, 0, 0, "timer" },
};
const LooksParam kGrain[] = {
    { "Amount", "Grain amount", 1, 0, LooksParamKind::Float, { 0.2f }, 0, 1, nullptr },
    { "Size", "Grain size", 1, 0, LooksParamKind::Float, { 1.6f }, 1, 4, nullptr },
};
const LooksShader kUser[] = {
    { "Everything", "Everything.fx", nullptr, nullptr, kEvery, (int)IM_COUNTOF(kEvery), true, false, "Everything.fx", (LooksShaderKind)0 },
    { "FilmGrain", "MyFilmGrain.fx", nullptr, nullptr, kGrain, (int)IM_COUNTOF(kGrain), false, false, "MyFilmGrain.fx", (LooksShaderKind)0 },
    { "MXAO", "qUINT_mxao.fx", nullptr, nullptr, kGrain, (int)IM_COUNTOF(kGrain), true, false, "qUINT_mxao.fx", (LooksShaderKind)0 }, // twin state only
};
Looks::Fx::Effect g_effect;
bool g_compiling = false;
bool g_twinState = false; // the twin state: qUINT downloaded, so MXAO runs (and Apex's Ambient Occlusion is on, pages.cpp)
int g_batch = 0;

Looks::Packs::Pack MakePack(const char* name, const char* author, const char* folder, uint32_t kb, int techniques, bool inList = true)
{
    Looks::Packs::Pack p;
    p.name = name; p.author = author; p.folder = folder; p.repo = std::string("someone/") + folder; p.branch = "master";
    p.downloadKB = kb; p.techniques = techniques; p.inList = inList;
    return p;
}
const std::vector<Looks::Packs::Pack>& FakePacks()
{
    static const std::vector<Looks::Packs::Pack> v = {
        MakePack("qUINT", "Marty McFly", "qUINT", 1200, 12), MakePack("prod80's shaders", "prod80", "prod80", 3400, 40),
        MakePack("SweetFX", "CeeJay.dk", "SweetFX", 900, 30), MakePack("AstrayFX", "BlueSkyDefender", "AstrayFX", 25000, 22),
        MakePack("Insane Shaders", "Lord of Lunacy", "Insane-Shaders", 800, 9), MakePack("Daodan's shaders", "Daodan317081", "Daodan", 150, 4),
        MakePack("An old pack with a rather long name", "Somebody", "OldPack", 0, 0, false), MakePack("Gone pack", "Nobody", "GonePack", 50, 2),
        MakePack("ReShade standard effects", "crosire", "ReShade standard effects", 600, 50),
    };
    // then the rest of ReShade's list from the built-in index (names only), so Get more packs and Add shaders show it whole
    static const std::vector<Looks::Packs::Pack> all = [] {
        std::vector<Looks::Packs::Pack> out = v;
        for (const Looks::Packs::IndexPack& x : Looks::Packs::kIndex)
        {
            auto same = std::find_if(out.begin(), out.end(), [&](const Looks::Packs::Pack& p) { return _stricmp(p.folder.c_str(), x.folder) == 0; });
            if (same != out.end()) { same->index = &x; continue; }
            Looks::Packs::Pack p = MakePack(x.name, x.author, x.folder, x.downloadKB, x.techniques);
            p.repo = x.repo; p.rank = x.rank; p.index = &x;
            out.push_back(std::move(p));
        }
        return out;
    }();
    return all;
}
// The live viewer (live.cpp): downloads that take about two seconds a pack, one at a time
bool g_live = false;
std::vector<Looks::Packs::Row> g_liveRows;
std::set<std::string> g_liveDone;
double g_liveStarted = 0.0;
int g_edge = 0; // Fixture::EdgeState: 4 = nothing installed (no pack, no file of the player's)
bool IsDownloaded(const std::string& f) { return g_edge != 4 && (f == "prod80" || f == "OldPack" || (g_batch && f == "SweetFX") || g_liveDone.count(f)); }
} // namespace

namespace Looks::Fx {
const LooksShader* UserShader(int i) { return i >= 0 && i < (int)IM_COUNTOF(kUser) ? &kUser[i] : nullptr; }
int FindUser(const std::string& t, const std::string& f)
{
    for (int i = 0; i < (int)IM_COUNTOF(kUser); ++i)
        if (t == kUser[i].technique && _stricmp(f.c_str(), kUser[i].file) == 0 && (i < 2 || g_twinState)) return i;
    return -1;
}
const Effect* UserEffect(int i) { return i == 0 ? &g_effect : nullptr; }
bool MacrosMatch(const Effect&, const LooksDefines&) { return !g_compiling; }
bool HasFile(const std::string& f) { return FindUser("Everything", f) >= 0 || FindUser("FilmGrain", f) >= 0 || FindUser("MXAO", f) >= 0; }
bool Pending(const std::string&) { return false; }
std::vector<Listed> List()
{
    if (g_edge == 4) return {};
    std::vector<Listed> v(2);
    v[0].index = 0; v[0].technique = "Everything"; v[0].file = "Everything.fx"; v[0].pack = "prod80"; v[0].checked = true;
    v[1].index = 1; v[1].technique = "FilmGrain"; v[1].file = "MyFilmGrain.fx"; v[1].checked = true;
    return v;
}
std::vector<std::string> PackFolders() { return g_edge == 4 ? std::vector<std::string>() : std::vector<std::string>{ "prod80", "OldPack" }; }
std::vector<std::wstring> OwnFiles() { return g_edge == 4 ? std::vector<std::wstring>() : std::vector<std::wstring>{ L"C:\\Shaders\\Shaders\\MyFilmGrain.fx" }; }
PackCount CountPack(const std::string&) { return { 31, 40, true }; }
int Generation() { return 1 + g_edge; } // an edge state lists other sources
bool Scanned() { return true; }
bool Scanning() { return false; }
void RequestScan(bool) {}
void SetDefines(const std::map<std::string, LooksDefines>&) {}
void WantFiles(const std::vector<std::string>&) {}
void BrowsePacks() {}
void WantProbe(const std::string&) {}
bool ProbedValues(const std::string&, std::map<std::string, std::set<std::string>>&) { return false; } // not tested: every value shows
void SetAdapter(uint32_t, uint32_t) {}
void SetBindFunctions(SetRenderTargetFn, SetDepthStencilFn) {}
uint64_t GpuBytes() { return 0; }
void ReleaseGpu() {}
bool Run(const RunFrame&, int, const float*, std::string*) { return false; }
bool Poll(UINT, UINT) { return false; }
std::wstring ShadersFolder() { return L"C:\\Shaders\\"; }
bool EnsureShadersFolder() { return true; }
std::map<std::string, std::string> FindFiles(const std::vector<std::string>&) { return {}; }
AddResult AddFiles(const std::vector<std::wstring>&) { return {}; }
std::string Reason(const std::string&, const std::string&) { return {}; }
int CheckedThisSession() { return 0; }
void Instance::Release() {}
bool Instance::Run(IDirect3DDevice9*, size_t, IDirect3DSurface9*, IDirect3DTexture9*, const std::vector<float>&) { return false; }
bool Instance::Create(IDirect3DDevice9*, std::shared_ptr<const Effect>, std::string*) { return false; }
bool DefaultRoom(uint64_t, uint64_t, std::string*) { return true; }
void SetRoomCheck(RoomFn) {}
} // namespace Looks::Fx

namespace Looks::Packs {
#include "failure_text.inc"
const std::vector<Pack>& List() { return FakePacks(); }
void BeginFrame() {}
void RefreshList() {}
ListState CurrentListState() { return ListState::Read; }
const Pack* Find(const std::string& folder)
{
    for (const Pack& p : FakePacks()) if (p.folder == folder) return &p;
    return nullptr;
}
const Pack* PackForFile(const std::string& file, const std::string&) { return _stricmp(file.c_str(), "qUINT_mxao.fx") == 0 ? Find("qUINT") : nullptr; }
std::string FileForTechnique(const std::string& file, const std::string& technique) { return technique == "MXAO" && _stricmp(file.c_str(), "qUINT_mxao.fx") != 0 ? "qUINT_mxao.fx" : ""; }
std::string GitHubUrl(const Pack& p) { return "https://github.com/" + p.repo; }
void Download(const std::vector<std::string>& folders)
{
    if (!g_live) return;
    for (const std::string& f : folders)
        if (!IsDownloaded(f) && std::none_of(g_liveRows.begin(), g_liveRows.end(), [&](const Row& r) { return r.folder == f && r.state != RowState::Failed; }))
            g_liveRows.push_back({ f, RowState::Waiting, 0.0f });
}
void StopRest()
{
    for (Row& r : g_liveRows) if (r.state == RowState::Waiting || r.state == RowState::Downloading) { r.state = RowState::Failed; r.failure = Failure::Stopped; }
}
Batch Status()
{
    Batch b;
    if (g_live)
    {
        const double now = ImGui::GetTime();
        for (Row& r : g_liveRows)
        {
            if (r.state == RowState::Done || r.state == RowState::Failed) continue;
            if (r.state == RowState::Waiting) { r.state = RowState::Downloading; g_liveStarted = now; }
            r.progress = (float)std::min(1.0, (now - g_liveStarted) / 2.0);
            if (r.progress >= 1.0f)
            {
                r.state = RowState::Done;
                g_liveDone.insert(r.folder);
                if (r.folder == "qUINT") { g_twinState = true; ResolveShaders(); } // its MXAO runs now
            }
            break; // one at a time
        }
        b.rows = g_liveRows;
        b.running = std::any_of(b.rows.begin(), b.rows.end(), [](const Row& r) { return r.state == RowState::Waiting || r.state == RowState::Downloading; });
        return b;
    }
    if (!g_batch) return b;
    b.running = g_batch == 1;
    b.rows = { { "SweetFX", RowState::Done, 1.0f }, { "AstrayFX", RowState::Downloading, 0.45f }, { "Insane-Shaders", b.running ? RowState::Waiting : RowState::Failed, 0.0f, b.running ? Failure::None : Failure::Stopped },
               { "Daodan", RowState::Failed, 0.0f, Failure::Offline } };
    if (!b.running) b.rows[1].state = RowState::Done;
    return b;
}
void ClearFinished() { if (g_live) g_liveRows.clear(); }
bool Unavailable(const std::string& folder) { return folder == "GonePack"; }
bool Downloaded(const std::string& folder) { return IsDownloaded(folder); }
void Remove(const std::string&) {}
} // namespace Looks::Packs

extern std::vector<std::unique_ptr<ApexPatch> (*)()> g_factories;
namespace Fixture {
ApexPatch* StartLooks()
{
    static std::unique_ptr<ApexPatch> patch;
    for (auto f : g_factories) patch = f();
    patch->Install();
    Looks::Fx::Macro m;
    g_effect.file = "Everything.fx";
    m.name = "EVERYTHING_QUALITY"; m.value = m.def = "1"; m.lo = 0; m.hi = 3; g_effect.macros.push_back(m);
    m.name = "EVERYTHING_USE_BLOOM"; m.value = m.def = "0"; m.lo = 0; m.hi = 1; g_effect.macros.push_back(m);
    m.name = "EVERYTHING_SAMPLE_RADIUS"; m.value = m.def = "2.5"; m.lo = 0; m.hi = -1; g_effect.macros.push_back(m);
    m.name = "EVERYTHING_VERY_LONG_PREPROCESSOR_DEFINITION_NAME"; m.value = m.def = "BUFFER_WIDTH"; g_effect.macros.push_back(m);
    return patch.get();
}

void LooksState(bool compiling, bool edited, bool offer, int batch, bool twin)
{
    g_compiling = compiling;
    g_twinState = twin;
    g_batch = batch;
    g.preset = "looks:Cinema suave with a long name.ini";
    g.off = { "FilmGrain" };
    g.tweaks.clear();
    g.defs.clear();
    g.extra.clear();
    g.runOrder.clear();
    g.iniOrder.clear();
    for (int s : { 0, 3, 7 }) g.iniOrder.push_back({ Looks::Shader(s).technique, Looks::Shader(s).file, s });
    g.iniOrder.push_back({ "Everything", "Everything.fx", Looks::kUserShaderBase });
    g.iniOrder.push_back({ "MXAO", "qUINT_mxao.fx", g_twinState ? Looks::kUserShaderBase + 2 : -1 });
    g.iniKeys.clear();
    for (auto& e : g.iniOrder) g.iniKeys.insert(Lower(EntryKey(e)));
    g.extra = { "FilmGrain@MyFilmGrain.fx" }; // added in the menu, switched off: the trash button beside its switch
    g.loaded = true;
    g.error.clear();
    g.on.clear();
    RebuildOrder();
    g.removed = { Lower(EntryKey(g.order[1])) }; // removed here: its row with Restore, and the look's changed dot and Save
    if (edited)
    {
        std::vector<float> v = g.iniValues[3];
        v[0] = 0.9f;
        g.tweaks["Everything"] = v;
        g.defs["everything.fx"]["EVERYTHING_QUALITY"] = "3";
    }
    g_offer.reset();
    if (offer) g_offer = ImportOffer{ "Cinema suave", 2, { "qUINT", "AstrayFX" } };
    g_message.clear();
    g_fxMessage.clear();
}

void EdgeState(int edge)
{
    g_edge = edge;
    if (edge == 1) // no look chosen
    {
        g.preset.clear();
        g.loaded = false;
        g.iniOrder.clear();
        g.extra.clear();
        g.removed.clear();
        RebuildOrder();
    }
    else if (edge == 2) // a new, empty look
    {
        g.preset = "looks:My look.ini";
        g.iniOrder.clear();
        g.iniKeys.clear();
        g.extra.clear();
        g.removed.clear();
        g.off.clear();
        RebuildOrder();
    }
    else if (edge == 3) // every built-in shader, a long look name and a long missing shader of a pack not on the list
    {
        g.preset = "looks:A look with a very long name that goes on and on past any width the menu has.ini";
        g.iniOrder.clear();
        for (int s = 0; s < Looks::ShaderCount(); ++s) g.iniOrder.push_back({ Looks::Shader(s).technique, Looks::Shader(s).file, s });
        g.iniOrder.push_back({ "AVeryLongTechniqueNameFromAShaderThatNobodyEverShortened", "SomeoneElsesShaderWithALongFileName_v2_final.fx", -1 });
        g.iniKeys.clear();
        for (auto& e : g.iniOrder) g.iniKeys.insert(Lower(EntryKey(e)));
        g.extra.clear();
        g.removed.clear();
        g.off.clear();
        RebuildOrder();
    }
    else if (edge == 4) g_fxMessage.clear(); // nothing installed: the look as LooksState left it
}

bool SaveRoundTrip()
{
    LooksState(false, false, false, 0);
    bool ok = true;
    auto check = [&](bool c, const char* what) { if (!c) { printf("save round trip: FAIL %s\n", what); ok = false; } };
    const std::string removedKey = EntryKey(g.order[1]), levels = EntryKey(g.order[0]);
    check(Dirty(), "a removed shader is an unsaved change");
    check(IsOff(1) && RunnableCount(false) == 4, "a removed shader no longer runs or counts");
    Looks::Preset p;
    Looks::ParsePreset(LookAsText(), p);
    auto has = [&](const std::string& key) { return std::any_of(p.order.begin(), p.order.end(), [&](const auto& e) { return _stricmp(EntryKey(e).c_str(), key.c_str()) == 0; }); };
    std::string apexOff;
    if (auto top = p.sections.find(""); top != p.sections.end())
        if (auto it = top->second.find("ApexOff"); it != top->second.end()) apexOff = it->second;
    check(has(levels) && !has(removedKey), "Techniques: the shaders on, without the removed one");
    check(!has("FilmGrain@MyFilmGrain.fx") && apexOff == "FilmGrain@MyFilmGrain.fx", "ApexOff: the shader switched off stays listed, not run");
    check(apexOff.find(removedKey) == std::string::npos, "the removed shader is in no list");
    // read back as the look: the switched-off shader is listed off, nothing is unsaved
    g.removed.clear(); g.extra.clear(); g.off.clear();
    g.ini = p;
    g.iniOff.clear();
    g.iniOff.insert(Lower(apexOff));
    g.iniOrder = p.order;
    g.iniOrder.push_back({ "FilmGrain", "MyFilmGrain.fx", Looks::kUserShaderBase + 1 });
    g.iniKeys.clear();
    for (auto& e : g.iniOrder) g.iniKeys.insert(Lower(EntryKey(e)));
    RebuildOrder();
    size_t grain = g.order.size();
    for (size_t i = 0; i < g.order.size(); ++i) if (g.order[i].technique == "FilmGrain") grain = i;
    check(grain < g.order.size() && IsOff(grain) && !Dirty(), "after saving: listed off, nothing unsaved");
    if (grain < g.order.size())
    {
        SwitchEntry(grain, true);
        check(!IsOff(grain) && Dirty(), "switching it on is an unsaved change");
        SwitchEntry(grain, false);
        check(IsOff(grain) && !Dirty(), "and off again is none");
    }
    return ok;
}

void Live() { g_live = true; }

void PacksState(int confirm)
{
    g_libraryConfirm = confirm == 1 ? "p:prod80" : confirm == 2 ? "f:C:\\Shaders\\Shaders\\MyFilmGrain.fx" : "";
}
} // namespace Fixture
