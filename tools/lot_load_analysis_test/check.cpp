#include "../../features/lot_load_analysis.cpp"
#include <cassert>
#include <cstring>
#include <iostream>
uint32_t g_exeTimestamp = 0;
namespace ApexLog { void Write(Level,const std::string&,const std::source_location&) noexcept {} }
namespace ApexPaths {
bool EnsureApexDirectory() { return true; }
const std::wstring& ApexDirectory() {
    static std::wstring s=[] { wchar_t p[MAX_PATH]{}; GetCurrentDirectoryW(MAX_PATH,p); return std::wstring(p)+L"\\"; }(); return s;
}
}
int main() {
    alignas(8) unsigned char renderer[0x40]{}, lot[0xD0]{};
    const uint64_t id=123;
    std::memcpy(renderer+0x10,&id,sizeof id);
    std::memcpy(lot+0x48,&id,sizeof id);
    assert(!LotLoadAnalysis::ReadRenderer(renderer).valid);
    g_exeTimestamp=0x52DEC247;
    uint32_t stage=17; std::memcpy(renderer+0x24,&stage,sizeof stage);
    auto before=LotLoadAnalysis::ReadRenderer(renderer);
    assert(before.valid && before.lot==id && before.stage==17);
    LotLoadAnalysis::Request(lot,true);
    auto start=LotLoadAnalysis::Clock(); Sleep(2); auto end=LotLoadAnalysis::Clock();
    LotLoadAnalysis::Sample(before,before,start,end); // a wait without stage progress
    stage=18; std::memcpy(renderer+0x24,&stage,sizeof stage);
    auto next=LotLoadAnalysis::ReadRenderer(renderer);
    LotLoadAnalysis::Sample(before,next,end,LotLoadAnalysis::Clock());
    stage=22; std::memcpy(renderer+0x24,&stage,sizeof stage); renderer[0x1E]=1;
    auto done=LotLoadAnalysis::ReadRenderer(renderer);
    assert(done.valid && done.done);
    auto finalStart=LotLoadAnalysis::Clock();
    auto finalEnd=LotLoadAnalysis::Clock();
    LotLoadAnalysis::Sample(next,done,finalStart,finalEnd);
    const auto queued=LotLoadAnalysis::size;
    LotLoadAnalysis::Sample(next,done,finalEnd+1,finalEnd);
    assert(LotLoadAnalysis::size==queued);
    LotLoadAnalysis::Pump();
    assert(LotLoadAnalysis::size==0);
    for(size_t i=0;i<LotLoadAnalysis::kCapacity+1;++i) LotLoadAnalysis::Sample(before,before,start,end);
    assert(LotLoadAnalysis::dropped.load()==1);
    auto epoch=LotLoadAnalysis::captureEpoch;
    LotLoadAnalysis::Reset();
    assert(LotLoadAnalysis::size==0 && LotLoadAnalysis::captureEpoch==epoch+1);
    std::cout << "Lot analysis: version gate, renderer fields, waiting/progress/completion, queue drain and overflow passed\n";
}
