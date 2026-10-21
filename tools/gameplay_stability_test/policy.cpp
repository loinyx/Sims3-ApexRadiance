#include "frame_pacing.h"
#include "gpu_timing_policy.h"
#include "cas_tri_sort.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <numeric>
#include <random>
#include <vector>
static unsigned allocations=0;
void* operator new(size_t n){++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete(void* p,size_t)noexcept{std::free(p);}
int main(){unsigned checks=0,fail=0;auto check=[&](bool ok){++checks;if(!ok)++fail;};std::mt19937 rng(102);
 uint64_t next=0;unsigned sampled=0;
 for(uint64_t ms=0;ms<60000;++ms) sampled+=GpuTiming::Due(ms,next,true);
 check(sampled==240);check(!GpuTiming::Due(59999,next,true));check(GpuTiming::Due(60000,next,true));
 for(unsigned i=0;i<100;++i)check(GpuTiming::Due(0,next,false));
 for(unsigned n=0;n<1000;++n){std::vector<unsigned> counts(n),order(n);for(auto& x:counts)x=rng()%20;std::iota(order.begin(),order.end(),0);std::stable_sort(order.begin(),order.end(),[&](unsigned a,unsigned b){return counts[a]>counts[b];});
  std::vector<unsigned short> src(n*3),expected(n*3),out(n*3);for(auto& x:src)x=static_cast<unsigned short>(rng());for(unsigned i=0;i<n;++i)std::copy_n(src.data()+order[i]*3,3,expected.data()+i*3);
  CasTriSort::Input in{src.data(),nullptr,n*3,0,0,0};CasTriSort::Fast::Finish(in,counts.data(),out.data());check(out==expected);CasTriSort::Fast::Finish(in,counts.data(),src.data());check(src==expected);
  std::sort(counts.begin(),counts.end(),std::greater<unsigned>());const unsigned before=allocations;CasTriSort::Fast::Finish(in,counts.data(),src.data());check(allocations==before);
 }
 FramePacing::Histogram h;for(int i=0;i<980;++i)h.Add(8.33);for(int i=0;i<15;++i)h.Add(16.67);for(int i=0;i<5;++i)h.Add(100);
 check(h.count==1000&&h.Percentile(50)==8.5&&h.Percentile(95)==8.5&&h.Percentile(99)==17&&h.over50==5);
 h.Add(0);h.Add(-1);h.Add(std::numeric_limits<double>::quiet_NaN());check(h.count==1000);
 FramePacing::Histogram overflow;overflow.Add(10000);check(overflow.Percentile(99)==10000&&overflow.maximum==10000);
 FramePacing::Session s;check(!s.Step(2000,true)&&s.frames.count==0);for(int i=0;i<12;++i)s.Step(250,true);check(s.frames.count==0);check(s.Step(60000,true)&&s.frames.count==1);s.NextWindow();check(s.frames.count==0&&s.active);s.Step(8,false);check(!s.active&&s.warming==0);
 printf("CAS ordering/frame pacing: %u checks, %u failures\n",checks,fail);return fail?1:0;
}
