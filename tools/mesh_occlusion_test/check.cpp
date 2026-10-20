// Production GPU-buffer collection and triangle-ray checks. Credits: @loinyx
#define main RoomMapSuiteMain
#include "../room_map_smooth_check/check.cpp"
#undef main
#include "features/mesh_occlusion.h"
#include "features/mesh_budget_policy.h"
#include "build_flavor.h"
#include <wrl/client.h>
#include <unordered_map>
#include <array>
#include <format>
#include <random>
#include <thread>
#define LOG_INFO(value) ((void)0)
IDirect3DPixelShader9* g_curPs=nullptr;
bool g_inOwnCall=false;
#include "collector.inc"
using Microsoft::WRL::ComPtr;
using namespace MeshOcclusion;

std::vector<DWORD> ReadShader(const char* path){std::ifstream f(path,std::ios::binary);std::vector<char> bytes{std::istreambuf_iterator<char>(f),{}};std::vector<DWORD> code(bytes.size()/4);if(!bytes.empty())std::memcpy(code.data(),bytes.data(),code.size()*4);return code;}
void Quad(std::vector<Triangle>& t,float x0,float x1,float z0,float z1,float y){t.push_back({{x0,y,z0},{x1,y,z0},{x1,y,z1}});t.push_back({{x0,y,z0},{x1,y,z1},{x0,y,z1}});}
void Collect(IDirect3DDevice9* dev,bool gap,bool index32=false,bool badIndex=false,bool alpha=false,bool dynamic=false,bool narrow=false,bool compact=false,bool saturated=false){
    Reset();PumpSolidMeshes();
    ComPtr<IDirect3DVertexBuffer9> vb,instances;ComPtr<IDirect3DIndexBuffer9> ib;ComPtr<IDirect3DVertexDeclaration9> declaration;
    const float closed[][4]={{0,0,0,1},{1.2f,0,0,1},{1.2f,0,1.2f,1},{0,0,1.2f,1}};
    const float open[][4]={{0,0,0,1},{.4f,0,0,1},{.4f,0,1.2f,1},{0,0,1.2f,1},{.8f,0,0,1},{1.2f,0,0,1},{1.2f,0,1.2f,1},{.8f,0,1.2f,1}};
    const UINT count=gap?8:4;const UINT indexCount=saturated?136*3:gap?12:6;
    const UINT instanceCount=compact?8:1,instanceStride=compact?28:32;
    const UINT min=3;const INT base=2;const UINT first=5;
    Check(SUCCEEDED(dev->CreateVertexBuffer((first+count)*16,dynamic?D3DUSAGE_DYNAMIC:0,0,dynamic?D3DPOOL_DEFAULT:D3DPOOL_MANAGED,vb.GetAddressOf(),nullptr)),"vertex buffer");
    Check(SUCCEEDED(dev->CreateVertexBuffer(instanceStride*instanceCount,0,0,D3DPOOL_MANAGED,instances.GetAddressOf(),nullptr)),"instance buffer");
    Check(SUCCEEDED(dev->CreateIndexBuffer(indexCount*(index32?4:2),0,index32?D3DFMT_INDEX32:D3DFMT_INDEX16,D3DPOOL_MANAGED,ib.GetAddressOf(),nullptr)),"index buffer");
    if(!vb||!instances||!ib)return;
    void* data=nullptr;Check(SUCCEEDED(vb->Lock(first*16,count*16,&data,0)),"fill vertices");if(!data)return;
    std::memcpy(data,gap?open:closed,count*16);if(narrow)for(UINT i=0;i<count;++i)static_cast<float*>(data)[i*4+2]*=.1f;vb->Unlock();
    float placement[8]={10,2,20,1,1,0,0,1};if(compact){placement[3]=0;placement[4]=0;placement[5]=-1;placement[6]=0;}
    instances->Lock(0,0,&data,0);
    for(UINT i=0;i<instanceCount;++i){float at[8];std::memcpy(at,placement,32);at[0]+=2*i;at[1]+=.75f*i;std::memcpy(static_cast<BYTE*>(data)+i*instanceStride,at,instanceStride);}
    instances->Unlock();
    const uint32_t ids[]={0,1,2,0,2,3,4,5,6,4,6,7};ib->Lock(0,0,&data,0);
    for(UINT i=0;i<indexCount;++i){const auto value=(badIndex&&i==0?count:ids[i%12])+min;if(index32)static_cast<uint32_t*>(data)[i]=value;else static_cast<uint16_t*>(data)[i]=static_cast<uint16_t>(value);}ib->Unlock();
    const D3DVERTEXELEMENT9 elements[]={{0,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},{1,0,static_cast<BYTE>(compact?D3DDECLTYPE_FLOAT3:D3DDECLTYPE_FLOAT4),D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,1},{1,static_cast<WORD>(compact?12:16),D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,2},D3DDECL_END()};
    dev->CreateVertexDeclaration(elements,declaration.GetAddressOf());dev->SetVertexDeclaration(declaration.Get());
    dev->SetStreamSource(0,vb.Get(),0,16);dev->SetStreamSource(1,instances.Get(),0,instanceStride);dev->SetIndices(ib.Get());
    Check(SUCCEEDED(dev->SetStreamSourceFreq(0,D3DSTREAMSOURCE_INDEXEDDATA|instanceCount))&&SUCCEEDED(dev->SetStreamSourceFreq(1,D3DSTREAMSOURCE_INSTANCEDATA|1)),"instance frequencies");
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);dev->SetRenderState(D3DRS_ALPHATESTENABLE,alpha);dev->SetRenderState(D3DRS_ZWRITEENABLE,TRUE);
    // A malformed group ahead of the stair must not consume every frame's sole read.
    if(!gap&&!badIndex&&!alpha&&!narrow&&!dynamic&&!compact&&SolidPixelShader()){
        vb->Lock(first*16,count*16,&data,0);for(UINT i=0;i<count;++i)static_cast<float*>(data)[i*4+3]=0;vb->Unlock();
        NoteSolidMesh(dev,D3DPT_TRIANGLELIST,base,min,count,0,indexCount/3,true);
        Check(g_solidGroups.empty()&&g_solidReadBudget==0,"malformed mesh spends one read and is rejected");
        g_solidNextScan=0;g_solidScanning=false;PumpSolidMeshes();NoteSolidMesh(dev,D3DPT_TRIANGLELIST,base,min,count,0,indexCount/3,true);
        Check(g_solidReadBudget==1,"rejected mesh yields later frames to following geometry");
        vb->Lock(first*16,count*16,&data,0);std::memcpy(data,closed,count*16);vb->Unlock();
        // Another mesh is represented by a distinct placement buffer.
        ComPtr<IDirect3DVertexBuffer9> next;
        dev->CreateVertexBuffer(32,0,0,D3DPOOL_MANAGED,next.GetAddressOf(),nullptr);
        next->Lock(0,32,&data,0);std::memcpy(data,placement,32);next->Unlock();
        dev->SetStreamSource(1,next.Get(),0,32);
    }
    if(saturated){
        for(uint64_t i=1;i<=23;++i){
            SolidGroup group;const float x=1000+static_cast<float>(i);
            group.triangles.assign(i==23?354:356,{{x,0,0},{x,.01f,0},{x,0,.01f}});
            group.priority=MeshBudget::Priority(group.triangles,0);group.read=group.seen=GetTickCount();
            g_solidTriangles+=group.triangles.size();g_solidGroups.emplace(i,std::move(group));
        }
        Check(g_solidTriangles==8186,"real-session occupancy reproduced before stair draw");
    }
    NoteSolidMesh(dev,D3DPT_TRIANGLELIST,base,min,count,0,indexCount/3,true);
    if(saturated)Check(g_solidTriangles==6762+indexCount/3*instanceCount&&g_solidGroups.size()==20,"1088-triangle stair group replaces narrow detail at saturation without raising limit");
    if(!badIndex&&!alpha&&SolidPixelShader()){
        ComPtr<IDirect3DVertexBuffer9> clone;
        dev->CreateVertexBuffer(instanceStride*instanceCount,0,0,D3DPOOL_MANAGED,clone.GetAddressOf(),nullptr);
        clone->Lock(0,0,&data,0);
        for(UINT i=0;i<instanceCount;++i){float at[8];std::memcpy(at,placement,32);at[0]+=2*i;at[1]+=.75f*i;std::memcpy(static_cast<BYTE*>(data)+i*instanceStride,at,instanceStride);}
        clone->Unlock();dev->SetStreamSource(1,clone.Get(),0,instanceStride);g_solidReadBudget=1;
        NoteSolidMesh(dev,D3DPT_TRIANGLELIST,base,min,count,0,indexCount/3,true);
        Check(g_solidGroups.size()==(saturated?20:1)&&g_solidTriangles==(saturated?6762:0)+indexCount/3*instanceCount,"identical geometry in another transient buffer does not consume scene budget");
    }
    g_solidDirty=GetTickCount()-600;g_solidPublished=GetTickCount()-3000;PumpSolidMeshes();
    const bool rejected=badIndex||alpha||!SolidPixelShader();
    auto snap=snapshot.load();Check(snap&&snap->Count()==(saturated?6762:0)+(rejected?0:indexCount/3*instanceCount),"production triangle count and rejection");
    const float z=narrow?20.06f:20.6f;
    const float x=compact?10+(z-20):10.2f,zSolid=compact?19.8f:z;
    const float p[]={x,3,zSolid},q[]={x,1,zSolid};Check(Blocked(p,q)==!rejected,"solid tread blocks ray");
    const float a[]={compact?x:10.6f,3,compact?19.4f:z},b[]={compact?x:10.6f,1,compact?19.4f:z};Check(Blocked(a,b)==(!gap&&!rejected),"open tread gap remains clear");
    Reset();PumpSolidMeshes();Check(!Blocked(p,q)&&g_solidGroups.empty(),"floor/world reset drops old geometry");
    dev->SetStreamSourceFreq(0,1);dev->SetStreamSourceFreq(1,1);
}
int main(int argc,char** argv){
    if(argc<3)return 2;
    std::vector<MeshBudget::Entry> entries={{1,4093,.001},{2,4093,.001}};
    std::vector<uint64_t> victims;
    Check(MeshBudget::Plan(entries,8186,1088,.1,8192,128,victims)&&victims==std::vector<uint64_t>{1},"captured 1088-triangle draw can replace lower coverage at 8186 occupancy");
    entries={{1,4093,.1},{2,4093,.1}};
    Check(!MeshBudget::Plan(entries,8186,1088,.001,8192,128,victims)&&victims.empty(),"weak candidate cannot evict important geometry");
    entries={{1,4093,.001},{2,4093,1.}};
    Check(!MeshBudget::Plan(entries,8186,8192,.1,8192,128,victims)&&victims.empty(),"failed multi-victim plan has no partial removals");
    Check(!MeshBudget::Plan(entries,8186,8193,2,8192,128,victims),"oversized candidate stays bounded");
    entries={{1,2,.001},{2,2,.001}};
    Check(MeshBudget::Plan(entries,4,2,.1,8192,2,victims)&&victims.size()==1,"group limit also replaces instead of starving discovery");
    float p[]={2,4,6,2},place[]={10,20,30,1},turn[]={0,.5f,1,1};Vec v;
    Check(Decode(p,place,turn,v)&&v.x==7&&v.y==22.5f&&v.z==31,"native packing rotation and slope");p[3]=0;Check(!Decode(p,place,turn,v),"zero denominator rejected");
    std::vector<Triangle> t;Quad(t,0,.4f,0,1,1);Quad(t,.8f,1.2f,0,1,1);Scene open(t);
    Check(open.Blocked({.2f,2,.5f},{.2f,0,.5f}),"two-sided solid tread");Check(!open.Blocked({.6f,2,.5f},{.6f,0,.5f}),"no filled bounding box across gap");
    Check(!open.Blocked({.2f,1,.5f},{.2f,2,.5f}),"receiver surface does not shadow itself");
    std::mt19937 rng(731);std::uniform_real_distribution<float> dist(-2,2);
    for(int i=0;i<300;++i){Triangle tri{{dist(rng),dist(rng),dist(rng)},{dist(rng),dist(rng),dist(rng)},{dist(rng),dist(rng),dist(rng)}};t.push_back(tri);}
    Scene many(t);
    for(int i=0;i<20000;++i){Vec a{dist(rng),dist(rng),dist(rng)},b{dist(rng),dist(rng),dist(rng)};const auto d=Sub(b,a);const float len=std::sqrt(Dot(d,d));bool expected=false;
        if(len>.04f)for(const auto& tri:t)if(Hits(tri,a,d,.02f/len,1-.02f/len)){expected=true;break;}
        Check(many.Blocked(a,b)==expected,"BVH equals triangle sweep");}
    std::atomic<bool> done=false;std::thread reader([&]{const float a[]={.2f,2,.5f},b[]={.2f,0,.5f};while(!done){(void)Blocked(a,b);}});
    for(int i=0;i<100;++i){Publish(t);Reset();}done=true;reader.join();
    Gpu gpu;Check(gpu.dev!=nullptr,"native D3D9 fixture device");if(!gpu.dev)return 1;
    ComPtr<IDirect3DPixelShader9> opaque,cutout;auto code=ReadShader(argv[1]);auto alpha=ReadShader(argv[2]);
    Check(SUCCEEDED(gpu.dev->CreatePixelShader(code.data(),opaque.GetAddressOf())),"captured opaque shader");
    Check(SUCCEEDED(gpu.dev->CreatePixelShader(alpha.data(),cutout.GetAddressOf())),"captured cutout shader");
    g_curPs=opaque.Get();Collect(gpu.dev,false);Collect(gpu.dev,true);Collect(gpu.dev,true,true);Collect(gpu.dev,false,false,true);Collect(gpu.dev,false,false,false,true);
    Collect(gpu.dev,false,false,false,false,false,true);Collect(gpu.dev,true,false,false,false,true,true);
    Collect(gpu.dev,true,false,false,false,true,true,true);
    Collect(gpu.dev,true,false,false,false,true,true,true,true);
    g_curPs=cutout.Get();Collect(gpu.dev,false);g_curPs=nullptr;
    std::printf("Mesh occlusion: %d checks, %d failures\n",g_checks,g_failures);return g_failures?1:0;
}
