from pathlib import Path
import sys
root=Path(sys.argv[1])
def part(s,a,b):
    start=s.index(a)
    return s[start:s.index(b,start)]
def replace(s,a,b):
    if a not in s: sys.exit('extract_ui.py: production text changed: '+a)
    return s.replace(a,b)
gui=Path('apex_gui.cpp').read_text(encoding='utf-8')
pages=part(gui,'const char* Description(','bool Loading()')+part(gui,'bool Switchable(','// ApexConfig::ApplyFeatureState')+part(gui,'void SetPatch(','// One feature card:')
looks=part(gui,'void LookLutRow()','// Tab Look: the look card')+part(gui,'void ShadersLookContent()','// Depth Blur and Edge Smoothing')
# the LUT note under the look card reads the Picture feature: never shown here
looks=replace(looks,'PictureParams pp = Picture::Get().GetParams();','PictureParams pp{g_fixtureTwin, g_fixtureTwin, g_fixtureTwin ? "Film.cube" : ""};')
looks='bool LookTwin(const std::string& technique, Looks::Twin& out, bool act);\n'+looks
looks=replace(looks,'Picture::Get().SetParams(pp, true);','')
pages+=looks+part(gui,'bool LooksOn()','void LotStreamingPage()')
root.joinpath('pages.inc').write_text(pages,encoding='utf-8')
packs=Path('features/looks_packs.cpp').read_text(encoding='utf-8')
root.joinpath('failure_text.inc').write_text(part(packs,'const char* FailureText(','Failure Unpack('),encoding='utf-8')
# CPU previews use the same rasterizer as the Report fixture, with synchronous PPM output
render=Path('tools/report_check/menu_253_check.cpp').read_text(encoding='utf-8')
render=render[render.index('void Rasterize('):render.index('\nint main(')]
render=render[:render.index('    { std::lock_guard<std::mutex> lk(Captures::g_lock);')]
# the atlas as the game has it: ImGui 1.92 textures (glyphs rasterised on demand), RGBA32
render=replace(render,'ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&atlas, &aw, &ah);','{ ImTextureData* t = ImGui::GetIO().Fonts->TexData; atlas = t->Pixels; aw = t->Width; ah = t->Height; }')
render+='''    std::ofstream output(file,std::ios::binary);
    output << "P6\\n" << width << " " << height << "\\n255\\n";
    for(size_t i=0;i<bgr.size();i+=3) {output.put(char(bgr[i+2]));output.put(char(bgr[i+1]));output.put(char(bgr[i]));}
}
'''
root.joinpath('raster.inc').write_text(render,encoding='utf-8')
