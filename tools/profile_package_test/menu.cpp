#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include "apex_config.h"
#include "apex_presets.h"
#include "apex_version.h"
#include "apex_log.h"
#include "apex_util.h"
#include "ui/widgets.h"
#include "ui/violet_theme.h"
#include "ui/i18n.h"
#include "imgui_internal.h"
namespace ApexLog {void Write(Level,const std::string&,const std::source_location&) noexcept {}}
std::wstring testRoot;
namespace ApexPaths {const std::wstring& ApexDirectory(){return testRoot;} bool EnsureApexDirectory(){std::filesystem::create_directories(testRoot);return true;}}
namespace ApexConfig {
UiSettings GetUi(){return {};}
void CaptureProfileState(toml::table& out) {
    out=toml::parse("[qol.picture]\nenabled=true\n[qol.picture.filters]\nlut=true\nlut_file='An unusually long cinematic color treatment.cube'\n[patches.AmbientOcclusion]\nenabled=true\n[patches.NightTerrainRelight]\nenabled=true\n[patches.DepthBlur]\nenabled=true\n[patches.EdgeSmoothing]\nenabled=true\n[patches.ResourceLookupCache]\nenabled=true\n[shortcuts]\nmenu_key='F11'\n");
}
}
using ApexUi::IconId;
using ApexUi::ButtonKind;
using VioletTheme::Col;
struct Record {std::string name; ImRect rect; bool enabled; float clipBottom;};
std::vector<Record> buttons;
namespace ApexUi {
bool RecordedIconTextButton(const char*,IconId,const char*,ButtonKind);
bool RecordedTextButton(const char*,const char*,ButtonKind,float);
void RecordButton(const char* label) {const auto& item=ImGui::GetCurrentContext()->LastItemData;buttons.push_back({label,item.Rect,!(item.ItemFlags&ImGuiItemFlags_Disabled),ImGui::GetCurrentWindow()->ClipRect.Max.y});}
bool IconTextButton(const char* label,IconId icon,const char* tooltip,ButtonKind kind) {const bool hit=RecordedIconTextButton(label,icon,tooltip,kind);RecordButton(label);return hit;}
bool TextButton(const char* label,const char* tooltip,ButtonKind kind,float width) {const bool hit=RecordedTextButton(label,tooltip,kind,width);RecordButton(label);return hit;}
}
#include "preset_state.inc"
#include "preset_helpers.inc"
#include "preset_footer.inc"
bool Loading(){return false;}
void PickProfileFile(bool,bool,std::string) {}
void SaveProfileNow(const std::string&) {}
void LoadProfileItem(const ProfileItem&,unsigned) {}
void LoadProfileNow(const std::string&,unsigned) {}
void OpenProfilesFolder() {}
#include "preset_page.inc"
#include "preset_raster.inc"
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    testRoot=std::filesystem::absolute(argv[1]).wstring()+L"\\ui-files\\";
    ApexPaths::EnsureApexDirectory();
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;
    VioletTheme::ApplyStyle(ImGui::GetStyle());VioletTheme::LoadFonts(io);
    unsigned char* pixels;int aw,ah;io.Fonts->GetTexDataAsRGBA32(&pixels,&aw,&ah);io.Fonts->SetTexID(1);
    int cases=0;
    for(int lang=0;lang<21;++lang) for(int width:{430,900}) for(float scale:{1.0f,1.4f}) for(int mode=0;mode<10;++mode) {
        I18n::SetChoice(lang);I18n::ClearMissing();g_profiles={};g_profilePickerBusy=false;
        ImGui::GetCurrentContext()->OpenPopupStack.clear();
        g_profiles.list={{"Cinema suave",IconId::Bookmark,ApexConfig::kProfilePartsAll&~ApexConfig::kPartDeveloper,-1}};
        for(int n=0;n<ApexPresets::kCount;++n){const auto& p=ApexPresets::Get(n);toml::table t;ApexPresets::Read(n,t);g_profiles.list.push_back({p.name,ApexUi::IconFromName(p.icon),ApexConfig::ProfilePartsOf(t),n});}
        g_profiles.listDirty=false;io.DisplaySize=ImVec2(float(width),1080);io.DeltaTime=1.f/60;
        ImGui::GetStyle().FontScaleMain=scale;
        for(int frame=0;frame<4;++frame) {
            buttons.clear();ImGui::NewFrame();ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("Presets",nullptr,ImGuiWindowFlags_NoTitleBar);
            if(frame==0 && mode && mode!=9) {
                StartPresetDialog(mode==1?ProfilesState::Save:mode==2||mode==3?ProfilesState::Import:mode==4||mode>=6?ProfilesState::Export:ProfilesState::Apply,mode==5?&g_profiles.list[1]:nullptr);
                if(mode==3){g_profiles.importPath=L"C:\\Cinema suave.zip";ApexConfig::CaptureProfileState(g_profiles.imported.state);g_profiles.available=ApexConfig::kProfilePartsAll&~ApexConfig::kPartDeveloper;g_profiles.saveParts=g_profiles.available&~ApexConfig::kPartShortcuts;std::snprintf(g_profiles.name,sizeof g_profiles.name,"Cinema suave");}
                if(mode==6)g_profilePickerBusy=true;
            }
            if(frame==0 && mode==9) {
                ImGui::PushID("Profiles");ImGui::PushID(g_profiles.list[0].Key().c_str());
                ImGui::OpenPopup("Actions");ImGui::PopID();ImGui::PopID();
            }
            const int stack=ImGui::GetCurrentContext()->StyleVarStack.Size;ApexUi::PageTitle("Presets","Save your setup and switch between them");ProfilesTab();
            if(ImGui::GetCurrentContext()->StyleVarStack.Size!=stack)return 10;
            if(mode>=7) for(auto* window:ImGui::GetCurrentContext()->Windows) {
                if(std::string(window->Name).find("PresetDialog")==std::string::npos)continue;
                const auto content=ImHashStr("##ExportContent",0,window->ID);
                window->StateStorage.SetBool(ImHashStr("##AdvancedOpen",0,content),true);
                const auto optional=ImHashStr("##ExportOptional",0,window->ID);
                window->StateStorage.SetBool(ImHashStr("##AdvancedOpen",0,optional),mode==8);
            }
            ImGui::End();ImGui::Render();
            if(ImGui::GetCurrentContext()->ErrorCountCurrentFrame)return 11;
            if(lang==1 && width==900 && scale==1.0f && frame==3 && (mode==0 || mode==3 || mode==4)) Rasterize(std::filesystem::path(argv[1])/("presets-"+std::to_string(mode)+".ppm"),width,1080);
            if(mode && mode!=9 && frame==3) {
                auto action=std::find_if(buttons.rbegin(),buttons.rend(),[](const auto& r){return r.name=="Save"||r.name=="Import"||r.name=="Export"||r.name=="Apply";});
                if(action==buttons.rend() || action->rect.Max.y>io.DisplaySize.y || action->rect.Max.x>io.DisplaySize.x || action->rect.Min.x<0 || action->rect.Max.y>action->clipBottom+1) {printf("Footer clipped lang=%d width=%d scale=%f mode=%d rect=%f,%f-%f,%f clip=%f\n",lang,width,scale,mode,action->rect.Min.x,action->rect.Min.y,action->rect.Max.x,action->rect.Max.y,action->clipBottom);return 12;}
                if((mode==2||mode==6) && action->enabled) return 13;
                if((mode==1||mode==3||mode==4) && (g_profiles.saveParts&ApexConfig::kPartShortcuts))return 14;
            }
        }
        if(I18n::MissingCount()){printf("Missing translations: %s\n",I18n::MissingList(30).c_str());return 15;}
        ++cases;
    }
    printf("%d native layout cases across 21 languages; four frames each; no stack errors or clipped modal actions\n",cases);
    ImGui::DestroyContext();return 0;
}
