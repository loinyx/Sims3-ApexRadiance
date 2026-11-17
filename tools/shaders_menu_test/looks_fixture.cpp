// patches/looks_patch.cpp compiled as is, with Looks::Fx (the player's .fx compiler) and Looks::Packs (downloads)
// replaced by fake data: a look of five runnable shaders (three built in, two of the player's, one with every kind of
// parameter control and quality settings, the other added in the menu and switched off) and one shader of a pack that
// is not downloaded.
#include "rec.h"
#include "patches/looks_patch.cpp"
#include "fixture.h"

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
        MakePack("ReShade standard effects", "crosire", kStandardPack, 600, 50),
    };
    return v;
}
bool IsDownloaded(const std::string& f) { return f == "prod80" || f == "OldPack" || (g_batch && f == "SweetFX"); }
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
    std::vector<Listed> v(2);
    v[0].index = 0; v[0].technique = "Everything"; v[0].file = "Everything.fx"; v[0].pack = "prod80"; v[0].checked = true;
    v[1].index = 1; v[1].technique = "FilmGrain"; v[1].file = "MyFilmGrain.fx"; v[1].checked = true;
    return v;
}
std::vector<std::string> PackFolders() { return { "prod80", "OldPack" }; }
std::vector<std::wstring> OwnFiles() { return { L"C:\\Shaders\\Shaders\\MyFilmGrain.fx" }; }
PackCount CountPack(const std::string&) { return { 31, 40, true }; }
int Generation() { return 1; }
bool Scanned() { return true; }
bool Scanning() { return false; }
void RequestScan(bool) {}
void SetDefines(const std::map<std::string, LooksDefines>&) {}
void WantFiles(const std::vector<std::string>&) {}
void BrowsePacks() {}
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
} // namespace Looks::Fx

namespace Looks::Packs {
#include "failure_text.inc"
const std::vector<Pack>& List() { return FakePacks(); }
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
void Download(const std::vector<std::string>&) {}
void StopRest() {}
Batch Status()
{
    Batch b;
    if (!g_batch) return b;
    b.running = g_batch == 1;
    b.rows = { { "SweetFX", RowState::Done, 1.0f }, { "AstrayFX", RowState::Downloading, 0.45f }, { "Insane-Shaders", b.running ? RowState::Waiting : RowState::Failed, 0.0f, b.running ? Failure::None : Failure::Stopped },
               { "Daodan", RowState::Failed, 0.0f, Failure::Offline } };
    if (!b.running) b.rows[1].state = RowState::Done;
    return b;
}
void ClearFinished() {}
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
    RebuildOrder();
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

void PacksState(bool picks, int confirm)
{
    g_packPicks.clear();
    if (picks) g_packPicks = { "qUINT", "SweetFX", "AstrayFX" }; // with the For this look pack; Download adds the standard effects
    g_libraryConfirm = confirm == 1 ? "p:prod80" : confirm == 2 ? "f:C:\\Shaders\\Shaders\\MyFilmGrain.fx" : "";
}
} // namespace Fixture
