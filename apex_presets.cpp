// Built-in profiles (see apex_presets.h).
#include "apex_presets.h"
#include "apex_log.h"
#include <format>

namespace ApexPresets {

namespace {

// Performance: Quality without Ambient Occlusion, Depth Blur and Picture.
constexpr const char* kPerformance = R"toml(
[patches.AmbientOcclusion]
alcance = 0.5360000133514404
distance = 771.8499755859375
enabled = false
forca = 2.0
hairStrength = 0.0
noMapa = true
protegerLuz = 0.4087177813053131
qualidade = 3
revisao = 10
simControls = false
simMaxShade = 0.23517785966396332
simStrength = 0.30582213401794434
transparentHair = true
temporal = true
thinDetail = false
thickness = 0.75

[patches.DepthBlur]
areaNitida = 2
blurSky = true
debugView = false
distancia = 0.3483201563358307
enabled = false
farPlane = 1000.0
focoAuto = false
forca = 1.0
offInMapView = true
qualidade = 1
quantidade = 0.43577075004577637
realceLuzes = true
tamanho = 0.800000011920929
transicao = 0.0613241121172905
velocidadeFoco = 0.10000000149011612

[patches.EdgeSmoothing]
debugView = false
depthEdges = true
enabled = true
metodo = 0
qualidade = 2
qualidadeSmaa = 4
sensibilidade = 0.06300000101327896
sharpen = 1.0
suavidade = 1.0

[patches.FastCacheCompression]
enabled = true

[patches.FastTextureCompression]
enabled = true
useSeveralCores = true

[patches.FileListCache]
enabled = true

[patches.LotLightingMotion]
budgetWhileMovingMs = 1
enabled = true

[patches.NightTerrainRelight]
atrasoSegundos = 2.0
automaticoAoAnoitecer = true
azulNosComodos = 0.0
bordasDosMapasDeLuz = true
brilhoNasRuas = 1.0
brilhoNoChao = 0.75
cercasComLuzDoChao = true
comodosEscurosSemLuz = true
corDasLampadasDoLote = 0.0
corPropriaNoLote = true
enabled = true
janelasLuzDeFora = true
luzRealistaPorAberturas = true
luzSobSacadas = true
paredesBloqueiamLuzNosObjetos = true
paredesBloqueiamLuzNosPisos = true
paredesExternasNaAlturaDesenhada = true
reflexosNeutrosANoite = true
forcaDasLampadasDoLote = 0.800000011920929
forcaDosPostes = 0.800000011920929
forcaLuzPorPixelNosObjetos = 0.75
forcaNasCercas = 0.75
forcaNasParedes = 0.3
forcaNosObjetos = 0.75
forcaNosTelhados = 0.44999998807907104
forcaSobSacadas = 0.45
gramaDoLoteUsaLuzDoLote = true
lampadasEmTodosObjetos = true
luar = 1.817391276359558
luzDasLampadasNatural = 0.2495652139186859
luzDoLoteNaGrama = true
luzDoPosteNaGramaDoLote = true
luzExternaEntreAndares = true
luzInternaEntreAndares = true
luzPorPixelNosObjetos = true
luzQueSobraNosComodos = 0.1200592890381813
mapaDeLuzSuavizado = true
objetosDeForaComLuzDoChao = true
paredesComLuz = true
paredesSemEmendaEntreAndares = true
postesAcesosNoCalculo = true
postesNosObjetos = true
qualidadeAltaEmTodosOsLotes = false
recalcularLotesAoAnoitecer = true
telhadosComLuz = true
todosOsAndaresEmDetalhe = true

[patches.ObjectLookupIndex]
enabled = true

[patches.ResourceLookupCache]
enabled = true

[patches.ResourceLookupMisses]
enabled = true

[patches.SceneDither]
enabled = true
forca = 1.0
graoEmMovimento = false

[patches.SceneNodeBudget]
enabled = true

[patches.SplitLevelGroundLight]
enabled = true

[patches.WallShadingWhileMoving]
enabled = true

[patches.FastCasSort]
enabled = true

[patches.FastMemory]
enabled = true

[patches.MemoryGuard]
enabled = true

[patches.RoomLightQueue]
enabled = true

[patches.ScriptMath]
enabled = true

[patches.WindowRepaint]
enabled = true

[qol.picture]
blacks = 0.0
clarity = 0.0
contrast = 1.0
deband = 0.08
enabled = false
exposure = 0.0
highlight_hue = 40.0
highlight_tint = 0.0
highlights = 0.0
midtones = 1.0
mixer = [ 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 ]
saturation = 1.0
shadow_hue = 215.0
shadow_tint = 0.0
shadows = 0.0
sharpen = 0.0
temperature = 0.0
tint = 0.0
vibrance = 0.0
vignette = 0.0
vignette_size = 0.5
)toml";

// Default: lighter Ambient Occlusion, sharp sky, faster Edge Smoothing.
constexpr const char* kDefaultPreset = R"toml(
[patches.AmbientOcclusion]
alcance = 0.5419999957084656
distance = 752.3499755859375
enabled = true
forca = 1.555999994277954
hairStrength = 0.0
noMapa = true
protegerLuz = 0.4647177457809448
qualidade = 1
revisao = 10
simControls = true
simMaxShade = 0.11
simStrength = 0.4
transparentHair = true
temporal = true
thinDetail = false
thickness = 0.75

[patches.DepthBlur]
areaNitida = 2
blurSky = false
debugView = false
distancia = 0.3483201563358307
enabled = true
farPlane = 1000.0
focoAuto = false
forca = 1.0
offInMapView = true
qualidade = 0
quantidade = 0.43577075004577637
realceLuzes = true
tamanho = 0.800000011920929
transicao = 0.0613241121172905
velocidadeFoco = 0.10000000149011612

[patches.EdgeSmoothing]
debugView = false
depthEdges = true
enabled = true
metodo = 0
qualidade = 1
qualidadeSmaa = 4
sensibilidade = 0.06300000101327896
sharpen = 1.0
suavidade = 1.0

[patches.FastCacheCompression]
enabled = true

[patches.FastTextureCompression]
enabled = true
useSeveralCores = true

[patches.FileListCache]
enabled = true

[patches.LotLightingMotion]
budgetWhileMovingMs = 1
enabled = true

[patches.NightTerrainRelight]
atrasoSegundos = 2.0
automaticoAoAnoitecer = true
azulNosComodos = 0.0
bordasDosMapasDeLuz = true
brilhoNasRuas = 1.0
brilhoNoChao = 0.75
cercasComLuzDoChao = true
comodosEscurosSemLuz = true
corDasLampadasDoLote = 0.0
corPropriaNoLote = true
enabled = true
janelasLuzDeFora = true
luzRealistaPorAberturas = true
luzSobSacadas = true
paredesBloqueiamLuzNosObjetos = true
paredesBloqueiamLuzNosPisos = true
paredesExternasNaAlturaDesenhada = true
reflexosNeutrosANoite = true
forcaDasLampadasDoLote = 0.800000011920929
forcaDosPostes = 0.800000011920929
forcaLuzPorPixelNosObjetos = 0.75
forcaNasCercas = 0.75
forcaNasParedes = 0.3
forcaNosObjetos = 0.75
forcaNosTelhados = 0.44999998807907104
forcaSobSacadas = 0.45
gramaDoLoteUsaLuzDoLote = true
lampadasEmTodosObjetos = true
luar = 1.817391276359558
luzDasLampadasNatural = 0.2495652139186859
luzDoLoteNaGrama = true
luzDoPosteNaGramaDoLote = true
luzExternaEntreAndares = true
luzInternaEntreAndares = true
luzPorPixelNosObjetos = true
luzQueSobraNosComodos = 0.1877470314502716
mapaDeLuzSuavizado = true
objetosDeForaComLuzDoChao = true
paredesComLuz = true
paredesSemEmendaEntreAndares = true
postesAcesosNoCalculo = true
postesNosObjetos = true
qualidadeAltaEmTodosOsLotes = false
recalcularLotesAoAnoitecer = true
telhadosComLuz = true
todosOsAndaresEmDetalhe = true

[patches.ObjectLookupIndex]
enabled = true

[patches.ResourceLookupCache]
enabled = true

[patches.ResourceLookupMisses]
enabled = true

[patches.SceneDither]
enabled = true
forca = 1.0
graoEmMovimento = false

[patches.SceneNodeBudget]
enabled = true

[patches.SplitLevelGroundLight]
enabled = true

[patches.WallShadingWhileMoving]
enabled = true

[patches.FastCasSort]
enabled = true

[patches.FastMemory]
enabled = true

[patches.MemoryGuard]
enabled = true

[patches.RoomLightQueue]
enabled = true

[patches.ScriptMath]
enabled = true

[patches.WindowRepaint]
enabled = true

[qol.picture]
blacks = 0.0
clarity = 0.0
contrast = 1.0
deband = 0.08
enabled = true
exposure = 0.0
highlight_hue = 40.0
highlight_tint = 0.0
highlights = 0.0
midtones = 1.0
mixer = [ 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 ]
saturation = 1.0
shadow_hue = 215.0
shadow_tint = 0.0
shadows = 0.0
sharpen = 0.0
temperature = 0.0
tint = 0.0
vibrance = 0.0
vignette = 0.0
vignette_size = 0.5
)toml";

// Quality: every effect on, at its higher quality.
constexpr const char* kQuality = R"toml(
[patches.AmbientOcclusion]
alcance = 0.5360000133514404
distance = 771.8499755859375
enabled = true
forca = 2.0
hairStrength = 0.0
noMapa = true
protegerLuz = 0.4087177813053131
qualidade = 3
revisao = 10
simControls = true
simMaxShade = 0.11
simStrength = 0.4
transparentHair = true
temporal = true
thinDetail = true
thickness = 0.75

[patches.DepthBlur]
areaNitida = 2
blurSky = true
debugView = false
distancia = 0.3483201563358307
enabled = true
farPlane = 1000.0
focoAuto = false
forca = 1.0
offInMapView = true
qualidade = 1
quantidade = 0.43577075004577637
realceLuzes = true
tamanho = 0.800000011920929
transicao = 0.0613241121172905
velocidadeFoco = 0.10000000149011612

[patches.EdgeSmoothing]
debugView = false
depthEdges = true
enabled = true
metodo = 0
qualidade = 2
qualidadeSmaa = 4
sensibilidade = 0.06300000101327896
sharpen = 1.0
suavidade = 1.0

[patches.FastCacheCompression]
enabled = true

[patches.FastTextureCompression]
enabled = true
useSeveralCores = true

[patches.FileListCache]
enabled = true

[patches.LotLightingMotion]
budgetWhileMovingMs = 1
enabled = true

[patches.NightTerrainRelight]
atrasoSegundos = 2.0
automaticoAoAnoitecer = true
azulNosComodos = 0.0
bordasDosMapasDeLuz = true
brilhoNasRuas = 1.0
brilhoNoChao = 0.75
cercasComLuzDoChao = true
comodosEscurosSemLuz = true
corDasLampadasDoLote = 0.0
corPropriaNoLote = true
enabled = true
janelasLuzDeFora = true
luzRealistaPorAberturas = true
luzSobSacadas = true
paredesBloqueiamLuzNosObjetos = true
paredesBloqueiamLuzNosPisos = true
paredesExternasNaAlturaDesenhada = true
reflexosNeutrosANoite = true
forcaDasLampadasDoLote = 0.800000011920929
forcaDosPostes = 0.800000011920929
forcaLuzPorPixelNosObjetos = 0.75
forcaNasCercas = 0.75
forcaNasParedes = 0.3
forcaNosObjetos = 0.75
forcaNosTelhados = 0.44999998807907104
forcaSobSacadas = 0.45
gramaDoLoteUsaLuzDoLote = true
lampadasEmTodosObjetos = true
luar = 1.817391276359558
luzDasLampadasNatural = 0.2495652139186859
luzDoLoteNaGrama = true
luzDoPosteNaGramaDoLote = true
luzExternaEntreAndares = true
luzInternaEntreAndares = true
luzPorPixelNosObjetos = true
luzQueSobraNosComodos = 0.1200592890381813
mapaDeLuzSuavizado = true
objetosDeForaComLuzDoChao = true
paredesComLuz = true
paredesSemEmendaEntreAndares = true
postesAcesosNoCalculo = true
postesNosObjetos = true
qualidadeAltaEmTodosOsLotes = false
recalcularLotesAoAnoitecer = true
telhadosComLuz = true
todosOsAndaresEmDetalhe = true

[patches.ObjectLookupIndex]
enabled = true

[patches.ResourceLookupCache]
enabled = true

[patches.ResourceLookupMisses]
enabled = true

[patches.SceneDither]
enabled = true
forca = 1.0
graoEmMovimento = false

[patches.SceneNodeBudget]
enabled = true

[patches.SplitLevelGroundLight]
enabled = true

[patches.WallShadingWhileMoving]
enabled = true

[patches.FastCasSort]
enabled = true

[patches.FastMemory]
enabled = true

[patches.MemoryGuard]
enabled = true

[patches.RoomLightQueue]
enabled = true

[patches.ScriptMath]
enabled = true

[patches.WindowRepaint]
enabled = true

[qol.picture]
blacks = 0.0
clarity = 0.0
contrast = 1.0
deband = 0.08
enabled = true
exposure = 0.0
highlight_hue = 40.0
highlight_tint = 0.0
highlights = 0.0
midtones = 1.0
mixer = [ 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 ]
saturation = 1.0
shadow_hue = 215.0
shadow_tint = 0.0
shadows = 0.0
sharpen = 0.0
temperature = 0.0
tint = 0.0
vibrance = 0.0
vignette = 0.0
vignette_size = 0.5
)toml";

const Preset kPresets[kCount] = {
    {"Performance", "More FPS with lighter effects", "rocket", kPerformance},
    {"Default", "A balance of looks and speed", "sun-medium", kDefaultPreset},
    {"Quality", "The best look, for stronger PCs", "aperture", kQuality},
};

} // namespace

const Preset& Get(int index) { return kPresets[index >= 0 && index < kCount ? index : kDefault]; }

bool Read(int index, toml::table& out, std::string* error) {
    const Preset& p = Get(index);
    try {
        out = toml::parse(p.toml);
        return true;
    } catch (const toml::parse_error& e) {
        LOG_ERROR(std::format("[Config] Built-in profile {} could not be read: {}", p.name, e.description()));
        if (error) *error = std::string(e.description());
        return false;
    }
}

} // namespace ApexPresets
