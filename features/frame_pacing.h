#pragma once
// Bounded frame-interval summaries for the public gameplay experiment. Credits: @loinyx
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace FramePacing {
struct Histogram {
    std::array<uint32_t,2048> bins{};
    uint32_t count=0,over33=0,over50=0;
    double total=0,maximum=0;
    void Add(double ms){
        if(!std::isfinite(ms)||ms<=0)return;
        const size_t bucket=ms>=1023.5?2047:static_cast<size_t>(std::ceil(ms*2));
        ++bins[bucket];++count;total+=ms;maximum=std::max(maximum,ms);
        over33+=ms>1000./30.;over50+=ms>50;
    }
    double Percentile(unsigned percent)const{
        if(!count)return 0;
        const uint64_t rank=std::max<uint64_t>(1,(uint64_t(count)*std::min(percent,100u)+99)/100);
        uint64_t seen=0;
        for(size_t i=0;i<bins.size();++i)if((seen+=bins[i])>=rank)return i==2047?maximum:i*.5;
        return maximum;
    }
};
struct Session {
    Histogram frames;
    bool active=false;
    double warming=0;
    bool Step(double ms,bool ready){
        if(!ready){if(active||frames.count||warming)*this={};return false;}
        if(!active){active=true;return false;}
        if(!std::isfinite(ms)||ms<=0)return false;
        if(warming<3000){warming+=std::min(ms,250.);return false;}
        frames.Add(ms);return frames.total>=60000;
    }
    void NextWindow(){frames={};}
};
}
