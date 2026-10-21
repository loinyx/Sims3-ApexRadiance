#pragma once
// Prefer broad blockers over dense narrow details within the same storage bound. Credits: @loinyx
#include "mesh_occlusion.h"
#include <span>
namespace MeshBudget {
inline double Priority(std::span<const MeshOcclusion::Triangle> triangles,float placementRise) noexcept {
    if(triangles.empty()||!std::isfinite(placementRise))return 0;
    double horizontal=0,surface=0;
    for(const auto& t:triangles) {
        const auto n=MeshOcclusion::Cross(MeshOcclusion::Sub(t.b,t.a),MeshOcclusion::Sub(t.c,t.a));
        if(!MeshOcclusion::Finite(n))return 0;
        horizontal+=std::fabs(double(n.y))*.5;
        surface+=std::sqrt(double(n.x)*n.x+double(n.y)*n.y+double(n.z)*n.z)*.5;
    }
    const double score=(horizontal+.05*surface)/triangles.size()*(1+std::clamp(double(placementRise),0.,3.));
    return std::isfinite(score)?score:0;
}
struct Entry {uint64_t key;size_t triangles;double priority;};
// Decide first; callers erase nothing unless every required replacement is possible.
inline bool Plan(std::span<Entry> entries,size_t used,size_t incoming,double priority,size_t limit,
                 size_t groupLimit,std::vector<uint64_t>& victims) {
    victims.clear();
    if(!incoming||incoming>limit||!std::isfinite(priority)||priority<=0)return false;
    if(used<=limit-incoming&&entries.size()<groupLimit)return true;
    std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.priority!=b.priority?a.priority<b.priority:a.key<b.key;});
    size_t remaining=entries.size();
    for(const auto& e:entries) {
        if(used<=limit-incoming&&remaining<groupLimit)return true;
        if(!(e.priority<priority))break;
        if(e.triangles>used)return false;
        used-=e.triangles;--remaining;victims.push_back(e.key);
    }
    if(used<=limit-incoming&&remaining<groupLimit)return true;
    victims.clear();return false;
}
}
