#include <atomic>
#include <cstdio>
#include <cstdint>
#include <format>
#include <thread>
#include <vector>
constexpr bool kPublicBuild=true;
#define LOG_ERROR(x) ((void)0)
static unsigned nativeCalls=0;static bool mismatch=false,builder=false;
void* __cdecl NativeType(void*,void* t){++nativeCalls;return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(t)+(mismatch?0x200000:0x100000));}
void __cdecl NativeFree(void*,int){}
namespace EntryChain {enum class Site{MonoTypeGetObject,MonoDomainFree};enum class Layer{ScriptMath};void* Next(Site s,Layer){return s==Site::MonoTypeGetObject?reinterpret_cast<void*>(&NativeType):reinterpret_cast<void*>(&NativeFree);}}
void* ReflectionInfo(void* t){return builder?reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(t)+0x100000):nullptr;}
#include "script-types.inc"
int main(){int fail=0,checks=0;auto check=[&](bool ok){++checks;if(!ok)++fail;};void* domain=reinterpret_cast<void*>(0x1000);
 g_typeOn=true;auto* type=reinterpret_cast<void*>(0x4000);auto* result=NativeType(domain,type);
 StoreType(domain,type,result,1);check(LookupType(domain,type,1)==result);check(LookupType(domain,type,2)==nullptr);
 t_typeLast={};check(LookupType(domain,type,1)==result);
 nativeCalls=0;for(int i=0;i<10000;++i)check(Hook_TypeGetObject(domain,type)==result);check(nativeCalls==kTypeStartupChecks);
 Hook_DomainFree(domain,0);check(LookupType(domain,type,g_typeGen.load())==nullptr);check(Hook_TypeGetObject(domain,type)==result);
 g_typeGen.fetch_add(1);g_typeHits=0;builder=true;type=reinterpret_cast<void*>(0x5000);nativeCalls=0;
 Hook_TypeGetObject(domain,type);Hook_TypeGetObject(domain,type);check(nativeCalls==2);builder=false;
 g_typeGen.fetch_add(1);g_typeHits=0;type=reinterpret_cast<void*>(0x6000);Hook_TypeGetObject(domain,type);mismatch=true;
 check(Hook_TypeGetObject(domain,type)==reinterpret_cast<void*>(0x206000));check(g_typeSelfDisabled.load());mismatch=false;
 std::atomic<int> bad=0;std::vector<std::thread> threads;
 for(int worker=0;worker<4;++worker)threads.emplace_back([&,worker]{for(unsigned i=0;i<100000;++i){auto* t=reinterpret_cast<void*>(((i+worker*13)%8192+1)*4);auto* r=reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(t)+0x100000);StoreType(domain,t,r,20);t_typeLast={};auto* got=LookupType(domain,t,20);if(got&&got!=r)++bad;}});
 for(auto& t:threads)t.join();check(bad==0);
 printf("Script type cache: %d checks, 400000 concurrent operations, %d failures\n",checks,fail);return fail?1:0;
}
