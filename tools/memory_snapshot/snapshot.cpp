// External read-only memory inventory. Credits: @loinyx
#define NOMINMAX
#define PSAPI_VERSION 1
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
namespace fs=std::filesystem;
struct Region {uintptr_t base;uint64_t bytes;DWORD state;};
struct Allocation {
 uintptr_t base=0;uint64_t span=0,commit=0,reserve=0,low=0,resident=0,shareable=0,privateResident=0;
 DWORD type=0;bool wsComplete=true;std::wstring mapped;std::vector<Region> regions;
};
struct Inventory {
 std::map<uintptr_t,Allocation> allocations;
 uint64_t free=0,freeLow=0,largest=0,largestLow=0,end=0,elapsed=0;
 bool complete=false,processValid=false;unsigned queryFailures=0;PROCESS_MEMORY_COUNTERS_EX process{};
};
std::string Utf8(const std::wstring& w){if(w.empty())return {};int n=WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),nullptr,0,nullptr,nullptr);std::string s(n,'\0');WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),s.data(),n,nullptr,nullptr);return s;}
std::string Csv(const std::string& s){std::string out="\"";for(char c:s){if(c=='"')out+='"';out+=c;}return out+'"';}
Inventory Take(HANDLE process,bool resident){
 Inventory out;const auto start=GetTickCount64();uint64_t at=0x10000,hi=0xffff0000ull;MEMORY_BASIC_INFORMATION m{};
 for(;at<hi;){
  if(VirtualQueryEx(process,reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(at)),&m,sizeof m)!=sizeof m)break;
  uint64_t end=std::min(hi,uint64_t(reinterpret_cast<uintptr_t>(m.BaseAddress))+m.RegionSize);if(end<=at)break;
  uint64_t bytes=end-at,low=at<0x80000000ull?std::min(end,0x80000000ull)-at:0;
  if(m.State==MEM_FREE){out.free+=bytes;out.freeLow+=low;out.largest=std::max(out.largest,bytes);out.largestLow=std::max(out.largestLow,low);}
  else{auto base=reinterpret_cast<uintptr_t>(m.AllocationBase);auto& a=out.allocations[base];a.base=base;a.type=m.Type;a.span+=bytes;a.low+=low;
   if(m.State==MEM_COMMIT)a.commit+=bytes;else if(m.State==MEM_RESERVE)a.reserve+=bytes;
   a.regions.push_back({static_cast<uintptr_t>(at),bytes,m.State});}
  at=end;if(out.allocations.size()>65536)break;
 }
 out.end=at;out.complete=at>=hi;PSAPI_WORKING_SET_EX_INFORMATION pages[1024]{};
 for(auto& pair:out.allocations){auto& a=pair.second;
  if(a.type==MEM_IMAGE||a.type==MEM_MAPPED)for(const auto& rg:a.regions)if(rg.state==MEM_COMMIT){
   wchar_t name[4096]{};DWORD n=GetMappedFileNameW(process,reinterpret_cast<void*>(rg.base),name,4096);
   if(n&&n<4096){a.mapped.assign(name,n);break;}}
  if(!resident)continue;
  for(const auto& rg:a.regions){if(rg.state!=MEM_COMMIT)continue;
   for(uint64_t offset=0;offset<rg.bytes;){
    unsigned count=unsigned(std::min<uint64_t>(1024,(rg.bytes-offset+4095)/4096));
    for(unsigned i=0;i<count;++i){pages[i]={};pages[i].VirtualAddress=reinterpret_cast<void*>(rg.base+static_cast<uintptr_t>(offset+uint64_t(i)*4096));}
    if(!QueryWorkingSetEx(process,pages,count*sizeof pages[0])){a.wsComplete=false;++out.queryFailures;}
    else for(unsigned i=0;i<count;++i)if(pages[i].VirtualAttributes.Valid){a.resident+=4096;if(pages[i].VirtualAttributes.Shared)a.shareable+=4096;else a.privateResident+=4096;}
    offset+=uint64_t(count)*4096;
   }
  }
 }
 out.process.cb=sizeof out.process;out.processValid=GetProcessMemoryInfo(process,reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&out.process),sizeof out.process)!=FALSE;
 out.elapsed=GetTickCount64()-start;return out;
}
bool Read(HANDLE p,uintptr_t address,void* dst,size_t size){SIZE_T got=0;return ReadProcessMemory(p,reinterpret_cast<LPCVOID>(address),dst,size,&got)&&got==size;}
bool Match(HANDLE p,uintptr_t address,std::initializer_list<unsigned char> expected){unsigned char buf[64]{};return expected.size()<=sizeof buf&&Read(p,address,buf,expected.size())&&std::equal(expected.begin(),expected.end(),buf);}
bool SteamLayout(HANDLE p){
 IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS32 nt{};
 return Read(p,0x400000,&dos,sizeof dos)&&dos.e_magic==IMAGE_DOS_SIGNATURE&&dos.e_lfanew>0&&dos.e_lfanew<4096&&Read(p,0x400000+dos.e_lfanew,&nt,sizeof nt)&&
 nt.Signature==IMAGE_NT_SIGNATURE&&nt.FileHeader.Machine==IMAGE_FILE_MACHINE_I386&&nt.FileHeader.TimeDateStamp==0x52DEC247&&nt.OptionalHeader.ImageBase==0x400000&&
 Match(p,0xe4dd30,{0xa1,0xa0,0x25,0x22,0x01,0xc3})&&Match(p,0xe4dd40,{0xa1,0xb4,0x25,0x22,0x01,0xc3});
}
void WriteCounters(HANDLE p,FILE* file){
 fprintf(file,"category,valid,address,count,bytes,free_bytes,limit_bytes,large_threshold_bytes,top_down_flag,note\n");
 if(!SteamLayout(p)){fprintf(file,"game_layout,false,0,0,0,0,0,0,-1,Unverified executable; game counter reads disabled\n");return;}
 uint32_t heap=0,free=0,limit=0,againHeap=0;
 bool ok=Read(p,0x12225a0,&heap,4)&&Read(p,0x12225b4,&free,4)&&Read(p,0x12225a4,&limit,4)&&Read(p,0x12225a0,&againHeap,4)&&heap==againHeap&&free<=heap;
 fprintf(file,"mono_boehm_heap,%s,0x012225A0,0,%u,%u,%u,0,-1,Heap minus free matches Mono wrapper; not exact live object bytes\n",ok?"true":"false",heap,free,limit);
 for(auto global:{0x011cb864u,0x011d4bc8u}){
  uint32_t allocator=0,cs=0,n=0,bytes=0,again=0,threshold=0;unsigned char topDown=0;
  bool valid=Read(p,global,&allocator,4)&&allocator&&allocator<0xfffff000u&&Read(p,allocator+0x4e4,&cs,4)&&cs==allocator+0x4e8&&Read(p,allocator+0x488,&n,4)&&Read(p,allocator+0x48c,&bytes,4)&&Read(p,allocator+0x494,&threshold,4)&&Read(p,allocator+0x498,&topDown,1)&&Read(p,allocator+0x488,&again,4)&&n==again;
  fprintf(file,"%s,%s,0x%08X,%u,%u,0,0,%u,%u,Only large-block accounting; not total heap; allocator globals may alias\n",global==0x11cb864u?"native_primary_large":"runtime_native_large",valid?"true":"false",allocator,n,bytes,threshold,unsigned(topDown));
 }
}
void WriteCaches(HANDLE p,FILE* file,uintptr_t resourceGlobal){
 fprintf(file,"valid,cache_index,cache_address,budget_accounting_bytes,other_accounting_bytes,idle_accounting_bytes,note\n");
 uint32_t rs=0,vt=0;
 if(!resourceGlobal||!SteamLayout(p)||!Read(p,resourceGlobal,&rs,4)||!rs||!Read(p,rs,&vt,4)||vt!=0x00ffe2f0){
  fprintf(file,"false,0,0,0,0,0,No verified Apex resource pointer or native vtable; cache reads disabled\n");return;
 }
 for(unsigned i=0;i<2;++i){
  uint32_t cache=0,values[4]{};
  bool ok=Read(p,rs+0x1e0+i*4,&cache,4)&&cache&&Read(p,cache+0x8c,values,sizeof values);
  fprintf(file,"%s,%u,0x%08X,%u,%u,%u,Raw native accounting; idle does not equal OS RAM reclaimable\n",ok?"true":"false",i,cache,values[0],values[2],values[3]);
 }
}
int wmain(int argc,wchar_t** argv){
 bool self=argc>=2&&std::wstring(argv[1])==L"--self-test";
 bool fixtureMode=argc>=2&&std::wstring(argv[1])==L"--fixture";
 if(argc<3){fwprintf(stderr,L"Usage: ApexMemorySnapshot.exe <pid|--self-test> <new-output-folder> [--resident]\n");return 2;}
 bool resident=self;uintptr_t resourceGlobal=0;
 for(int i=3;i<argc;++i){const std::wstring flag=argv[i];if(flag==L"--resident")resident=true;
  else if(flag==L"--resource-global"&&i+1<argc)resourceGlobal=wcstoul(argv[++i],nullptr,0);else{fprintf(stderr,"Unknown option\n");return 2;}}
 DWORD target=(self||fixtureMode)?GetCurrentProcessId():wcstoul(argv[1],nullptr,10);fs::path output=argv[2];
 if(fs::exists(output)){fwprintf(stderr,L"Output folder exists; nothing overwritten.\n");return 2;}
 HANDLE process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,target);if(!process){fprintf(stderr,"OpenProcess failed: %lu\n",GetLastError());return 3;}
 BOOL wow=false;if(!self&&!fixtureMode&&(!IsWow64Process(process,&wow)||!wow)){fprintf(stderr,"A 32-bit target on 64-bit Windows is required.\n");CloseHandle(process);return 3;}
 void* fixture=nullptr;HANDLE fixtureFile=INVALID_HANDLE_VALUE,fixtureMap=nullptr;void* view=nullptr;
 if(self||fixtureMode){fixture=VirtualAlloc(nullptr,64u<<20,MEM_RESERVE,PAGE_READWRITE);assert(fixture);assert(VirtualAlloc(fixture,8u<<20,MEM_COMMIT,PAGE_READWRITE)==fixture);memset(fixture,7,8u<<20);
  wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);fixtureFile=CreateFileW(exe,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);assert(fixtureFile!=INVALID_HANDLE_VALUE);
  fixtureMap=CreateFileMappingW(fixtureFile,nullptr,PAGE_READONLY,0,0,nullptr);assert(fixtureMap);view=MapViewOfFile(fixtureMap,FILE_MAP_READ,0,0,0);assert(view);}
 if(fixtureMode){
  fs::create_directories(output);FILE* info=nullptr;_wfopen_s(&info,(output/L"Fixture.json").c_str(),L"wb");assert(info);
  fprintf(info,"{\"pid\":%lu,\"allocation_base\":%u,\"mapped_base\":%u}\n",target,unsigned(reinterpret_cast<uintptr_t>(fixture)),unsigned(reinterpret_cast<uintptr_t>(view)));fclose(info);
  const auto name=L"Local\\ApexMemoryFixture-"+std::to_wstring(target);HANDLE stop=CreateEventW(nullptr,TRUE,FALSE,name.c_str());assert(stop);WaitForSingleObject(stop,30000);CloseHandle(stop);
  UnmapViewOfFile(view);CloseHandle(fixtureMap);CloseHandle(fixtureFile);VirtualFree(fixture,0,MEM_RELEASE);CloseHandle(process);return 0;
 }
 auto inventory=Take(process,resident);
 if(self){assert(inventory.complete&&inventory.processValid&&inventory.freeLow<=inventory.free);auto& a=inventory.allocations.at(reinterpret_cast<uintptr_t>(fixture));assert(a.commit==8u<<20&&a.reserve==56u<<20&&a.privateResident>=8u<<20&&a.wsComplete);auto& mapped=inventory.allocations.at(reinterpret_cast<uintptr_t>(view));assert(mapped.type==MEM_MAPPED&&!mapped.mapped.empty());assert(!SteamLayout(process));}
 fs::create_directories(output);FILE* csv=nullptr;_wfopen_s(&csv,(output/L"Allocations.csv").c_str(),L"wb");if(!csv){CloseHandle(process);return 4;}
 fprintf(csv,"allocation_base,type,span_bytes,committed_va_bytes,reserved_va_bytes,span_below_2g_bytes,resident_sampled,resident_bytes,resident_private_bytes,resident_shareable_bytes,working_set_complete,mapped_file\n");
 std::vector<const Allocation*> sorted;for(auto& pair:inventory.allocations)sorted.push_back(&pair.second);std::sort(sorted.begin(),sorted.end(),[](const Allocation* a,const Allocation* b){return a->commit>b->commit;});
 for(auto* a:sorted){auto name=Csv(Utf8(a->mapped));fprintf(csv,"0x%08X,%s,%llu,%llu,%llu,%llu,%s,%llu,%llu,%llu,%s,%s\n",unsigned(a->base),a->type==MEM_PRIVATE?"private":a->type==MEM_IMAGE?"image":a->type==MEM_MAPPED?"mapped":"unknown",a->span,a->commit,a->reserve,a->low,resident?"true":"false",a->resident,a->privateResident,a->shareable,a->wsComplete?"true":"false",name.c_str());}fclose(csv);
 FILE* counters=nullptr;_wfopen_s(&counters,(output/L"EngineCounters.csv").c_str(),L"wb");if(!counters){CloseHandle(process);return 4;}WriteCounters(process,counters);fclose(counters);
 FILE* caches=nullptr;_wfopen_s(&caches,(output/L"ResourceCaches.csv").c_str(),L"wb");if(!caches){CloseHandle(process);return 4;}WriteCaches(process,caches,resourceGlobal);fclose(caches);
 FILE* summary=nullptr;_wfopen_s(&summary,(output/L"Summary.json").c_str(),L"wb");if(!summary){CloseHandle(process);return 4;}
 fprintf(summary,"{\"pid\":%lu,\"scan_complete\":%s,\"scan_end\":%llu,\"elapsed_ms\":%llu,\"process_valid\":%s,\"private_bytes\":%llu,\"working_set_bytes\":%llu,\"free_va_bytes\":%llu,\"free_below_2g_bytes\":%llu,\"largest_free_va_bytes\":%llu,\"largest_below_2g_bytes\":%llu,\"resident_sampled\":%s,\"working_set_query_failures\":%u,\"approximate_concurrent_snapshot\":true}\n",target,inventory.complete?"true":"false",inventory.end,inventory.elapsed,inventory.processValid?"true":"false",uint64_t(inventory.process.PrivateUsage),uint64_t(inventory.process.WorkingSetSize),inventory.free,inventory.freeLow,inventory.largest,inventory.largestLow,resident?"true":"false",inventory.queryFailures);fclose(summary);
 if(self){UnmapViewOfFile(view);CloseHandle(fixtureMap);CloseHandle(fixtureFile);VirtualFree(fixture,0,MEM_RELEASE);puts("Self-test passed: reserve/commit, private resident pages, mapped file, partitions and foreign-layout gate");}
 CloseHandle(process);printf("Snapshot: %zu allocations in %llu ms; approximate while target continues\n",inventory.allocations.size(),inventory.elapsed);return 0;
}
