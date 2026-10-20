// Native pixel checks for the public-test basis filter. Credits: @loinyx
#define main RoomMapChecksMain
#include "../room_map_smooth_check/check.cpp"
#undef main

std::vector<DWORD> AssembleBasis() {
    const char* source=R"(ps_3_0
def c0, 0.8944, 0.4472, 0, -0.8944
def c1, 1, 0, 0, 1
dcl_texcoord1 v0.xy
dcl_2d s0
dcl_2d s1
dcl_2d s2
dcl_2d s3
mov r5, c1
texld r0, v0, s0
texld r1, v0, s1
texld r2, v0, s2
texld r3, v0, s3
dp2add_sat r4.x, r5.xy, c0.xy, c0.z
dp2add_sat r4.y, r5.xy, c0.wy, c0.z
dp2add_sat r4.z, r5.yz, c0.yx, c0.z
dp2add_sat r4.w, r5.yz, c0.yw, c0.z
mul r6.xyz, r0, r4.x
mad r6.xyz, r1, r4.y, r6
mad r6.xyz, r2, r4.z, r6
mad r6.xyz, r3, r4.w, r6
mov r6.w, c1.w
mov oC0, r6
)";
    ID3DBlob *code=nullptr,*error=nullptr;
    using Assemble=HRESULT(WINAPI*)(LPCVOID,SIZE_T,LPCSTR,const D3D_SHADER_MACRO*,ID3DInclude*,UINT,ID3DBlob**,ID3DBlob**);
    const auto compiler=LoadLibraryW(L"d3dcompiler_47.dll");
    const auto assemble=compiler?reinterpret_cast<Assemble>(GetProcAddress(compiler,"D3DAssemble")):nullptr;
    const auto hr=assemble?assemble(source,std::strlen(source),nullptr,nullptr,nullptr,0,&code,&error):E_NOTIMPL;
    if(FAILED(hr)&&error) std::printf("%s\n",static_cast<const char*>(error->GetBufferPointer()));
    Check(SUCCEEDED(hr)&&code,"assemble basis fixture");
    std::vector<DWORD> result;
    if(code){result.resize(code->GetBufferSize()/4);std::memcpy(result.data(),code->GetBufferPointer(),code->GetBufferSize());}
    Release(code);Release(error);if(compiler)FreeLibrary(compiler);return result;
}

void ShadowPixels(Gpu& gpu) {
    if(!gpu.dev)return;
    const auto original=AssembleBasis();
    if(original.empty())return;
    auto old=original,guarded=original;
    ShaderPatches::BasisSmoothPatch a,b;
    Check(ShaderPatches::PatchBasisSmooth(old,a),"patch ordinary basis fixture");
    Check(ShaderPatches::PatchBasisSmooth(guarded,b,true),"patch guarded basis fixture");
    IDirect3DPixelShader9 *native=nullptr,*smooth=nullptr,*guard=nullptr;
    Check(SUCCEEDED(gpu.dev->CreatePixelShader(original.data(),&native)),"native fixture shader");
    Check(SUCCEEDED(gpu.dev->CreatePixelShader(old.data(),&smooth)),"smooth fixture shader");
    Check(SUCCEEDED(gpu.dev->CreatePixelShader(guarded.data(),&guard)),"guarded fixture shader");
    if(!native||!smooth||!guard){Release(native);Release(smooth);Release(guard);return;}
    IDirect3DTexture9* tex=nullptr;
    Check(SUCCEEDED(gpu.dev->CreateTexture(8,8,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&tex,nullptr)),"basis fixture map");
    if(!tex){Release(native);Release(smooth);Release(guard);return;}
    for(DWORD s=0;s<4;++s){
        gpu.dev->SetTexture(s,tex);
        gpu.dev->SetSamplerState(s,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);
        gpu.dev->SetSamplerState(s,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
        gpu.dev->SetSamplerState(s,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
        gpu.dev->SetSamplerState(s,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
        gpu.dev->SetSamplerState(s,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
    }
    const float size[4]={8,8,.125f,.125f};
    for(int scenario=0;scenario<3;++scenario){
        D3DLOCKED_RECT lock{};
        Check(SUCCEEDED(tex->LockRect(0,&lock,nullptr,0)),"lock basis fixture");
        if(!lock.pBits)continue;
        for(int y=0;y<8;++y)for(int x=0;x<8;++x){
            const DWORD colour=scenario==1?0xff4080c0u:x<4?(scenario==2?0u:0xff000000u):0xffff0000u;
            reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits)+y*lock.Pitch)[x]=colour;
        }
        tex->UnlockRect(0);
        const auto base=gpu.Render(native);
        gpu.dev->SetPixelShaderConstantF(a.sizeConst,size,1);const auto before=gpu.Render(smooth);
        gpu.dev->SetPixelShaderConstantF(b.sizeConst,size,1);const auto after=gpu.Render(guard);
        int spills=0,remaining=0,overshoot=0,worstFlat=0;
        for(size_t i=0;i<base.size();++i)for(int shift:{0,8,16}){
            const int reference=(base[i]>>shift)&255,previous=(before[i]>>shift)&255,current=(after[i]>>shift)&255;
            if(reference==0){spills+=previous>2;remaining+=current>2;}
            if(scenario==0)overshoot+=current>reference+2;
            if(scenario==1)worstFlat=std::max(worstFlat,std::abs(current-reference));
        }
        std::printf("Scenario %d: old spill channels %d, guarded %d, overshoots %d, flat difference %d LSB\n",scenario,spills,remaining,overshoot,worstFlat);
        if(scenario!=1)Check(spills>0&&remaining==0,"wider filter spill reproduced and removed");
        Check(overshoot==0,"guard never adds light over native opaque read");
        if(scenario==1)Check(worstFlat<=2,"flat interior light preserved");
    }
    for(DWORD s=0;s<4;++s)gpu.dev->SetTexture(s,nullptr);
    Release(tex);Release(native);Release(smooth);Release(guard);
}

int main(int argc,char** argv){
    if(argc<2)return 2;
    Gpu gpu;ShadowPixels(gpu);
    std::ifstream file(argv[1],std::ios::binary);
    std::vector<unsigned char> raw{std::istreambuf_iterator<char>(file),{}};
    std::vector<Blob> blobs;std::vector<Tech> techniques;
    Check(LoadPrecomp(raw,blobs,techniques),"load original game precomp");
    unsigned matched=0,created=0,recovered=0,captures=0;
    std::set<std::vector<DWORD>> captured;
    for(int a=2;a<argc;++a){
        std::error_code error;
        for(auto it=std::filesystem::recursive_directory_iterator(argv[a],error);!error&&it!=std::filesystem::recursive_directory_iterator();it.increment(error)){
            if(!it->is_regular_file()||it->path().extension()!=".bin"||it->path().filename().string().rfind("PS_",0)!=0)continue;
            std::ifstream input(it->path(),std::ios::binary);std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(input),{}};
            if(bytes.size()<8||bytes.size()%4)continue;
            std::vector<DWORD> code(bytes.size()/4);std::memcpy(code.data(),bytes.data(),bytes.size());captured.insert(code);
        }
    }
    auto validate=[&](const std::vector<DWORD>& source){
        auto code=source;ShaderPatches::BasisSmoothPatch patch;
        if(!ShaderPatches::PatchBasisSmooth(code,patch,true))return false;
        ++matched;ID3DBlob* disassembly=nullptr;
        Check(SUCCEEDED(D3DDisassemble(code.data(),code.size()*4,0,nullptr,&disassembly)),"guarded shader disassembly");Release(disassembly);
        const bool valid=gpu.AcceptsPs(code);Check(valid,"guarded shader accepted by native D3D9");created+=valid;
        return true;
    };
    for(const auto& blob:blobs)if(!blob.vs){
        if(!validate(blob.code))continue;
        auto old=blob.code;ShaderPatches::BasisSmoothPatch patch;
        if(ShaderPatches::PatchBasisSmooth(old,patch)&&captured.count(old))++recovered;
    }
    for(const auto& code:captured)captures+=validate(code);
    Check(matched>0&&matched==created,"all matched copies create natively");
    std::printf("Basis copies %u, created %u, original captured matches %u, captured native patterns %u\n%d checks, %d failures\n",matched,created,recovered,captures,g_checks,g_failures);
    return g_failures?1:0;
}
