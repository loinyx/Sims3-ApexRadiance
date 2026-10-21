#include <windows.h>
#include <cstdint>
#include <cstdio>
constexpr int kMaxDepth=6,kMaxProbe=32;
constexpr uint32_t kMask=4095,kRootsBegin=0x9C,kRootsEnd=0xA0,kChildBegin=0xA0,kChildEnd=0xA4,kIdLo=0x48,kIdHi=0x4C;
constexpr uint32_t kMaxAgeMs=2000;
uint32_t nowTick=100;
uint32_t MockTick(){return nowTick;}
#define GetTickCount MockTick
#include "object-entry.inc"
Entry table[4096]{};Entry* g_table=table;
SRWLOCK g_lock=SRWLOCK_INIT;uint32_t g_stamp=1,g_stampGen=1,locks=0;
uint32_t Hash(uint32_t,uint32_t lo,uint32_t){return lo;}
uint32_t Rd(uint32_t p){return *reinterpret_cast<const uint32_t*>(p);}
void Acquire(SRWLOCK* p){++locks;AcquireSRWLockShared(p);}
#define AcquireSRWLockShared Acquire
enum Why {Ok,BadRoot,BadClass,BadChild,BadId,EarlierMatch,Fault};
#include "object-cache.inc"
#undef AcquireSRWLockShared
int main(){int checks=0,fail=0;auto check=[&](bool ok){++checks;if(!ok)++fail;};
 alignas(16) unsigned char manager[256]{},parent[256]{},object[256]{};
 const auto address=[](auto* p){return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));};
 uint32_t roots[]={address(parent)},children[]={address(object)};
 auto put=[&](unsigned char* p,unsigned off,uint32_t v){*reinterpret_cast<uint32_t*>(p+off)=v;};
 put(manager,kRootsBegin,address(roots));put(manager,kRootsEnd,address(roots)+4);
 put(parent,0,111);put(parent,kChildBegin,address(children));put(parent,kChildEnd,address(children)+4);
 put(object,0,222);put(object,kIdLo,99);put(object,kIdHi,88);
 Entry e{};e.tick=nowTick;e.mgr=address(manager);e.idLo=99;e.idHi=88;e.stamp=1;e.depth=2;e.ptr[0]=address(parent);e.ptr[1]=address(object);e.vt[0]=111;e.vt[1]=222;table[99]=e;
 Entry out{};for(int i=0;i<10000;++i)check(Find(e.mgr,99,88,1,out)&&ValidateRaw(e.mgr,out,99,88)==Ok);check(locks==1);
 nowTick=2201;table[99].tick=nowTick;
 check(Find(e.mgr,99,88,1,out)&&out.tick==nowTick&&locks==2);
 check(Find(e.mgr,99,88,1,out)&&out.tick==nowTick&&locks==2);
 children[0]=0;check(Find(e.mgr,99,88,1,out)&&ValidateRaw(e.mgr,out,99,88)==BadChild);children[0]=address(object);
 put(object,kIdLo,100);check(Find(e.mgr,99,88,1,out)&&ValidateRaw(e.mgr,out,99,88)==BadId);put(object,kIdLo,99);
 put(parent,kIdLo,99);put(parent,kIdHi,88);check(ValidateRaw(e.mgr,e,99,88)==EarlierMatch);put(parent,kIdLo,0);put(parent,kIdHi,0);
 g_stampGen=2;g_stamp=2;check(!Find(e.mgr,99,88,2,out));table[99].stamp=2;check(Find(e.mgr,99,88,2,out));
 printf("Object cache: %d checks, %d failures; live path/ID checks retained\n",checks,fail);return fail?1:0;
}
