#pragma once
// Opaque instanced geometry supplements floor/wall rays. Credits: @loinyx
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numeric>
#include <vector>

namespace MeshOcclusion {
struct Vec {float x=0,y=0,z=0;};
inline Vec Sub(Vec a,Vec b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
inline Vec Cross(Vec a,Vec b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline float Dot(Vec a,Vec b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline bool Finite(Vec a){return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z);}
// The captured instanced VS divides POSITION0 by w, rotates xz and shears y along x.
inline bool Decode(const float p[4],const float place[4],const float turn[4],Vec& out){
    if(!std::isfinite(p[3])||std::fabs(p[3])<1e-6f)return false;
    const Vec l{p[0]/p[3],p[1]/p[3],p[2]/p[3]};
    out={l.x*turn[0]-l.z*turn[2]+place[0],l.y+l.x*turn[1]+place[1],l.x*turn[2]+l.z*turn[0]+place[2]};
    return Finite(out)&&std::fabs(l.x)<64&&std::fabs(l.y)<64&&std::fabs(l.z)<64;
}
struct Triangle {Vec a,b,c;};
struct Box {
    Vec lo{1e30f,1e30f,1e30f},hi{-1e30f,-1e30f,-1e30f};
    void Add(Vec p){lo={std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};hi={std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)};}
    bool Hits(Vec a,Vec d,float low,float high)const{
        const float p[3]={a.x,a.y,a.z},v[3]={d.x,d.y,d.z},l[3]={lo.x,lo.y,lo.z},h[3]={hi.x,hi.y,hi.z};
        for(int k=0;k<3;++k){
            if(std::fabs(v[k])<1e-8f){if(p[k]<l[k]||p[k]>h[k])return false;continue;}
            float x=(l[k]-p[k])/v[k],y=(h[k]-p[k])/v[k];if(x>y)std::swap(x,y);
            low=std::max(low,x);high=std::min(high,y);if(high<low)return false;
        }return true;
    }
};
inline bool Hits(const Triangle& t,Vec a,Vec d,float low,float high){
    const Vec e1=Sub(t.b,t.a),e2=Sub(t.c,t.a),p=Cross(d,e2);const float det=Dot(e1,p);
    if(std::fabs(det)<1e-8f)return false;
    const float inv=1/det;const Vec s=Sub(a,t.a);const float u=Dot(s,p)*inv;
    if(u<0||u>1)return false;const Vec q=Cross(s,e1);const float v=Dot(d,q)*inv;
    if(v<0||u+v>1)return false;const float distance=Dot(e2,q)*inv;
    return distance>low&&distance<high;
}
class Scene {
    struct Node {Box box;uint32_t begin=0,count=0,left=0,right=0;};
    std::vector<Triangle> triangles;
    std::vector<uint32_t> order;
    std::vector<Node> nodes;
    uint32_t Build(uint32_t begin,uint32_t end){
        Node n;n.begin=begin;n.count=end-begin;Box centres;
        for(auto i=begin;i<end;++i){const auto& t=triangles[order[i]];n.box.Add(t.a);n.box.Add(t.b);n.box.Add(t.c);centres.Add({(t.a.x+t.b.x+t.c.x)/3,(t.a.y+t.b.y+t.c.y)/3,(t.a.z+t.b.z+t.c.z)/3});}
        const auto index=static_cast<uint32_t>(nodes.size());nodes.push_back(n);
        if(end-begin>8){
            const Vec extent=Sub(centres.hi,centres.lo);const int axis=extent.x>extent.y?(extent.x>extent.z?0:2):(extent.y>extent.z?1:2);
            const auto centre=[&](uint32_t i){const auto& t=triangles[i];return axis==0?t.a.x+t.b.x+t.c.x:axis==1?t.a.y+t.b.y+t.c.y:t.a.z+t.b.z+t.c.z;};
            const auto middle=begin+(end-begin)/2;
            std::nth_element(order.begin()+begin,order.begin()+middle,order.begin()+end,[&](auto a,auto b){return centre(a)<centre(b);});
            const auto left=Build(begin,middle),right=Build(middle,end);nodes[index].left=left;nodes[index].right=right;nodes[index].count=0;
        }return index;
    }
public:
    explicit Scene(std::vector<Triangle> data):triangles(std::move(data)){
        std::erase_if(triangles,[](const auto& t){return !Finite(t.a)||!Finite(t.b)||!Finite(t.c)||Dot(Cross(Sub(t.b,t.a),Sub(t.c,t.a)),Cross(Sub(t.b,t.a),Sub(t.c,t.a)))<1e-12f;});
        order.resize(triangles.size());std::iota(order.begin(),order.end(),0u);nodes.reserve(triangles.size()*2);
        if(!triangles.empty())Build(0,static_cast<uint32_t>(triangles.size()));
    }
    size_t Count()const{return triangles.size();}
    bool Blocked(Vec a,Vec b)const{
        if(nodes.empty()||!Finite(a)||!Finite(b))return false;const Vec d=Sub(b,a);const float length=std::sqrt(Dot(d,d));
        if(!(length>.04f))return false;const float low=.02f/length,high=1-low;
        uint32_t stack[64]={0};unsigned count=1;
        while(count){const auto& n=nodes[stack[--count]];if(!n.box.Hits(a,d,low,high))continue;
            if(n.count){for(auto i=n.begin;i<n.begin+n.count;++i)if(Hits(triangles[order[i]],a,d,low,high))return true;}
            else{if(count+2>64)return false;stack[count++]=n.left;stack[count++]=n.right;}
        }return false;
    }
};
inline std::atomic<std::shared_ptr<const Scene>> snapshot;
inline std::atomic<uint32_t> generation{1},reset{1};
inline void Publish(std::vector<Triangle> triangles){snapshot.store(std::make_shared<const Scene>(std::move(triangles)),std::memory_order_release);generation.fetch_add(1,std::memory_order_release);}
inline void Reset(){snapshot.store({},std::memory_order_release);generation.fetch_add(1,std::memory_order_release);reset.fetch_add(1,std::memory_order_release);}
inline bool Blocked(const float* a,const float* b){
    static thread_local uint32_t known=0;static thread_local std::shared_ptr<const Scene> scene;
    const auto now=generation.load(std::memory_order_acquire);if(now!=known){scene=snapshot.load(std::memory_order_acquire);known=now;}
    return scene&&scene->Blocked({a[0],a[1],a[2]},{b[0],b[1],b[2]});
}
}
