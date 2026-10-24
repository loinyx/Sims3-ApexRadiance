// Extracted scene drain regression checks with synthetic native node layouts.
// Part of Apex Radiance. Credits: @loinyx
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstdio>
#include <format>
#include <functional>
#include <vector>
#include <cstdlib>
struct Link {Link* next;Link* prev;};
struct Node {uintptr_t vt;uint8_t pad[20];Link link;uint8_t pad2[16];void* owner;int id;};
struct Holder {uint8_t pad[24];uint32_t count;uint8_t pad2[4];Link head;};
static_assert(offsetof(Node,link)==0x18&&offsetof(Node,owner)==0x30&&offsetof(Holder,head)==0x20);
constexpr uint32_t kLinkOff=0x18,kOwnerOff=0x30,kListOff=0x20,kCounterOff=0x18,kUpdateSlot=0x48,kMinNodes=8,kCountCap=1u<<20;
constexpr bool kPublicBuild=PUBLIC_VARIANT!=0;
struct Counter {uint64_t value=0;void Add(uint64_t n=1){value+=n;}};
Counter c_drawState,c_repaired,c_foreignOwner;
std::atomic<bool>g_stopped=false,g_loggedRepair=false;
std::atomic<uint32_t>g_deferredCount=0;
uintptr_t g_deadVtable=0,g_swarmVtable=0,g_furVtable=0;
std::vector<std::pair<Link*,void*>>g_deferred;
struct RegGuard{RegGuard(){} ~RegGuard(){}};
namespace HookGuard{template<class F>void Try(const char*,F f){f();}}
#define LOG_WARNING(x) ((void)0)
template<class F>void StopForCheck(F){g_stopped=true;}
int64_t Qpc(){return 100;}
bool allocationFails=false;
bool ReserveRecords(std::vector<Link*>&r,uint32_t n){if(allocationFails)return false;r.reserve(n);g_deferred.reserve(g_deferred.size()+n);return true;}
void DropHolderLocked(void*h){std::erase_if(g_deferred,[h](auto&e){return e.second==h;});}
bool LinkConsistent(const Link*l){return l->next&&l->prev&&l->next->prev==l&&l->prev->next==l;}
int failedNode=-1;
bool NodeLooksAlive(const uint8_t*n,uintptr_t*vt){*vt=*(const uintptr_t*)n;return ((const Node*)n)->id!=failedNode;}
void UnlinkDead(const Link*l){l->prev->next=l->next;l->next->prev=l->prev;}
std::vector<int>seen;
std::function<void(Node*)>during;
void __fastcall Update(void*p,void*){Node*n=(Node*)p;seen.push_back(n->id);if(during)during(n);}
void* __fastcall Bounds(void*,void*,float*b){return b;}
void __fastcall Spatial(void*,void*,void*){}
using FnUpdate=void(__fastcall*)(void*,void*);
auto g_bounds=&Bounds;auto g_spatial=&Spatial;
#include "production.h"
std::array<uintptr_t,32>ordinary{},swarm{},fur{};
void push(Link*h,Node*n){n->link={h,h->prev};h->prev->next=&n->link;h->prev=&n->link;}
void remove(Node*n){auto*l=&n->link;l->prev->next=l->next;l->next->prev=l->prev;l->next=l->prev=nullptr;}
int checks=0;
void Check(bool b,int line){++checks;if(!b){std::printf("FAIL %d line %d\n",checks,line);std::exit(1);}}
#define check(x) Check((x),__LINE__)
std::vector<int>queue(Holder&h){std::vector<int>r;for(Link*l=h.head.prev;l!=&h.head;l=l->prev){check(LinkConsistent(l));r.push_back(((Node*)((uint8_t*)l-kLinkOff))->id);check(r.size()<100);}return r;}
void run(uint32_t mask,bool oom=false,bool timed=false){
 Holder h{};h.head={&h.head,&h.head};std::array<Node,24>nodes{};g_deferred.clear();seen.clear();during={};g_stopped=false;failedNode=-1;allocationFails=oom;
 for(int i=0;i<24;i++){nodes[i].id=i;nodes[i].owner=&h;nodes[i].vt=(uintptr_t)(i<16&&((mask>>i)&1)?((i&1)?fur.data():swarm.data()):ordinary.data());push(&h.head,&nodes[i]);}
 std::vector<int>expected,leftExpected;for(int i=23;i>=0;i--){if(i>=16||((mask>>i)&1)||oom)expected.push_back(i);else leftExpected.push_back(i);}
 uint32_t left=0;auto done=BudgetedDrain((uint8_t*)&h,8,timed?50:1000,&left);
 // OOM fallback drains ordinary nodes after the critical nodes; each must occur exactly once.
 if(oom){auto a=seen;std::sort(a.begin(),a.end());check(a.size()==24);for(int i=0;i<24;i++)check(a[i]==i);check(left==0);}
 else{check(seen==expected);check(queue(h)==leftExpected);check(left==leftExpected.size());}
 check(done==seen.size());check(h.count==done);check(g_deferred.size()==left);check(LinkConsistent(&h.head));
}
void callbackCases(){
 for(int mode=0;mode<4;mode++){
 Holder h{};h.head={&h.head,&h.head};std::array<Node,25>n{};seen.clear();g_deferred.clear();g_stopped=false;allocationFails=false;failedNode=-1;
 for(int i=0;i<24;i++){n[i].id=i;n[i].owner=&h;n[i].vt=(uintptr_t)(i==10?swarm.data():ordinary.data());push(&h.head,&n[i]);}
 n[24].id=24;n[24].owner=&h;n[24].vt=(uintptr_t)ordinary.data();
 during=[&](Node*x){if(x->id!=10)return;if(mode==0)remove(&n[14]);if(mode==1)remove(&n[2]);if(mode==2)push(&h.head,&n[24]);if(mode==3)push(&h.head,x);};
 uint32_t left;BudgetedDrain((uint8_t*)&h,8,1000,&left);auto q=queue(h);std::vector<int>expect;
 for(int i=15;i>=0;i--)if(i!=10&&!(mode==0&&i==14)&&!(mode==1&&i==2))expect.push_back(i);
 if(mode==2)expect.push_back(24);if(mode==3)expect.push_back(10);if(q!=expect){std::printf("mode=%d queue:",mode);for(int v:q)std::printf(" %d",v);std::printf(" expected:");for(int v:expect)std::printf(" %d",v);std::puts("");}check(q==expect);check(left==expect.size()-(mode>=2?1:0));check(seen==std::vector<int>({23,22,21,20,19,18,17,16,10}));
 }
 during={};
 if(!kPublicBuild){
 Holder h{};h.head={&h.head,&h.head};std::array<Node,24>n{};seen.clear();g_deferred.clear();g_stopped=false;failedNode=10;
 for(int i=0;i<24;i++){n[i].id=i;n[i].owner=&h;n[i].vt=(uintptr_t)(i==10?swarm.data():ordinary.data());push(&h.head,&n[i]);}
 uint32_t left;BudgetedDrain((uint8_t*)&h,8,1000,&left);std::vector<int>expect;for(int i=15;i>=0;i--)expect.push_back(i);check(queue(h)==expect);check(g_stopped);check(left==16);failedNode=-1;
 }
}
void emptyCase(){Holder h{};h.head={&h.head,&h.head};g_deferred.clear();allocationFails=false;uint32_t left=99;check(BudgetedDrain((uint8_t*)&h,8,0,&left)==0);check(left==0);check(LinkConsistent(&h.head));}
int main(){ordinary[kUpdateSlot/4]=swarm[kUpdateSlot/4]=fur[kUpdateSlot/4]=(uintptr_t)&Update;g_swarmVtable=(uintptr_t)swarm.data();g_furVtable=(uintptr_t)fur.data();
 for(uint32_t mask=0;mask<65536;mask++)run(mask);
 emptyCase();run(0,true);run(0xffff,true);run(0x5555,true);run(0xaaaa,false,true);callbackCases();std::printf("%d checks passed, public=%d\n",checks,PUBLIC_VARIANT);
}
