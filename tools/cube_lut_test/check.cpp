#define NOMINMAX
#include "../../features/cube_lut.h"
#include <d3dcompiler.h>
#include <d3d9.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <filesystem>
static int checks=0, failures=0;
void Check(bool ok,const char* why){++checks;if(!ok){++failures;printf("FAIL %s\n",why);}}
std::string Identity(int n){std::ostringstream s;s<<"LUT_3D_SIZE "<<n<<"\n";for(int b=0;b<n;b++)for(int g=0;g<n;g++)for(int r=0;r<n;r++)s<<float(r)/(n-1)<<' '<<float(g)/(n-1)<<' '<<float(b)/(n-1)<<'\n';return s.str();}
bool Parse(const std::string& s,CubeLut::Table& t){std::istringstream in(s);std::string e;return CubeLut::Parse(in,t,e);}
void GpuCheck(const std::string& hlsl){
 HWND wnd=CreateWindowA("STATIC","LUT test",WS_OVERLAPPED,0,0,32,32,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
 IDirect3D9* d3d=Direct3DCreate9(D3D_SDK_VERSION);IDirect3DDevice9* dev=nullptr;
 D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=wnd;pp.BackBufferWidth=16;pp.BackBufferHeight=16;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
 Check(d3d&&SUCCEEDED(d3d->CreateDevice(0,D3DDEVTYPE_HAL,wnd,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&dev)),"native D3D9 device");if(!dev){if(d3d)d3d->Release();DestroyWindow(wnd);return;}
 ID3DBlob *code=nullptr,*error=nullptr;
 auto hr=D3DCompile(hlsl.data(),hlsl.size(),nullptr,nullptr,nullptr,"PicturePS","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
 if(error)error->Release();IDirect3DPixelShader9* full=nullptr;
 Check(SUCCEEDED(hr)&&code&&SUCCEEDED(dev->CreatePixelShader((DWORD*)code->GetBufferPointer(),&full)),"production shader accepted by device");if(full)full->Release();if(code)code->Release();
 const auto helperStart=hlsl.find("float3 CubeFetch(");const auto helperEnd=hlsl.find("float3 SceneTap(",helperStart);
 std::string shader="sampler2D sLut:register(s6);float4 cLut:register(c47);float4 cCubeMin:register(c50);float4 cCubeInv:register(c51);float4 cCubeTex:register(c55);"+hlsl.substr(helperStart,helperEnd-helperStart)+"float4 main(float2 uv:TEXCOORD0):COLOR0{return float4(CubeLookup(float3(uv,0.37)),1);}";
 code=nullptr;error=nullptr;hr=D3DCompile(shader.data(),shader.size(),nullptr,nullptr,nullptr,"main","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);if(error)error->Release();IDirect3DPixelShader9* ps=nullptr;
 Check(SUCCEEDED(hr)&&code&&SUCCEEDED(dev->CreatePixelShader((DWORD*)code->GetBufferPointer(),&ps)),"production cube helper shader");if(code)code->Release();
 D3DCAPS9 caps{};dev->GetDeviceCaps(&caps);
 for(int n:{2,17,33,65})for(int mode=0;mode<3;++mode){
  const auto packed=CubeLut::Pack(n,caps.MaxTextureWidth,caps.MaxTextureHeight);
  const unsigned width=packed.width,height=packed.height;
  IDirect3DTexture9* tex=nullptr;D3DLOCKED_RECT lock{};
  Check(width&&SUCCEEDED(dev->CreateTexture(width,height,1,0,D3DFMT_A32B32G32R32F,D3DPOOL_MANAGED,&tex,nullptr)),"production-packed float32 texture");
  if(!tex||!ps){if(tex)tex->Release();continue;}
  Check(SUCCEEDED(tex->LockRect(0,&lock,nullptr,0)),"texture lock");
  for(int b=0;b<n;b++)for(int g=0;g<n;g++)for(int r=0;r<n;r++){
   size_t index=r+size_t(n)*(g+size_t(n)*b);float* v=(float*)((char*)lock.pBits+(index/width)*lock.Pitch)+(index%width)*4;
   float R=float(r)/(n-1),G=float(g)/(n-1),B=float(b)/(n-1);
   v[0]=mode?R*G:R;v[1]=mode?B*.5f+.1f:G;v[2]=mode?R*R:B;v[3]=1;
  }
  tex->UnlockRect(0);dev->SetTexture(6,tex);
  dev->SetSamplerState(6,D3DSAMP_MINFILTER,D3DTEXF_POINT);dev->SetSamplerState(6,D3DSAMP_MAGFILTER,D3DTEXF_POINT);dev->SetSamplerState(6,D3DSAMP_MIPFILTER,D3DTEXF_NONE);dev->SetSamplerState(6,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);dev->SetSamplerState(6,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
  float size[4]={1,float(n),1,1},lo[4]={0,0,0,0},inv[4]={1,1,1,0},layout[4]={float(width),float(height),1.f/width,1.f/height};
  if(mode){lo[0]=lo[1]=lo[2]=-.2f;inv[0]=inv[1]=inv[2]=1.f/1.4f;}
  if(mode==2){lo[0]=lo[1]=lo[2]=.2f;inv[0]=inv[1]=inv[2]=1.f/.6f;}
  dev->SetPixelShaderConstantF(47,size,1);dev->SetPixelShaderConstantF(50,lo,1);dev->SetPixelShaderConstantF(51,inv,1);dev->SetPixelShaderConstantF(55,layout,1);
  dev->SetPixelShader(ps);dev->SetVertexShader(nullptr);dev->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);dev->SetRenderState(D3DRS_ZENABLE,FALSE);dev->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);dev->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);dev->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);
  struct V{float x,y,z,w,u,v;};V quad[]={{-.5f,-.5f,0,1,0,0},{15.5f,-.5f,0,1,1,0},{-.5f,15.5f,0,1,0,1},{15.5f,15.5f,0,1,1,1}};
  dev->BeginScene();Check(SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(V))),"draw LUT");dev->EndScene();
  IDirect3DSurface9 *bb=nullptr,*read=nullptr;dev->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&bb);dev->CreateOffscreenPlainSurface(16,16,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&read,nullptr);
  Check(bb&&read&&SUCCEEDED(dev->GetRenderTargetData(bb,read)),"LUT readback");
  if(read&&SUCCEEDED(read->LockRect(&lock,nullptr,D3DLOCK_READONLY))){for(int y=0;y<16;y++)for(int x=0;x<16;x++){
   float R=((x+.5f)/16-lo[0])*inv[0],G=((y+.5f)/16-lo[1])*inv[1],B=(.37f-lo[2])*inv[2];
   R=std::clamp(R,0.f,1.f);G=std::clamp(G,0.f,1.f);B=std::clamp(B,0.f,1.f);
   float rp=R*(n-1),r0=std::floor(rp)/(n-1),r1=std::min(std::floor(rp)+1,float(n-1))/(n-1),fraction=rp-std::floor(rp);
   float expected[3]={mode?R*G:R,mode?B*.5f+.1f:G,mode?r0*r0+(r1*r1-r0*r0)*fraction:B};
   auto pixel=(unsigned char*)lock.pBits+y*lock.Pitch+x*4;
   for(int c=0;c<3;c++)Check(std::abs(pixel[2-c]/255.f-expected[c])<1.6f/255,"GPU interpolation matches analytic result");
  }read->UnlockRect();}if(bb)bb->Release();if(read)read->Release();dev->SetTexture(6,nullptr);tex->Release();
 }
 if(ps)ps->Release();dev->Release();d3d->Release();DestroyWindow(wnd);
}
int main(int argc,char** argv){
 CubeLut::Table t;
 for(int n:{2,17,33,65}){
  Check(Parse(Identity(n),t),"supported size");Check(t.values.size()==size_t(n)*n*n,"table length");
  Check(t.values[1][0]>0&&t.values[1][1]==0&&t.values[1][2]==0,"red fastest");
  Check(t.values[n][1]>0&&t.values[n][0]==0,"green next");Check(t.values[n*n][2]>0&&t.values[n*n][1]==0,"blue slowest");
 }
 Check(CubeLut::Pack(65,4096,256).width==0,"oversized packed texture rejected");
 Check(CubeLut::Pack(33,1024,1024).height==64,"33-cell packing");
 Check(CubeLut::Pack(2,1024,1024).width==8,"small cube packing");
 const auto id=Identity(2);
 Check(Parse("\xEF\xBB\xBF# comment\r\nTITLE \"A # title\"\r\nDOMAIN_MIN -1 -2 -3\nDOMAIN_MAX 1 2 3\n"+id,t),"BOM, comments, CRLF and domain");
 Check(t.minimum[1]==-2&&t.inverseRange[1]==.25f,"domain normalization");
 Check(Parse("LUT_3D_INPUT_RANGE -1 1\n"+id,t)&&t.inverseRange[0]==.5f,"Resolve input range");
 Check(Parse("LUT_3D_SIZE 2\n-0.2 1.5 4.57771e-05\n"+id.substr(id.find('\n')+1+6),t)&&t.values[0][0]<0&&t.values[0][1]>1,"float values preserved");
 for(const auto& bad:std::vector<std::string>{"", "LUT_1D_SIZE 8\n", "LUT_3D_SIZE 66\n", "LUT_3D_SIZE 2.5\n", "LUT_3D_SIZE 2\n0 0 0\n", id+"0 0 0\n", "DOMAIN_MIN 1 0 0\n"+id, "DOMAIN_MIN nan 0 0\n"+id, "LUT_3D_SIZE 2\nLUT_3D_SIZE 2\n"+id, "LUT_3D_INPUT_RANGE 0 1\nDOMAIN_MIN 0 0 0\n"+id,"LUT_3D_SIZE 2\nnan 0 0\n", "LUT_3D_SIZE 2\n0 0 0 extra\n",id+"TITLE \"late\"\n"}){
  Check(!Parse(bad,t),"invalid input refused");Check(t.values.empty()&&t.size==0,"failure publishes no table");
 }
 if(argc>1){std::ifstream in(std::filesystem::path(argv[1]),std::ios::binary);std::string e;Check(CubeLut::Parse(in,t,e)&&t.size==33&&t.values.size()==35937,"player Resolve LUT");}
 std::ifstream src("features/picture.cpp");std::string cpp((std::istreambuf_iterator<char>(src)),{});std::string hlsl;size_t pos=0;
 while((pos=cpp.find("R\"HLSL(",pos))!=std::string::npos){pos+=7;auto end=cpp.find(")HLSL\"",pos);hlsl+=cpp.substr(pos,end-pos);pos=end+6;}

 for(const char* entry:{"PicturePS","AdaptPS"}){
 ID3DBlob *code=nullptr,*err=nullptr;auto hr=D3DCompile(hlsl.data(),hlsl.size(),"production picture.hlsl",nullptr,nullptr,entry,"ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&err);
 if(err){printf("%s: %s\n",entry,(char*)err->GetBufferPointer());err->Release();}Check(SUCCEEDED(hr),"production shader compiles ps_3_0");if(code){printf("%s: %zu bytes\n",entry,code->GetBufferSize());
 if(std::string(entry)=="PicturePS"){ID3DBlob* assembly=nullptr;if(SUCCEEDED(D3DDisassemble(code->GetBufferPointer(),code->GetBufferSize(),0,nullptr,&assembly))){std::ofstream out("../../outputs/cube-picture.asm",std::ios::binary);out.write((const char*)assembly->GetBufferPointer(),assembly->GetBufferSize());assembly->Release();}}
 code->Release();}
 }
 GpuCheck(hlsl);
 printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
