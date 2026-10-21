#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include "ui/widgets.h"
#include "ui/violet_theme.h"
#include "ui/i18n.h"
#include "framework/apex_log.h"
#include "apex_version.h"
#include "build_flavor.h"
#include "imgui_internal.h"
namespace ApexLog {void Write(Level,const std::string&,const std::source_location&) noexcept {}}
namespace ApexUtil { bool FileExists(const std::wstring& p){return GetFileAttributesW(p.c_str())!=INVALID_FILE_ATTRIBUTES;} }
namespace ApexConfig {
struct UiSettings {std::string testBuildSeen;};
UiSettings settings;
UiSettings GetUi(){return settings;} void SetUi(const UiSettings& s){settings=s;}
}
using ApexUi::IconId;using ApexUi::ButtonKind;
ImRect lastButton;bool buttonDrawn=false;
namespace ApexUi {
bool RecordedIconTextButton(const char*,IconId,const char*,ButtonKind);
bool IconTextButton(const char* label,IconId icon,const char* tip,ButtonKind kind){
 bool clicked=RecordedIconTextButton(label,icon,tip,kind);
 if(std::string(label)=="Continue"){lastButton=ImGui::GetCurrentContext()->LastItemData.Rect;buttonDrawn=true;}
 return clicked;
}
}
#include "notice_under_test.inc"

void RasterizeNotice() {
    auto& io=ImGui::GetIO();const int width=static_cast<int>(io.DisplaySize.x),height=static_cast<int>(io.DisplaySize.y);
    unsigned char* atlas=nullptr;int aw=0,ah=0;io.Fonts->GetTexDataAsRGBA32(&atlas,&aw,&ah);
    const int stride=(width*3+3)&~3;std::vector<unsigned char> bgr(static_cast<size_t>(stride)*height,21);
    auto edge=[](ImVec2 a,ImVec2 b,float x,float y){return (b.x-a.x)*(y-a.y)-(b.y-a.y)*(x-a.x);};
    const auto* data=ImGui::GetDrawData();
    for(int list=0;list<data->CmdListsCount;++list){const auto* dl=data->CmdLists[list];
      for(const auto& cmd:dl->CmdBuffer)for(unsigned j=0;j+2<cmd.ElemCount;j+=3){
        const auto& a=dl->VtxBuffer[dl->IdxBuffer[cmd.IdxOffset+j]+cmd.VtxOffset];
        const auto& b=dl->VtxBuffer[dl->IdxBuffer[cmd.IdxOffset+j+1]+cmd.VtxOffset];
        const auto& c=dl->VtxBuffer[dl->IdxBuffer[cmd.IdxOffset+j+2]+cmd.VtxOffset];
        const float area=edge(a.pos,b.pos,c.pos.x,c.pos.y);if(std::abs(area)<.0001f)continue;
        const int x0=std::max({0,int(cmd.ClipRect.x),int(std::floor(std::min({a.pos.x,b.pos.x,c.pos.x})))});
        const int y0=std::max({0,int(cmd.ClipRect.y),int(std::floor(std::min({a.pos.y,b.pos.y,c.pos.y})))});
        const int x1=std::min({width,int(cmd.ClipRect.z),int(std::ceil(std::max({a.pos.x,b.pos.x,c.pos.x})))});
        const int y1=std::min({height,int(cmd.ClipRect.w),int(std::ceil(std::max({a.pos.y,b.pos.y,c.pos.y})))});
        for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x){
          const float wa=edge(b.pos,c.pos,x+.5f,y+.5f)/area,wb=edge(c.pos,a.pos,x+.5f,y+.5f)/area,wc=1-wa-wb;
          if(wa<0||wb<0||wc<0)continue;
          const int tx=std::clamp(int((a.uv.x*wa+b.uv.x*wb+c.uv.x*wc)*aw),0,aw-1);
          const int ty=std::clamp(int((a.uv.y*wa+b.uv.y*wb+c.uv.y*wc)*ah),0,ah-1);
          const float alpha=((a.col>>24)*wa+(b.col>>24)*wb+(c.col>>24)*wc)*atlas[(ty*aw+tx)*4+3]/65025.f;
          for(int channel=0;channel<3;++channel){const int shift=(2-channel)*8;
            const float color=((a.col>>shift)&255)*wa+((b.col>>shift)&255)*wb+((c.col>>shift)&255)*wc;
            auto& dest=bgr[size_t(y)*stride+x*3+channel];dest=static_cast<unsigned char>(color*alpha+dest*(1-alpha));
          }
        }
      }
    }
    BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);file.bfSize=file.bfOffBits+static_cast<DWORD>(bgr.size());
    BITMAPINFOHEADER info{};info.biSize=sizeof info;info.biWidth=width;info.biHeight=-height;info.biPlanes=1;info.biBitCount=24;
    FILE* out=nullptr;fopen_s(&out,"notice-pt.bmp","wb");assert(out);fwrite(&file,sizeof file,1,out);fwrite(&info,sizeof info,1,out);fwrite(bgr.data(),1,bgr.size(),out);fclose(out);
}
int main(){
 ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DeltaTime=1.f/60.f;
 VioletTheme::ApplyStyle(ImGui::GetStyle());VioletTheme::LoadFonts(io);
 unsigned char* pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);io.Fonts->SetTexID(1);
 int cases=0;
 for(int lang=0;lang<static_cast<int>(I18n::Lang::Count);++lang)for(float scale:{1.f,1.5f})for(int width:{430,1000}){
   I18n::SetChoice(lang);ImGui::GetStyle().FontScaleMain=scale;io.DisplaySize=ImVec2(float(width),700);
   ApexConfig::settings.testBuildSeen="2.13.2-test.1";assert(PublicTestPending());
   ImVec2 center;float popupWidth=0;
   for(int frame=0;frame<7;++frame){
     if(frame==3){io.AddMousePosEvent(center.x,center.y);io.AddMouseButtonEvent(0,true);}
     if(frame==4)io.AddMouseButtonEvent(0,false);
     ImGui::NewFrame();ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(io.DisplaySize);
     ImGui::Begin("Host",nullptr,ImGuiWindowFlags_NoSavedSettings);
     const int stack=ImGui::GetCurrentContext()->StyleVarStack.Size;buttonDrawn=false;
     PublicTestWelcome();
     assert(VisibleVersionLabel(1.f)==std::string("v")+APEX_VERSION_STRING);
     assert(VisibleVersionLabel().find(I18n::Tr("RAM and gameplay test"))!=std::string::npos);
     assert(ImGui::GetCurrentContext()->StyleVarStack.Size==stack);
     if(frame<4){
       assert(buttonDrawn);auto* popup=ImGui::FindWindowByName("##PublicTestWelcome");assert(popup);
       assert(lastButton.Min.x>=popup->Pos.x && lastButton.Max.x<=popup->Pos.x+popup->Size.x);
       assert(lastButton.Min.y>=popup->Pos.y && lastButton.Max.y<=popup->Pos.y+popup->Size.y);
       assert(popup->Pos.x>=0 && popup->Pos.y>=0 && popup->Pos.x+popup->Size.x<=io.DisplaySize.x+1);
       assert(popup->Pos.y+popup->Size.y<=io.DisplaySize.y+1);
       if(frame==2){center=lastButton.GetCenter();popupWidth=popup->Size.x;}
       if(frame==3)assert(popup->Size.x==popupWidth);
     }
     ImGui::End();ImGui::Render();assert(!ImGui::GetCurrentContext()->ErrorCountCurrentFrame);
     if(lang==1 && scale==1.f && width==1000 && frame==2) RasterizeNotice();
   }
   assert(ApexConfig::settings.testBuildSeen==APEX_VERSION_STRING && !PublicTestPending());
   assert(VisibleVersionLabel().find(APEX_VERSION_STRING)!=std::string::npos);
   cases++;
 }
 ImGui::DestroyContext();printf("PASS: %d native notice cases, 21 languages, two widths/scales, Continue acknowledgment, bounds and style-stack stability\n",cases);
}
