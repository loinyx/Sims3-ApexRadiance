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
    Check(ShaderPatches::PatchBasisSmooth(guarded,b,{.preserveShadows=true}),"patch guarded basis fixture");
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

// Walls in the directional maps (basis_wall_filter): a lit room (x < 4, red) beside a dark one, a wall between texels 3 and 4
void WallPixels(Gpu& gpu) {
    if(!gpu.dev)return;
    const auto original=AssembleBasis();
    if(original.empty())return;
    auto guarded=original,walled=original;
    ShaderPatches::BasisSmoothPatch b,w;
    Check(ShaderPatches::PatchBasisSmooth(guarded,b,{.preserveShadows=true}),"patch guarded basis fixture (wall test)");
    Check(ShaderPatches::PatchBasisSmooth(walled,w,{.preserveShadows=true,.wallMask=true})&&w.maskSampler>=0,"patch wall-aware basis fixture");
    IDirect3DPixelShader9 *guard=nullptr,*wall=nullptr;
    Check(SUCCEEDED(gpu.dev->CreatePixelShader(guarded.data(),&guard)),"guarded fixture shader (wall test)");
    Check(SUCCEEDED(gpu.dev->CreatePixelShader(walled.data(),&wall)),"wall-aware fixture shader created by native D3D9");
    if(!wall){ // what was made, to see why
        ID3DBlob* text=nullptr;
        if(SUCCEEDED(D3DDisassemble(walled.data(),walled.size()*4,0,nullptr,&text))&&text)std::printf("%.*s\n",static_cast<int>(text->GetBufferSize()),static_cast<const char*>(text->GetBufferPointer()));
        else std::printf("(the wall-aware shader does not disassemble)\n");
        Release(text);
    }
    IDirect3DTexture9 *tex=nullptr,*mask=nullptr;
    gpu.dev->CreateTexture(8,8,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&tex,nullptr);
    gpu.dev->CreateTexture(8,8,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&mask,nullptr);
    if(!guard||!wall||!tex||!mask){Release(guard);Release(wall);Release(tex);Release(mask);return;}
    const auto fill=[](IDirect3DTexture9* t,auto colour){
        D3DLOCKED_RECT lock{};
        if(FAILED(t->LockRect(0,&lock,nullptr,0))||!lock.pBits)return;
        for(int y=0;y<8;++y)for(int x=0;x<8;++x)reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits)+y*lock.Pitch)[x]=colour(x,y);
        t->UnlockRect(0);
    };
    fill(tex,[](int x,int){return x<4?0xffff0000u:0xff000000u;});
    for(DWORD s=0;s<4;++s){
        gpu.dev->SetTexture(s,tex);
        gpu.dev->SetSamplerState(s,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);
        gpu.dev->SetSamplerState(s,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
        gpu.dev->SetSamplerState(s,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
        gpu.dev->SetSamplerState(s,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
        gpu.dev->SetSamplerState(s,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
    }
    const DWORD M=static_cast<DWORD>(w.maskSampler);
    gpu.dev->SetTexture(M,mask);
    gpu.dev->SetSamplerState(M,D3DSAMP_MINFILTER,D3DTEXF_POINT);
    gpu.dev->SetSamplerState(M,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
    gpu.dev->SetSamplerState(M,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
    gpu.dev->SetSamplerState(M,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
    gpu.dev->SetSamplerState(M,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
    const float size[4]={8,8,.125f,.125f};
    gpu.dev->SetPixelShaderConstantF(b.sizeConst,size,1);
    gpu.dev->SetPixelShaderConstantF(w.sizeConst,size,1);
    // no wall in the mask: the same picture as without it
    fill(mask,[](int,int){return 0u;});
    const auto plain=gpu.Render(guard),open=gpu.Render(wall);
    int worstOpen=0;
    for(size_t i=0;i<plain.size();++i)for(int shift:{0,8,16})worstOpen=std::max(worstOpen,std::abs(int((plain[i]>>shift)&255)-int((open[i]>>shift)&255)));
    // the wall between texels 3 and 4: texel 3 walled on +x (g), texel 4 on -x (r)
    fill(mask,[](int x,int){return x==3?0x0000ff00u:x==4?0x00ff0000u:0u;});
    const auto walledPx=gpu.Render(wall);
    int bleedBefore=0,bleedAfter=0,litLost=0;
    for(int y=0;y<64;++y){
        for(int x=32;x<40;++x){ // texel 4, the dark room
            bleedBefore=std::max(bleedBefore,int((plain[y*64+x]>>16)&255));
            bleedAfter=std::max(bleedAfter,int((walledPx[y*64+x]>>16)&255));
        }
        for(int x=24;x<32;++x)litLost=std::max(litLost,int((plain[y*64+x]>>16)&255)-int((walledPx[y*64+x]>>16)&255)); // texel 3, the lit room
    }
    std::printf("Wall: no-wall mask difference %d LSB, red past the wall %d -> %d, lit side lost at most %d LSB\n",worstOpen,bleedBefore,bleedAfter,litLost);
    Check(worstOpen<=1,"an empty wall mask changes nothing");
    Check(bleedBefore>8,"the lit room's red reaches past the wall without the mask (reproduced)");
    Check(bleedAfter<=2,"with the wall in the mask no red reaches past it");
    Check(litLost<=0,"the lit side never gets darker");
    // in the wall's thickness (3 cm each side of its line): the lower of the two texels on both sides. A 2 x 2 map over the
    // 64 pixels: pixel 31 is 0.016 texel before the line, 32 0.016 after it (a stair's edge a millimetre either way)
    IDirect3DTexture9 *tex2=nullptr,*mask2=nullptr;
    gpu.dev->CreateTexture(2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&tex2,nullptr);
    gpu.dev->CreateTexture(2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&mask2,nullptr);
    if(tex2&&mask2){
        const auto fill2=[](IDirect3DTexture9* t,auto colour){
            D3DLOCKED_RECT lock{};
            if(FAILED(t->LockRect(0,&lock,nullptr,0))||!lock.pBits)return;
            for(int y=0;y<2;++y)for(int x=0;x<2;++x)reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits)+y*lock.Pitch)[x]=colour(x,y);
            t->UnlockRect(0);
        };
        fill2(tex2,[](int x,int){return x==0?0xffff0000u:0xff000000u;});
        fill2(mask2,[](int x,int){return x==0?0x0000ff00u:0x00ff0000u;});
        for(DWORD s=0;s<4;++s)gpu.dev->SetTexture(s,tex2);
        gpu.dev->SetTexture(M,mask2);
        const float size2[4]={2,2,.5f,.5f};
        gpu.dev->SetPixelShaderConstantF(w.sizeConst,size2,1);
        const auto band=gpu.Render(wall);
        const auto red=[&](int x){int m=0;for(int y=0;y<64;++y)m=std::max(m,int((band[y*64+x]>>16)&255));return m;};
        std::printf("Wall band: red at pixels 20/30/31/32/33 = %d/%d/%d/%d/%d\n",red(20),red(30),red(31),red(32),red(33));
        Check(red(31)<=2&&red(32)<=2,"in the wall's thickness, on either side of its line, the lower texel (no sliver)");
        gpu.dev->SetPixelShaderConstantF(b.sizeConst,size2,1);
        const auto ref=gpu.Render(guard); // the same maps without the mask
        const int ref20=int((ref[10*64+20]>>16)&255);
        Check(red(30)<=2,"a surface 4.7 cm into the lit texel at a wall (a stair's lip in the wall, as captured) takes the lower texel");
        Check(red(20)>=ref20-1&&red(29)>=ref20-1&&ref20>100,"outside the band the lit room keeps its whole light (as without the mask)");
        Check(red(33)<=2,"the dark room stays dark");
    }
    Release(tex2);Release(mask2);
    for(DWORD s=0;s<4;++s)gpu.dev->SetTexture(s,nullptr);
    gpu.dev->SetTexture(M,nullptr);
    Release(tex);Release(mask);Release(guard);Release(wall);
}

// The floor light map read through the basis uv's zw (a stair's top step, PS 2CB7BD58): a 2 x 2 basis grid (black), a wall
// between its tiles 0 and 1, and an 8 x 8 floor map (4 texels per tile) lit red in tile 0. zwScale: the draw's zw = uv x it.
std::vector<DWORD> RenderUv4(Gpu& gpu,IDirect3DPixelShader9* ps,float zwScale){
    struct Vertex{float x,y,z,rhw,u,v,s,t;};
    const float k=zwScale;
    const Vertex q[4]={{-0.5f,-0.5f,0,1,0,0,0,0},{63.5f,-0.5f,0,1,1,0,k,0},{-0.5f,63.5f,0,1,0,1,0,k},{63.5f,63.5f,0,1,1,1,k,k}};
    gpu.dev->SetVertexShader(nullptr);
    gpu.dev->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1|D3DFVF_TEXCOORDSIZE4(0));
    gpu.dev->SetPixelShader(ps);
    gpu.dev->BeginScene();
    Check(SUCCEEDED(gpu.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,q,sizeof(Vertex))),"floor test quad draw");
    gpu.dev->EndScene();
    std::vector<DWORD> px(4096);
    D3DLOCKED_RECT r{};
    if(SUCCEEDED(gpu.dev->GetRenderTargetData(gpu.target,gpu.readback))&&SUCCEEDED(gpu.readback->LockRect(&r,nullptr,D3DLOCK_READONLY))){
        for(int y=0;y<64;y++)std::memcpy(px.data()+y*64,static_cast<char*>(r.pBits)+y*r.Pitch,256);
        gpu.readback->UnlockRect();
    }
    return px;
}
void FloorPixels(Gpu& gpu){
    if(!gpu.dev)return;
    const char* source=R"(ps_3_0
def c0, 0.8944, 0.4472, 0, -0.8944
def c1, 1, 0, 0, 1
dcl_texcoord v0
dcl_2d s0
dcl_2d s1
dcl_2d s2
dcl_2d s3
dcl_2d s4
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
texld_pp r7, v0.zwzw, s4
add r6.xyz, r6, r7
mov r6.w, c1.w
mov oC0, r6
)";
    ID3DBlob *code=nullptr,*error=nullptr;
    using Assemble=HRESULT(WINAPI*)(LPCVOID,SIZE_T,LPCSTR,const D3D_SHADER_MACRO*,ID3DInclude*,UINT,ID3DBlob**,ID3DBlob**);
    const auto compiler=LoadLibraryW(L"d3dcompiler_47.dll");
    const auto assemble=compiler?reinterpret_cast<Assemble>(GetProcAddress(compiler,"D3DAssemble")):nullptr;
    const auto hr=assemble?assemble(source,std::strlen(source),nullptr,nullptr,nullptr,0,&code,&error):E_NOTIMPL;
    if(FAILED(hr)&&error)std::printf("%s\n",static_cast<const char*>(error->GetBufferPointer()));
    Check(SUCCEEDED(hr)&&code,"assemble floor fixture");
    std::vector<DWORD> original;
    if(code){original.resize(code->GetBufferSize()/4);std::memcpy(original.data(),code->GetBufferPointer(),code->GetBufferSize());}
    Release(code);Release(error);if(compiler)FreeLibrary(compiler);
    if(original.empty())return;
    auto guarded=original,walled=original,noFloor=original;
    ShaderPatches::BasisSmoothPatch b,w,n;
    Check(ShaderPatches::PatchBasisSmooth(guarded,b,{.preserveShadows=true}),"patch floor fixture (guarded)");
    Check(ShaderPatches::PatchBasisSmooth(walled,w,{.preserveShadows=true,.wallMask=true,.floorWall=true})&&w.maskSampler>=0&&w.floorSampler==4&&w.floorConst>=0,"patch floor fixture: the floor read found");
    Check(ShaderPatches::PatchBasisSmooth(noFloor,n,{.preserveShadows=true,.wallMask=true,.floorWall=false})&&n.floorSampler<0,"without floorWall the floor read is left alone");
    IDirect3DPixelShader9 *guard=nullptr,*wall=nullptr;
    Check(SUCCEEDED(gpu.dev->CreatePixelShader(guarded.data(),&guard)),"guarded floor fixture created");
    Check(SUCCEEDED(gpu.dev->CreatePixelShader(walled.data(),&wall)),"wall-aware floor copy created by native D3D9");
    if(!wall){
        ID3DBlob* text=nullptr;
        if(SUCCEEDED(D3DDisassemble(walled.data(),walled.size()*4,0,nullptr,&text))&&text)std::printf("%.*s\n",static_cast<int>(text->GetBufferSize()),static_cast<const char*>(text->GetBufferPointer()));
        Release(text);
    }
    IDirect3DTexture9 *basis=nullptr,*floor=nullptr,*mask=nullptr;
    gpu.dev->CreateTexture(2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&basis,nullptr);
    gpu.dev->CreateTexture(8,8,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&floor,nullptr);
    gpu.dev->CreateTexture(2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&mask,nullptr);
    if(!guard||!wall||!basis||!floor||!mask){Release(guard);Release(wall);Release(basis);Release(floor);Release(mask);return;}
    const auto fill=[](IDirect3DTexture9* t,int size,auto colour){
        D3DLOCKED_RECT lock{};
        if(FAILED(t->LockRect(0,&lock,nullptr,0))||!lock.pBits)return;
        for(int y=0;y<size;++y)for(int x=0;x<size;++x)reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits)+y*lock.Pitch)[x]=colour(x,y);
        t->UnlockRect(0);
    };
    fill(basis,2,[](int,int){return 0xff000000u;});
    fill(floor,8,[](int x,int){return x<4?0xffff0000u:0xff000000u;});
    for(DWORD s=0;s<5;++s){
        gpu.dev->SetTexture(s,s<4?static_cast<IDirect3DBaseTexture9*>(basis):floor);
        gpu.dev->SetSamplerState(s,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);
        gpu.dev->SetSamplerState(s,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
        gpu.dev->SetSamplerState(s,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
        gpu.dev->SetSamplerState(s,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
        gpu.dev->SetSamplerState(s,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
    }
    const DWORD M=static_cast<DWORD>(w.maskSampler);
    gpu.dev->SetTexture(M,mask);
    gpu.dev->SetSamplerState(M,D3DSAMP_MINFILTER,D3DTEXF_POINT);
    gpu.dev->SetSamplerState(M,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
    gpu.dev->SetSamplerState(M,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
    gpu.dev->SetSamplerState(M,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
    gpu.dev->SetSamplerState(M,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
    const float size[4]={2,2,.5f,.5f};
    gpu.dev->SetPixelShaderConstantF(b.sizeConst,size,1);
    gpu.dev->SetPixelShaderConstantF(w.sizeConst,size,1);
    const float k[4]={4.0f*2/8,4.0f*2/8,1,0}; // the draw code's (4 x basis size / floor size): 1
    gpu.dev->SetPixelShaderConstantF(static_cast<UINT>(w.floorConst),k,1);
    const auto red=[](const std::vector<DWORD>& p,int x){int m=0;for(int y=0;y<64;++y)m=std::max(m,int((p[y*64+x]>>16)&255));return m;};
    const auto worst=[](const std::vector<DWORD>& a,const std::vector<DWORD>& c){int m=0;for(size_t i=0;i<a.size();++i)for(int sh:{0,8,16})m=std::max(m,std::abs(int((a[i]>>sh)&255)-int((c[i]>>sh)&255)));return m;};
    fill(mask,2,[](int,int){return 0u;});
    const auto plain=RenderUv4(gpu,guard,1),open=RenderUv4(gpu,wall,1);
    // the wall between tiles 0 and 1: tile 0 walled on +x (g), tile 1 on -x (r)
    fill(mask,2,[](int x,int){return x==0?0x0000ff00u:0x00ff0000u;});
    const auto walledPx=RenderUv4(gpu,wall,1);
    std::printf("Floor read: red at pixels 20/30/31/32/33/36 without the mask %d/%d/%d/%d/%d/%d, with it %d/%d/%d/%d/%d/%d\n",red(plain,20),red(plain,30),red(plain,31),red(plain,32),red(plain,33),red(plain,36),
                red(walledPx,20),red(walledPx,30),red(walledPx,31),red(walledPx,32),red(walledPx,33),red(walledPx,36));
    Check(worst(plain,open)<=1,"floor read: an empty wall mask changes nothing");
    Check(red(plain,32)>50&&red(plain,33)>20,"floor read: the lit room's red reaches past the wall without the mask (reproduced)");
    int past=0;for(int x=32;x<64;++x)past=std::max(past,red(walledPx,x));
    Check(past<=2,"floor read: with the wall in the mask no red past the wall line");
    Check(red(walledPx,31)<=2,"floor read: in the wall's thickness the lower texel (no sliver)");
    int lost=0;for(int x=0;x<30;++x)lost=std::max(lost,red(plain,x)-red(walledPx,x));
    Check(lost<=0&&red(walledPx,29)>=250,"floor read: the lit side keeps its own texel up to the band");
    Check(red(walledPx,30)<=2,"floor read: 4.7 cm into the lit tile at a wall (the stair's lip) the lower texel");
    // the draw's zw not the basis uv x cG.xy (another VS): the game's read
    const auto plainHalf=RenderUv4(gpu,guard,0.5f),wallHalf=RenderUv4(gpu,wall,0.5f);
    Check(worst(plainHalf,wallHalf)<=1,"floor read: a zw that is not the expected uv keeps the game's read");
    const float off[4]={1,1,0,0};
    gpu.dev->SetPixelShaderConstantF(static_cast<UINT>(w.floorConst),off,1);
    Check(worst(plain,RenderUv4(gpu,wall,1))<=1,"floor read: off (no floor map size) keeps the game's read");
    for(DWORD s=0;s<5;++s)gpu.dev->SetTexture(s,nullptr);
    gpu.dev->SetTexture(M,nullptr);
    Release(basis);Release(floor);Release(mask);Release(guard);Release(wall);
}

// The fill of a stair (a capture: the stair beside a green floor was grey-brown, (18, 20, 7) against (2, 19, 1):
// its ambient cube fill had no room colour). A basis fixture with a cube fill (x c2.w, maxed in) and the floor read: patched
// with stairRoomFill, the fill is the tile's room ambient x the floor's alpha and the floor read takes map.rgb + map.a x ambient.
void FillPixels(Gpu& gpu){
    if(!gpu.dev)return;
    const char* source=R"(ps_3_0
def c0, 0.8944, 0.4472, 0, -0.8944
def c1, 1, 0, 0, 1
def c2, 0, 0, 0, 0.25
dcl_texcoord v0
dcl_2d s0
dcl_2d s1
dcl_2d s2
dcl_2d s3
dcl_2d s4
dcl_cube s5
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
texld r8, r5, s5
mul r8.xyz, r8, c2.w
max r6.xyz, r6, r8
texld_pp r7, v0.zwzw, s4
add r6.xyz, r6, r7
mov r6.w, c1.w
mov oC0, r6
)";
    ID3DBlob *code=nullptr,*error=nullptr;
    using Assemble=HRESULT(WINAPI*)(LPCVOID,SIZE_T,LPCSTR,const D3D_SHADER_MACRO*,ID3DInclude*,UINT,ID3DBlob**,ID3DBlob**);
    const auto compiler=LoadLibraryW(L"d3dcompiler_47.dll");
    const auto assemble=compiler?reinterpret_cast<Assemble>(GetProcAddress(compiler,"D3DAssemble")):nullptr;
    const auto hr=assemble?assemble(source,std::strlen(source),nullptr,nullptr,nullptr,0,&code,&error):E_NOTIMPL;
    if(FAILED(hr)&&error)std::printf("%s\n",static_cast<const char*>(error->GetBufferPointer()));
    Check(SUCCEEDED(hr)&&code,"assemble fill fixture");
    std::vector<DWORD> original;
    if(code){original.resize(code->GetBufferSize()/4);std::memcpy(original.data(),code->GetBufferPointer(),code->GetBufferSize());}
    Release(code);Release(error);if(compiler)FreeLibrary(compiler);
    if(original.empty())return;
    auto guarded=original,filled=original,walledOnly=original;
    ShaderPatches::BasisSmoothPatch b,f,w;
    Check(ShaderPatches::PatchBasisSmooth(guarded,b,{.preserveShadows=true}),"patch fill fixture (guarded)");
    Check(ShaderPatches::PatchBasisSmooth(walledOnly,w,{.preserveShadows=true,.wallMask=true,.floorWall=true,.stairRoomFill=false})&&w.ambSampler<0,"without stairRoomFill no ambient sampler");
    Check(ShaderPatches::PatchBasisSmooth(filled,f,{.preserveShadows=true,.wallMask=true,.floorWall=true,.stairRoomFill=true})&&f.ambSampler>=0&&f.floorSampler==4,"stairRoomFill: the cube fill and the floor read found");
    IDirect3DPixelShader9 *guard=nullptr,*fill=nullptr;
    Check(SUCCEEDED(gpu.dev->CreatePixelShader(guarded.data(),&guard)),"guarded fill fixture created");
    Check(SUCCEEDED(gpu.dev->CreatePixelShader(filled.data(),&fill)),"room-fill copy created by native D3D9");
    if(!fill){
        ID3DBlob* text=nullptr;
        if(SUCCEEDED(D3DDisassemble(filled.data(),filled.size()*4,0,nullptr,&text))&&text)std::printf("%.*s\n",static_cast<int>(text->GetBufferSize()),static_cast<const char*>(text->GetBufferPointer()));
        Release(text);
    }
    IDirect3DTexture9 *basis=nullptr,*floor=nullptr,*mask=nullptr,*amb=nullptr;IDirect3DCubeTexture9* cube=nullptr;
    gpu.dev->CreateTexture(2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&basis,nullptr);
    gpu.dev->CreateTexture(8,8,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&floor,nullptr);
    gpu.dev->CreateTexture(2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&mask,nullptr);
    gpu.dev->CreateTexture(2,2,1,0,D3DFMT_A32B32G32R32F,D3DPOOL_MANAGED,&amb,nullptr);
    gpu.dev->CreateCubeTexture(1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&cube,nullptr);
    if(!guard||!fill||!basis||!floor||!mask||!amb||!cube){Release(guard);Release(fill);Release(basis);Release(floor);Release(mask);Release(amb);Release(cube);Check(false,"fill test resources");return;}
    const auto fillTex=[](IDirect3DTexture9* t,int size,DWORD colour){
        D3DLOCKED_RECT lock{};
        if(FAILED(t->LockRect(0,&lock,nullptr,0))||!lock.pBits)return;
        for(int y=0;y<size;++y)for(int x=0;x<size;++x)reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits)+y*lock.Pitch)[x]=colour;
        t->UnlockRect(0);
    };
    fillTex(basis,2,0xff000000u);
    fillTex(floor,8,0x80003300u); // floor map: rgb (0, 0.2, 0), alpha 0.5
    fillTex(mask,2,0u);
    {
        D3DLOCKED_RECT lock{};
        if(SUCCEEDED(amb->LockRect(0,&lock,nullptr,0))&&lock.pBits){
            for(int y=0;y<2;++y)for(int x=0;x<2;++x){float* p=reinterpret_cast<float*>(static_cast<char*>(lock.pBits)+y*lock.Pitch)+x*4;p[0]=0.02f;p[1]=0.1f;p[2]=0.02f;p[3]=1.0f;}
            amb->UnlockRect(0);
        }
        for(int face=0;face<6;++face)if(SUCCEEDED(cube->LockRect(static_cast<D3DCUBEMAP_FACES>(face),0,&lock,nullptr,0))&&lock.pBits){*static_cast<DWORD*>(lock.pBits)=0xffffffffu;cube->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face),0);}
    }
    for(DWORD s=0;s<5;++s){
        gpu.dev->SetTexture(s,s<4?static_cast<IDirect3DBaseTexture9*>(basis):floor);
        gpu.dev->SetSamplerState(s,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);
        gpu.dev->SetSamplerState(s,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
        gpu.dev->SetSamplerState(s,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
        gpu.dev->SetSamplerState(s,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
        gpu.dev->SetSamplerState(s,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
    }
    gpu.dev->SetTexture(5,cube);
    for(DWORD s:{static_cast<DWORD>(f.maskSampler),static_cast<DWORD>(f.ambSampler)}){
        gpu.dev->SetTexture(s,s==static_cast<DWORD>(f.maskSampler)?static_cast<IDirect3DBaseTexture9*>(mask):amb);
        gpu.dev->SetSamplerState(s,D3DSAMP_MINFILTER,D3DTEXF_POINT);
        gpu.dev->SetSamplerState(s,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
        gpu.dev->SetSamplerState(s,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
        gpu.dev->SetSamplerState(s,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
        gpu.dev->SetSamplerState(s,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
    }
    const float size[4]={2,2,.5f,.5f},k[4]={1,1,1,0};
    gpu.dev->SetPixelShaderConstantF(b.sizeConst,size,1);
    gpu.dev->SetPixelShaderConstantF(f.sizeConst,size,1);
    gpu.dev->SetPixelShaderConstantF(static_cast<UINT>(f.floorConst),k,1);
    const auto native=RenderUv4(gpu,guard,1),room=RenderUv4(gpu,fill,1);
    const auto at=[](const std::vector<DWORD>& p,int sh){return int((p[32*64+20]>>sh)&255);};
    std::printf("Room fill: native (%d %d %d), with the room's ambient (%d %d %d); expected about (64 115 64) and (5 77 5)\n",at(native,16),at(native,8),at(native,0),at(room,16),at(room,8),at(room,0));
    Check(at(native,16)>=60&&at(native,0)>=60,"room fill: the game's cube fill is grey (reproduced: red and blue on a green floor)");
    Check(std::abs(at(room,16)-5)<=2&&std::abs(at(room,0)-5)<=2&&std::abs(at(room,8)-77)<=2,"room fill: fill = ambient x alpha, floor term = map.rgb + map.a x ambient");
    // the floors' tone curve on top (stair_floor_tone, the InteriorFloor PS): c x (2 / (1 + 2^(-4.2 l)) - 1) / l, l = (r + g + b) x 0.333
    {
        auto toned=original;ShaderPatches::BasisSmoothPatch tp;
        Check(ShaderPatches::PatchBasisSmooth(toned,tp,{.preserveShadows=true,.wallMask=true,.floorWall=true,.stairRoomFill=true,.floorTone=true})&&tp.floorTone,"floor tone: the colour write found");
        IDirect3DPixelShader9* tone=nullptr;
        Check(SUCCEEDED(gpu.dev->CreatePixelShader(toned.data(),&tone)),"floor-tone copy created by native D3D9");
        if(tone){
            gpu.dev->SetPixelShaderConstantF(tp.sizeConst,size,1);
            gpu.dev->SetPixelShaderConstantF(static_cast<UINT>(tp.floorConst),k,1);
            gpu.dev->SetTexture(static_cast<DWORD>(tp.maskSampler),mask);gpu.dev->SetTexture(static_cast<DWORD>(tp.ambSampler),amb);
            const auto out=RenderUv4(gpu,tone,1);
            const float c[3]={at(room,16)/255.0f,at(room,8)/255.0f,at(room,0)/255.0f},l=std::max(1e-4f,(c[0]+c[1]+c[2])*0.333f);
            const float f=(2.0f/(1.0f+std::exp2(-4.2f*l))-1.0f)/l; // (D3D exp is 2^x, as in the floor shader)
            std::printf("Floor tone: (%d %d %d) -> (%d %d %d), the floor's law gives x%.3f\n",at(room,16),at(room,8),at(room,0),at(out,16),at(out,8),at(out,0),f);
            Check(std::abs(at(out,8)-int(c[1]*f*255.0f+0.5f))<=3&&std::abs(at(out,16)-int(c[0]*f*255.0f+0.5f))<=2,"floor tone: the stair's colour follows the floor's tone curve");
            Check(f>1.4f,"floor tone: about x1.46 in the dark (the floor's law: the steps were that much darker than the floor)");
            Release(tone);
        }
    }
    for(DWORD s=0;s<16;++s)gpu.dev->SetTexture(s,nullptr);
    Release(basis);Release(floor);Release(mask);Release(amb);Release(cube);Release(guard);Release(fill);
}

// The light by height with the story above (stair_story_blend, F7: the top steps of a stair under the lit floor above
// were black). A fixture shaped like the stair shader (normal from TEXCOORD1, nrm, the four weighted maps, the cube fill, the
// floor read, the max that ends the lighting); this story's maps all black, the story above's lit. The quad's height goes
// from yLow (top row) to yUp + 1 (bottom row), the normal points up.
std::vector<DWORD> RenderBlend(Gpu& gpu,IDirect3DPixelShader9* ps,float yTop,float yBottom){
    struct Vertex{float x,y,z,rhw,u,v,s,t,nx,ny,nz,h;};
    const Vertex q[4]={{-0.5f,-0.5f,0,1,0,0,0,0,0,1,0,yTop},{63.5f,-0.5f,0,1,1,0,1,0,0,1,0,yTop},{-0.5f,63.5f,0,1,0,1,0,1,0,1,0,yBottom},{63.5f,63.5f,0,1,1,1,1,1,0,1,0,yBottom}};
    gpu.dev->SetVertexShader(nullptr);
    gpu.dev->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX2|D3DFVF_TEXCOORDSIZE4(0)|D3DFVF_TEXCOORDSIZE4(1));
    gpu.dev->SetPixelShader(ps);
    gpu.dev->BeginScene();
    Check(SUCCEEDED(gpu.dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,q,sizeof(Vertex))),"blend test quad draw");
    gpu.dev->EndScene();
    std::vector<DWORD> px(4096);
    D3DLOCKED_RECT r{};
    if(SUCCEEDED(gpu.dev->GetRenderTargetData(gpu.target,gpu.readback))&&SUCCEEDED(gpu.readback->LockRect(&r,nullptr,D3DLOCK_READONLY))){
        for(int y=0;y<64;y++)std::memcpy(px.data()+y*64,static_cast<char*>(r.pBits)+y*r.Pitch,256);
        gpu.readback->UnlockRect();
    }
    return px;
}
void BlendPixels(Gpu& gpu){
    if(!gpu.dev)return;
    const char* source=R"(ps_3_0
def c0, 0.8944, 0.4472, 0, -0.8944
def c1, 1, 0, 0, 1
def c2, 0, 0, 0, 0.25
dcl_texcoord v0
dcl_texcoord1 v1.xyz
dcl_2d s0
dcl_2d s1
dcl_2d s2
dcl_2d s3
dcl_2d s4
dcl_cube s5
nrm r9.xyz, v1
texld r0, v0, s0
texld r1, v0, s1
texld r2, v0, s2
texld r3, v0, s3
dp2add_sat r4.x, r9.xy, c0.xy, c0.z
dp2add_sat r4.y, r9.xy, c0.wy, c0.z
dp2add_sat r4.z, r9.yz, c0.yx, c0.z
dp2add_sat r4.w, r9.yz, c0.yw, c0.z
mul r6.xyz, r0, r4.x
mad r6.xyz, r1, r4.y, r6
mad r6.xyz, r2, r4.z, r6
mad r6.xyz, r3, r4.w, r6
texld r8, r9, s5
mul r8.xyz, r8, c2.w
max r6.xyz, r6, r8
texld_pp r7, v0.zwzw, s4
mul r7.xyz, r7, c1.x
max r6.xyz, r6, r7
mov r6.w, c1.w
mov oC0, r6
)";
    ID3DBlob *code=nullptr,*error=nullptr;
    using Assemble=HRESULT(WINAPI*)(LPCVOID,SIZE_T,LPCSTR,const D3D_SHADER_MACRO*,ID3DInclude*,UINT,ID3DBlob**,ID3DBlob**);
    const auto compiler=LoadLibraryW(L"d3dcompiler_47.dll");
    const auto assemble=compiler?reinterpret_cast<Assemble>(GetProcAddress(compiler,"D3DAssemble")):nullptr;
    const auto hr=assemble?assemble(source,std::strlen(source),nullptr,nullptr,nullptr,0,&code,&error):E_NOTIMPL;
    if(FAILED(hr)&&error)std::printf("%s\n",static_cast<const char*>(error->GetBufferPointer()));
    Check(SUCCEEDED(hr)&&code,"assemble blend fixture");
    std::vector<DWORD> original;
    if(code){original.resize(code->GetBufferSize()/4);std::memcpy(original.data(),code->GetBufferPointer(),code->GetBufferSize());}
    Release(code);Release(error);if(compiler)FreeLibrary(compiler);
    if(original.empty())return;
    auto blended=original;ShaderPatches::BasisSmoothPatch p;
    const bool patched=ShaderPatches::PatchBasisSmooth(blended,p,{.preserveShadows=true,.wallMask=true,.floorWall=true,.stairRoomFill=true,.floorTone=false,.storyBlend=true});
    std::printf("blend fixture: patched %d mask %d floor %d amb %d up %d story %d\n",patched?1:0,p.maskSampler,p.floorSampler,p.ambSampler,p.upSampler,p.storyConst);
    Check(patched&&p.upSampler>=0&&p.upAmbSampler>=0&&p.storyConst>=0,"story blend: the normal, the floor read and the light found");
    IDirect3DPixelShader9* ps=nullptr;
    Check(SUCCEEDED(gpu.dev->CreatePixelShader(blended.data(),&ps)),"story-blend copy created by native D3D9");
    if(!ps){
        ID3DBlob* text=nullptr;
        if(SUCCEEDED(D3DDisassemble(blended.data(),blended.size()*4,0,nullptr,&text))&&text)std::printf("%.*s\n",static_cast<int>(text->GetBufferSize()),static_cast<const char*>(text->GetBufferPointer()));
        Release(text);return;
    }
    IDirect3DTexture9 *black=nullptr,*lit=nullptr,*floor=nullptr,*mask=nullptr,*amb=nullptr;IDirect3DCubeTexture9* cube=nullptr;
    gpu.dev->CreateTexture(2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&black,nullptr);
    gpu.dev->CreateTexture(2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&lit,nullptr);
    gpu.dev->CreateTexture(8,8,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&floor,nullptr);
    gpu.dev->CreateTexture(2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&mask,nullptr);
    gpu.dev->CreateTexture(2,2,1,0,D3DFMT_A32B32G32R32F,D3DPOOL_MANAGED,&amb,nullptr);
    gpu.dev->CreateCubeTexture(1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&cube,nullptr);
    if(!black||!lit||!floor||!mask||!amb||!cube){Release(black);Release(lit);Release(floor);Release(mask);Release(amb);Release(cube);Release(ps);Check(false,"blend test resources");return;}
    const auto fillTex=[](IDirect3DTexture9* t,int size,DWORD colour){
        D3DLOCKED_RECT lock{};
        if(FAILED(t->LockRect(0,&lock,nullptr,0))||!lock.pBits)return;
        for(int y=0;y<size;++y)for(int x=0;x<size;++x)reinterpret_cast<DWORD*>(static_cast<char*>(lock.pBits)+y*lock.Pitch)[x]=colour;
        t->UnlockRect(0);
    };
    fillTex(black,2,0xff000000u);fillTex(lit,2,0xff663319u); // the story above: (0.4, 0.2, 0.1)
    fillTex(floor,8,0xff000000u);fillTex(mask,2,0u);
    {
        D3DLOCKED_RECT lock{};
        if(SUCCEEDED(amb->LockRect(0,&lock,nullptr,0))&&lock.pBits){for(int y=0;y<2;++y)std::memset(static_cast<char*>(lock.pBits)+y*lock.Pitch,0,2*16);amb->UnlockRect(0);}
        for(int face=0;face<6;++face)if(SUCCEEDED(cube->LockRect(static_cast<D3DCUBEMAP_FACES>(face),0,&lock,nullptr,0))&&lock.pBits){*static_cast<DWORD*>(lock.pBits)=0xff000000u;cube->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face),0);}
    }
    const auto bind=[&](DWORD s,IDirect3DBaseTexture9* t,bool point){
        gpu.dev->SetTexture(s,t);
        gpu.dev->SetSamplerState(s,D3DSAMP_MINFILTER,point?D3DTEXF_POINT:D3DTEXF_LINEAR);
        gpu.dev->SetSamplerState(s,D3DSAMP_MAGFILTER,point?D3DTEXF_POINT:D3DTEXF_LINEAR);
        gpu.dev->SetSamplerState(s,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
        gpu.dev->SetSamplerState(s,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
        gpu.dev->SetSamplerState(s,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
    };
    for(DWORD s=0;s<4;++s)bind(s,black,false);
    bind(4,floor,false);bind(5,cube,false);
    bind(static_cast<DWORD>(p.maskSampler),mask,true);bind(static_cast<DWORD>(p.ambSampler),amb,true);
    for(DWORD d=0;d<4;++d)bind(static_cast<DWORD>(p.upSampler)+d,lit,false);
    bind(static_cast<DWORD>(p.upAmbSampler),amb,true);
    const float size[4]={2,2,.5f,.5f},k[4]={1,1,1,0};
    gpu.dev->SetPixelShaderConstantF(p.sizeConst,size,1);
    gpu.dev->SetPixelShaderConstantF(static_cast<UINT>(p.floorConst),k,1);
    const float yLow=10.0f,yUp=13.0f,story[4]={yLow+0.5f,1.0f/(yUp-yLow),1.0f,0.0f};
    gpu.dev->SetPixelShaderConstantF(static_cast<UINT>(p.storyConst),story,1);
    const auto px=RenderBlend(gpu,ps,yLow,yUp+1.0f);
    const auto red=[&](int row){return int((px[row*64+32]>>16)&255);};
    // rows: y = yLow + (row + 0.5) / 64 x 4; the blend runs from yLow + 0.5 (row 7.5) to yUp + 0.5 (row 55.5)
    const int full=int(4*0.4472f*0.4f*255.0f+0.5f);
    std::printf("Story blend: red at rows 2/8/20/32/44/56/62 = %d/%d/%d/%d/%d/%d/%d (the story above's light: %d)\n",red(2),red(8),red(20),red(32),red(44),red(56),red(62),full);
    Check(red(2)<=1,"story blend: at its own floor's sample height, the story's own (black) light");
    Check(std::abs(red(62)-full)<=3,"story blend: at the floor above, the story above's light");
    bool rising=true;for(int row=1;row<64;++row)rising&=red(row)+1>=red(row-1);
    Check(rising&&red(32)>red(20)&&red(44)>red(32),"story blend: rises with the height");
    const float off[4]={yLow+0.5f,1.0f/(yUp-yLow),0.0f,0.0f};
    gpu.dev->SetPixelShaderConstantF(static_cast<UINT>(p.storyConst),off,1);
    const auto offPx=RenderBlend(gpu,ps,yLow,yUp+1.0f);
    int worst=0;for(DWORD c:offPx)worst=std::max(worst,int((c>>16)&255));
    Check(worst<=1,"story blend: off (no story above or no height), the story's own light");
    for(DWORD s=0;s<16;++s)gpu.dev->SetTexture(s,nullptr);
    Release(black);Release(lit);Release(floor);Release(mask);Release(amb);Release(cube);Release(ps);
}

int main(int argc,char** argv){
    if(argc<2)return 2;
    Gpu gpu;ShadowPixels(gpu);WallPixels(gpu);FloorPixels(gpu);FillPixels(gpu);BlendPixels(gpu);
    std::ifstream file(argv[1],std::ios::binary);
    std::vector<unsigned char> raw{std::istreambuf_iterator<char>(file),{}};
    std::vector<Blob> blobs;std::vector<Tech> techniques;
    Check(LoadPrecomp(raw,blobs,techniques),"load original game precomp");
    unsigned matched=0,created=0,recovered=0,captures=0,wallMatched=0,wallCreated=0,floorCopies=0,fillCopies=0,toneCopies=0,blendCopies=0,refused[5]={};
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
        if(!ShaderPatches::PatchBasisSmooth(code,patch,{.preserveShadows=true}))return false;
        ++matched;ID3DBlob* disassembly=nullptr;
        Check(SUCCEEDED(D3DDisassemble(code.data(),code.size()*4,0,nullptr,&disassembly)),"guarded shader disassembly");Release(disassembly);
        const bool valid=gpu.AcceptsPs(code);Check(valid,"guarded shader accepted by native D3D9");created+=valid;
        // the wall-aware copy (basis_wall_filter) of the same shader, where it fits (temps and samplers): native too
        auto walled=source;ShaderPatches::BasisSmoothPatch wp;
        if(ShaderPatches::PatchBasisSmooth(walled,wp,{.preserveShadows=true,.wallMask=true,.floorWall=true,.stairRoomFill=true,.floorTone=true,.storyBlend=true})){floorCopies+=wp.floorSampler>=0;fillCopies+=wp.ambSampler>=0;toneCopies+=wp.floorTone;blendCopies+=wp.storyConst>=0;if(wp.blendRefused>0&&wp.blendRefused<5)refused[wp.blendRefused]++;
            ++wallMatched;const bool ok=gpu.AcceptsPs(walled);Check(ok,"wall-aware shader accepted by native D3D9");wallCreated+=ok;
            static int shown=0;
            if(!ok&&shown++<1){ // why: the assembler's message for the disassembled copy
                ID3DBlob *text=nullptr,*again=nullptr,*err=nullptr;
                if(SUCCEEDED(D3DDisassemble(walled.data(),walled.size()*4,0,nullptr,&text))&&text){
                    using Assemble=HRESULT(WINAPI*)(LPCVOID,SIZE_T,LPCSTR,const D3D_SHADER_MACRO*,ID3DInclude*,UINT,ID3DBlob**,ID3DBlob**);
                    const auto lib=LoadLibraryW(L"d3dcompiler_47.dll");
                    const auto assemble=lib?reinterpret_cast<Assemble>(GetProcAddress(lib,"D3DAssemble")):nullptr;
                    if(assemble)assemble(text->GetBufferPointer(),text->GetBufferSize(),nullptr,nullptr,nullptr,0,&again,&err);
                    std::printf("rejected wall-aware copy (%zu dwords): %s\n",walled.size(),err?static_cast<const char*>(err->GetBufferPointer()):"(assembles: the device refused it)");
                    if(lib)FreeLibrary(lib);
                }
                Release(text);Release(again);Release(err);
            }
        }
        return true;
    };
    for(const auto& blob:blobs)if(!blob.vs){
        if(!validate(blob.code))continue;
        auto old=blob.code;ShaderPatches::BasisSmoothPatch patch;
        if(ShaderPatches::PatchBasisSmooth(old,patch)&&captured.count(old))++recovered;
    }
    for(const auto& code:captured)captures+=validate(code);
    Check(matched>0&&matched==created,"all matched copies create natively");
    Check(wallMatched>0&&wallMatched==wallCreated,"all wall-aware copies create natively");
    Check(floorCopies>0,"the game's basis shaders that read the floor map through the uv's zw get the floor rule");
    std::printf("Basis copies %u, created %u, original captured matches %u, captured native patterns %u; wall-aware copies %u, created %u (with the floor read %u, with the room fill %u, with the floor tone %u, blended with the story above %u; refused: samplers %u, no normal %u, no light max %u, no dcl %u)\n%d checks, %d failures\n",matched,created,recovered,captures,wallMatched,wallCreated,floorCopies,fillCopies,toneCopies,blendCopies,refused[1],refused[2],refused[3],refused[4],g_checks,g_failures);
    return g_failures?1:0;
}
