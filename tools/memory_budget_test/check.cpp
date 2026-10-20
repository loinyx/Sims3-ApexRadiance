#include "../../features/memory_guard.cpp"
#include "../../features/fast_refpack.cpp"
#include "../../features/fast_memory.cpp"
#include "../../features/scratch_budget.h"
#include <cassert>
#include <iostream>
#include <thread>
namespace ApexLog { void Write(Level,const std::string&,const std::source_location&) noexcept {} }
uint32_t g_exeTimestamp=0x52DEC247;
namespace GameAddr {
bool IsFixed(){return true;} uintptr_t Get(Id){return 0;}
bool Have(std::initializer_list<Id>,std::string*){return false;}
std::string NotAvailable(const std::string& s){return s;}
bool GroupAvailable(const char*,std::string*){return false;}
}
namespace MemPatch {
bool ReadBytes(uintptr_t,void*,size_t){return false;}
bool ValidateBytes(LPCVOID,const BYTE*,size_t){return false;}
bool WriteDWORD(uintptr_t,DWORD,std::vector<PatchLocation>*,const DWORD*){return false;}
bool RestoreAll(std::vector<PatchLocation>&){return true;}
int32_t CalculateRelativeOffset(uintptr_t,uintptr_t,size_t){return 0;}
bool WriteCodeSuspended(uintptr_t,const BYTE*,size_t){return false;}
}
namespace SlotChain {
std::atomic<void*> g_next[static_cast<int>(Site::Count)][static_cast<int>(Layer::Count)]{};
bool Install(Site,Layer,void*,std::string*){return false;}
void Remove(Site,Layer){} uintptr_t GameFunction(Site){return 0;}
}
namespace ImGui {
ImGuiContext* GetCurrentContext(){return nullptr;}
void TextUnformatted(const char*,const char*){}
void TextColored(const ImVec4&,const char*,...){}
void TextDisabled(const char*,...){} void TextWrapped(const char*,...){}
bool Button(const char*,const ImVec2&){return false;}
}
namespace ApexUi {
bool DiagnosticIntRow(const char*,int*,int,int,const char*,int){return false;}
bool BeginAdvanced(const char*,const char*){return false;} void EndAdvanced(){}
}

static uint32_t __fastcall FakeTrim(void* p,void*,uint32_t subtract,void*,bool force) {
    assert(force); auto* b=static_cast<unsigned char*>(p);
    auto& budget=*reinterpret_cast<uint32_t*>(b+0x8c);
    auto& other=*reinterpret_cast<uint32_t*>(b+0x94);
    auto& idle=*reinterpret_cast<uint32_t*>(b+0x98);
    const uint32_t target=budget-subtract;
    const uint32_t before=idle; idle=target>other ? target-other : 0;
    return before-idle;
}
static unsigned expectedSaveDepth=1;
static bool __fastcall FakeWorldSave(void*,void*,void*,void*) {
    assert(MemoryTelemetry::saveDepth==expectedSaveDepth);
    assert(!MemoryGuard::Holding());
    assert(!MemoryGuard::TakeReserve());
    return false;
}
static int dispatchResult=0;
static int __fastcall FakeDispatch(void* owner,void*,void* a,void* b) {
    assert(owner==reinterpret_cast<void*>(11) && a==reinterpret_cast<void*>(22) && b==reinterpret_cast<void*>(33));
    assert(MemoryTelemetry::saveDepth==1 && !MemoryGuard::Holding());
    assert(!MemoryGuard::TakeReserve());
    expectedSaveDepth=2;
    assert(!MemoryGuard::WorldSaveHook(nullptr,nullptr,nullptr,nullptr));
    expectedSaveDepth=1;
    SetLastError(1234);
    return dispatchResult;
}
static bool __fastcall FakeStage(void* wm,void*,void* a,void* b) {
    assert(wm==reinterpret_cast<void*>(11) && a==reinterpret_cast<void*>(22) && b==reinterpret_cast<void*>(33));
    SetLastError(5678);
    return dispatchResult==0;
}
int main() {
    static_assert(kMemoryOptimizationsEnabled);
    assert(kPublicBuild);
    MemoryTelemetry::Event observed[64];
    SetLastError(4321);
    MemoryTelemetry::Attempt(123,true,0x1234,0,MEM_COMMIT);
    assert(MemoryTelemetry::Drain(observed)==0);
    MemoryTelemetry::Attempt(123,false,0x1234,8,MEM_COMMIT);
    assert(GetLastError()==4321 && MemoryTelemetry::allocationFailureTick.load()!=0);
    assert(MemoryTelemetry::Drain(observed)==1 && observed[0].size==123 && observed[0].caller==0x1234 && observed[0].error==8);
    for(unsigned i=0;i<70;++i)MemoryTelemetry::Attempt(i,false,0x1234,8,MEM_COMMIT);
    assert(MemoryTelemetry::Drain(observed)==64 && MemoryTelemetry::dropped==6);
    MemoryGuard::ReportAllocationFailures();assert(MemoryTelemetry::dropped==0);
    using namespace MemoryPolicy;
    // Real Windows pages exercise the production queue without installing a game hook.
    FastMemory::FreeFn freeFunction=&VirtualFree;
    FastMemory::g_freeSlot=reinterpret_cast<uintptr_t>(&freeFunction);
    FastMemory::g_on=true;FastMemory::g_workerAlive=true;
    auto* queuedBlock=VirtualAlloc(nullptr,MiB,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(queuedBlock && FastMemory::Stub_Free(queuedBlock,0,MEM_RELEASE));
    MEMORY_BASIC_INFORMATION blockInfo{};
    assert(VirtualQuery(queuedBlock,&blockInfo,sizeof blockInfo) && blockInfo.State==MEM_COMMIT);
    assert(FastMemory::PrepareForSave()==1 && FastMemory::g_queued==0);
    assert(VirtualQuery(queuedBlock,&blockInfo,sizeof blockInfo) && blockInfo.State==MEM_FREE);
    {
        MemoryTelemetry::SaveScope saving;
        auto* directBlock=VirtualAlloc(nullptr,MiB,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        assert(directBlock && FastMemory::Stub_Free(directBlock,0,MEM_RELEASE));
        assert(FastMemory::g_queued==0);
        assert(VirtualQuery(directBlock,&blockInfo,sizeof blockInfo) && blockInfo.State==MEM_FREE);
    }
    FastMemory::g_on=false;FastMemory::g_workerAlive=false;FastMemory::g_freeSlot=0;
    Space s; s.AddFree(0x7f000000ull,0x82000000ull);
    assert(s.free==48*MiB && s.lowFree==16*MiB && s.lowLargest==16*MiB);
    s={};s.complete=s.processValid=true;s.free=1024*MiB;s.largest=768*MiB;
    s.lowFree=128*MiB;s.lowLargest=8*MiB;s.privateBytes=1800*MiB;
    Pressure p; assert(!p.Observe(s,false)); // fragmentation alone
    s.privateBytes=2200*MiB;
    assert(!p.Observe(s,false) && !p.Observe(s,false) && p.Observe(s,false));
    s.lowFree=220*MiB;assert(p.Observe(s,false)); // hysteresis
    s.lowFree=300*MiB;assert(!p.Observe(s,false));
    s.complete=false; assert(!p.Observe(s,true));
    assert(TrimArgument(256*MiB,64*MiB,128*MiB)==80*MiB);
    assert(TrimArgument(256*MiB,64*MiB,64*MiB)==0);
    assert(TrimArgument(128*MiB,64*MiB,128*MiB)==0);
    assert(TrimArgument(0xffffffffu,0xffffffffu,0xffffffffu)==0);
    assert(!Expired(10,20,1) && Expired(120001,1,120000));
    alignas(4) unsigned char cache[0xA0]{},rs[0x1E8]{};
    *reinterpret_cast<uint32_t*>(cache+0x8c)=256*MiB;
    *reinterpret_cast<uint32_t*>(cache+0x94)=64*MiB;
    *reinterpret_cast<uint32_t*>(cache+0x98)=128*MiB;
    *reinterpret_cast<void**>(rs+0x1e0)=cache;
    MemoryGuard::g_partialTrim=reinterpret_cast<uintptr_t>(&FakeTrim);
    assert(MemoryGuard::TrimIdleStepRaw(rs)==16*MiB);
    assert(*reinterpret_cast<uint32_t*>(cache+0x98)==112*MiB);
    { ScratchBudget::Lease a(48*MiB,true); ScratchBudget::Lease b(32*MiB,true);
      assert(a.bytes==48*MiB && b.bytes==0 && ScratchBudget::used==48*MiB); }
    assert(ScratchBudget::used==0);
    MemoryTelemetry::Attempt(1*MiB,true,0,0,MEM_RESERVE);
    MemoryTelemetry::Attempt(1*MiB,false,0,8,MEM_RESERVE);
    assert(MemoryTelemetry::count==1 && MemoryTelemetry::calls[2]==0 && MemoryTelemetry::allocationFailureTick!=0);
    assert(MemoryTelemetry::Drain(observed)==1);
    std::vector<uint8_t> input(384*1024),decoded(input.size());
    for(size_t i=0;i<input.size();++i) input[i]=static_cast<uint8_t>((i/7)^i);
    std::vector<uint8_t> out(RefPackCodec::SizeBound(static_cast<uint32_t>(input.size()))),again(out.size());
    auto* slot=FastRefPack::Acquire();assert(slot);
    assert(reinterpret_cast<uintptr_t>(slot->mem)>=0x80000000u);
    RefPackCodec::Effort effort;
    auto encoded=FastRefPack::CompressAny(*slot,input.data(),static_cast<uint32_t>(input.size()),out.data(),static_cast<uint32_t>(out.size()),2,effort);
    assert(encoded!=RefPackCodec::kFailed);
    const auto useful=std::min(DxtCodec::Parallel::DefaultWorkers(),2u);
    assert(FastRefPack::g_workersMade.load()==useful);
    if(useful) {
        auto* pool=FastRefPack::g_existingPool.load();assert(pool);
        unsigned tokenArrays=0;for(auto* token:pool->tokens) if(token) ++tokenArrays;
        assert(tokenArrays==3);
    }
    assert(RefPackCodec::Decompress(decoded.data(),static_cast<uint32_t>(decoded.size()),out.data(),encoded)==input.size());
    assert(decoded==input);
    const uint64_t now=GetTickCount64();slot->lastUsed=now-120001;
    auto* original=slot->mem;
    FastRefPack::RetireIdleStorage(now);assert(slot->mem==original); // busy slot is protected
    slot->busy.store(false);
    if(auto* pool=FastRefPack::g_existingPool.load()) pool->lastUsed.store(now-120001);
    MemoryTelemetry::pressure=true;
    FastRefPack::RetireIdleStorage(now+5000);assert(!slot->mem && !slot->tokens);
    if(auto* pool=FastRefPack::g_existingPool.load())
        for(auto& worker:pool->workers) assert(!worker.mem && !worker.ctx.head);
    slot=FastRefPack::Acquire();assert(slot && slot->mem && slot->tokens);
    auto encodedAgain=FastRefPack::CompressAny(*slot,input.data(),static_cast<uint32_t>(input.size()),again.data(),static_cast<uint32_t>(again.size()),2,effort);
    assert(encodedAgain==encoded && std::memcmp(out.data(),again.data(),encoded)==0);
    if(auto* pool=FastRefPack::g_existingPool.load())
        for(unsigned i=0;i<useful;++i) assert(pool->workers[i].mem && pool->workers[i].ctx.head);
    std::vector<uint8_t> twoPieceData(out.size());
    const uint32_t twoPieceSize=256*1024;
    const auto twoPieceEncoded=FastRefPack::CompressAny(*slot,input.data(),twoPieceSize,twoPieceData.data(),static_cast<uint32_t>(twoPieceData.size()),2,effort);
    assert(twoPieceEncoded!=RefPackCodec::kFailed);
    slot->lastUsed=GetTickCount64();slot->busy=false;
    std::atomic<bool> done{false};
    std::thread retirement([&] {
        auto tick=GetTickCount64()+240000;
        while(!done.load()) { FastRefPack::RetireIdleStorage(tick);tick+=5000;std::this_thread::yield(); }
    });
    std::vector<std::thread> threads;
    for (unsigned i=0;i<4;++i) threads.emplace_back([&] {
        std::vector<uint8_t> encodedData(out.size()),roundTrip(input.size());
        for(unsigned j=0;j<15;++j) {
            FastRefPack::Slot temporary;
            auto* context=FastRefPack::Acquire();
            if(!context) { assert(FastRefPack::InitSlot(temporary));context=&temporary; }
            const auto size=j%2 ? twoPieceSize : static_cast<uint32_t>(input.size());
            const auto expected=j%2 ? twoPieceEncoded : encoded;
            const auto* expectedData=j%2 ? twoPieceData.data() : out.data();
            auto n=FastRefPack::CompressAny(*context,input.data(),size,encodedData.data(),static_cast<uint32_t>(encodedData.size()),2,effort);
            assert(n==expected && std::memcmp(expectedData,encodedData.data(),expected)==0);
            assert(RefPackCodec::Decompress(roundTrip.data(),static_cast<uint32_t>(roundTrip.size()),encodedData.data(),n)==size);
            assert(std::memcmp(roundTrip.data(),input.data(),size)==0);
            if(context==&temporary) {
                VirtualFree(temporary.mem,0,MEM_RELEASE);VirtualFree(temporary.tokens,0,MEM_RELEASE);
            } else { context->lastUsed=GetTickCount64();context->busy.store(false,std::memory_order_release); }
        }
    });
    for(auto& thread:threads) thread.join();
    done=true;retirement.join();
    MemoryTelemetry::pressure=false;
    // A copied counting result remains usable even when every compressor slot is busy.
    FastRefPack::g_on=true;
    FastRefPack::g_selfDisabled=false;
    FastRefPack::g_seq=FastRefPack::kStartupChecks;
    const auto src=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(input.data()));
    const auto dst=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(again.data()));
    auto counted=FastRefPack::Hook_StreamWrite(nullptr,nullptr,src,static_cast<uint32_t>(input.size()),0,0,2);
    assert(counted!=RefPackCodec::kFailed && FastRefPack::t_held.valid);
    for(auto& context:FastRefPack::g_pool) assert(!context.busy.exchange(true));
    const auto temporaryBefore=FastRefPack::c_temp.load();
    auto written=FastRefPack::Hook_StreamWrite(nullptr,nullptr,src,static_cast<uint32_t>(input.size()),dst,static_cast<uint32_t>(counted),1);
    assert(written==counted && FastRefPack::c_temp==temporaryBefore);
    assert(RefPackCodec::Decompress(decoded.data(),static_cast<uint32_t>(decoded.size()),again.data(),static_cast<uint32_t>(written))==input.size());
    assert(decoded==input);
    for(auto& context:FastRefPack::g_pool) context.busy=false;
    // Small compression uses a context without allocating segmented token storage.
    FastRefPack::Slot compactSlot;
    assert(FastRefPack::InitSlot(compactSlot,false) && compactSlot.mem && !compactSlot.tokens);
    VirtualFree(compactSlot.mem,0,MEM_RELEASE);
    // Reused writes still check destination capacity and preserve the output guard.
    counted=FastRefPack::Hook_StreamWrite(nullptr,nullptr,src,static_cast<uint32_t>(input.size()),0,0,2);
    std::fill(again.begin(),again.end(),uint8_t{0xa5});
    written=FastRefPack::Hook_StreamWrite(nullptr,nullptr,src,static_cast<uint32_t>(input.size()),dst,static_cast<uint32_t>(counted-1),1);
    assert(written==RefPackCodec::kFailed);
    for(auto byte:again) assert(byte==0xa5);
    FastRefPack::t_held.Free();
    // Startup decoding remains active for copied output and small copies need only 64 KiB.
    FastRefPack::g_seq=0;
    const auto checksBefore=FastRefPack::c_checked.load();
    counted=FastRefPack::Hook_StreamWrite(nullptr,nullptr,src,128,0,0,2);
    assert(FastRefPack::t_held.cap==64*1024);
    written=FastRefPack::Hook_StreamWrite(nullptr,nullptr,src,128,dst,static_cast<uint32_t>(counted),1);
    assert(written==counted && FastRefPack::c_checked==checksBefore+1);
    assert(RefPackCodec::Decompress(decoded.data(),static_cast<uint32_t>(decoded.size()),again.data(),static_cast<uint32_t>(written))==128);
    assert(std::memcmp(decoded.data(),input.data(),128)==0);
    FastRefPack::t_held.Free();
    const auto retainedBefore=FastRefPack::g_heldBytes.load();
    { FastRefPack::Held excessive;excessive.Alloc(65*MiB);assert(!excessive.buf); }
    assert(FastRefPack::g_heldBytes==retainedBefore);
    MemoryTelemetry::pressure=true;
    std::atomic<unsigned> attempted{0},admitted{0};
    threads.clear();
    for(unsigned i=0;i<8;++i) threads.emplace_back([&] {
        FastRefPack::Held held;held.Alloc(8*MiB);
        if(held.buf) ++admitted;
        ++attempted;
        while(attempted.load()<8) std::this_thread::yield();
        assert(FastRefPack::g_heldBytes<=32*MiB);
    });
    for(auto& thread:threads) thread.join();
    assert(admitted==4 && FastRefPack::g_heldBytes==retainedBefore);
    MemoryTelemetry::pressure=false;
    FastRefPack::t_held.Alloc(1*MiB);assert(FastRefPack::t_held.buf);
    MemoryTelemetry::pressure=true;FastRefPack::DropHeld();assert(!FastRefPack::t_held.buf);
    MemoryTelemetry::pressure=false;
    {
        MemoryTelemetry::SaveScope saving;
        FastRefPack::Held oversized;oversized.Alloc(9*MiB);assert(!oversized.buf);
        assert(!MemoryGuard::TakeReserve());
        FastRefPack::t_held.valid=true;
        FastRefPack::t_held.Alloc(1*MiB);assert(FastRefPack::t_held.buf);
        FastRefPack::PrepareForSave();assert(FastRefPack::t_held.buf); // pending paired write is protected
        FastRefPack::t_held.valid=false;
        assert(FastRefPack::PrepareForSave()>=1*MiB && !FastRefPack::t_held.buf);
    }
    assert(MemoryTelemetry::saveDepth==0);
    assert(MemoryGuard::TakeReserve());
    MemoryGuard::g_rs=nullptr;
    MemoryGuard::kWorldSave=reinterpret_cast<uintptr_t>(&FakeWorldSave);
    const auto failedBefore=MemoryGuard::g_saveFails.load();
    assert(!MemoryGuard::WorldSaveHook(nullptr,nullptr,nullptr,nullptr));
    assert(MemoryTelemetry::saveDepth==0 && MemoryGuard::g_saveFails==failedBefore+1 && !MemoryGuard::Holding());
    assert(MemoryGuard::TakeReserve() && MemoryGuard::LetReserveGo());
    MemoryGuard::kDispatch=reinterpret_cast<uintptr_t>(&FakeDispatch);
    for(int result:{0,9,10,11,12,13}) {
        dispatchResult=result;
        assert(MemoryGuard::TakeReserve());
        assert(MemoryGuard::SaveDispatchHook(reinterpret_cast<void*>(11),nullptr,reinterpret_cast<void*>(22),reinterpret_cast<void*>(33))==result);
        assert(GetLastError()==1234 && MemoryTelemetry::saveDepth==0 && !MemoryGuard::Holding());
    }
    MemoryGuard::kStageTargets[0]=reinterpret_cast<uintptr_t>(&FakeStage);
    for(int result:{0,1}) {
        dispatchResult=result;
        MemoryGuard::SaveStages stages;
        MemoryGuard::t_stages=&stages;
        const bool stage=MemoryGuard::SaveStageHook<0>(reinterpret_cast<void*>(11),nullptr,reinterpret_cast<void*>(22),reinterpret_cast<void*>(33));
        MemoryGuard::t_stages=nullptr;
        assert(stage==(result==0) && stages.seen[0] && stages.ok[0]==stage && !stages.seen[1] && !stages.seen[2]);
        assert(GetLastError()==5678);
        assert(MemoryGuard::SaveStageHook<0>(reinterpret_cast<void*>(11),nullptr,reinterpret_cast<void*>(22),reinterpret_cast<void*>(33))==stage);
    }
    std::cout<<"Public memory test: pressure, trim ABI, retirement races, identical compression, context-free copy, capacity guard and concurrent active-copy budget passed\n";
}
