#pragma once
#include <cstdint>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Small, uncompressed ZIP packages. Only named payloads are returned; never extract paths.
namespace ProfilePackage {
inline constexpr size_t kLimit = 34u * 1024u * 1024u;
struct Entry { std::string name, bytes; };
inline uint32_t Crc(std::string_view bytes) {
    static constexpr auto table=[] {
        std::array<uint32_t,256> result{};
        for(uint32_t n=0;n<256;++n) {
            uint32_t v=n;
            for(int i=0;i<8;++i) v=(v>>1) ^ (0xedb88320u & (0u-(v&1u)));
            result[n]=v;
        }
        return result;
    }();
    uint32_t crc = ~0u;
    for (unsigned char c : bytes) crc=(crc>>8)^table[(crc^c)&255u];
    return ~crc;
}
inline bool SafeName(std::string_view name) {
    if (name.empty() || name.size()>255 || name.front()=='/' || name.back()=='/') return false;
    if (name.find('\\')!=name.npos || name.find(':')!=name.npos || name.find('\0')!=name.npos) return false;
    size_t start=0;
    while(start<name.size()) {
        size_t end=name.find('/',start); if(end==name.npos) end=name.size();
        const auto part=name.substr(start,end-start);
        if(part.empty() || part=="." || part=="..") return false;
        start=end+1;
    }
    return true;
}
inline void Put(std::string& out,uint32_t n,int count) { for(int i=0;i<count;++i) out.push_back(char(n>>(8*i))); }
inline uint32_t Get(std::string_view in,size_t p,int count) {
    if(p>in.size() || size_t(count)>in.size()-p) throw std::runtime_error("Incomplete profile package");
    uint32_t n=0; for(int i=0;i<count;++i) n|=uint32_t(uint8_t(in[p+i]))<<(8*i); return n;
}
inline std::string Write(const std::vector<Entry>& entries) {
    if(entries.empty() || entries.size()>2) throw std::runtime_error("Invalid profile package");
    std::string out, directory;
    for(const auto& e:entries) {
        if(!SafeName(e.name) || e.bytes.size()>kLimit || out.size()+e.bytes.size()>kLimit) throw std::runtime_error("Invalid profile package");
        const auto offset=uint32_t(out.size()), crc=Crc(e.bytes), size=uint32_t(e.bytes.size());
        Put(out,0x04034b50,4); Put(out,20,2); Put(out,0x800,2); Put(out,0,2); Put(out,0,2); Put(out,33,2);
        Put(out,crc,4); Put(out,size,4); Put(out,size,4); Put(out,uint32_t(e.name.size()),2); Put(out,0,2);
        out+=e.name; out+=e.bytes;
        Put(directory,0x02014b50,4); Put(directory,20,2); Put(directory,20,2); Put(directory,0x800,2);
        Put(directory,0,2); Put(directory,0,2); Put(directory,33,2); Put(directory,crc,4); Put(directory,size,4); Put(directory,size,4);
        Put(directory,uint32_t(e.name.size()),2); Put(directory,0,2); Put(directory,0,2); Put(directory,0,2); Put(directory,0,2);
        Put(directory,0,4); Put(directory,offset,4); directory+=e.name;
    }
    const auto offset=uint32_t(out.size()); out+=directory;
    Put(out,0x06054b50,4); Put(out,0,2); Put(out,0,2); Put(out,uint32_t(entries.size()),2); Put(out,uint32_t(entries.size()),2);
    Put(out,uint32_t(directory.size()),4); Put(out,offset,4); Put(out,0,2);
    if(out.size()>kLimit) throw std::runtime_error("The profile package is too large");
    return out;
}
inline std::vector<Entry> Read(std::string_view in) {
    if(in.size()<22 || in.size()>kLimit) throw std::runtime_error("Invalid profile package");
    const size_t end=in.size()-22;
    if(Get(in,end,4)!=0x06054b50 || Get(in,end+4,2) || Get(in,end+6,2) || Get(in,end+20,2)) throw std::runtime_error("Invalid profile package");
    const auto count=Get(in,end+10,2), dirSize=Get(in,end+12,4), dirOffset=Get(in,end+16,4);
    if(!count || count>2 || count!=Get(in,end+8,2) || dirOffset>end || dirSize!=end-dirOffset) throw std::runtime_error("Invalid profile package");
    std::vector<Entry> entries; size_t p=dirOffset, localEnd=0;
    for(uint32_t i=0;i<count;++i) {
        if(Get(in,p,4)!=0x02014b50) throw std::runtime_error("Invalid profile package");
        const auto flags=Get(in,p+8,2), method=Get(in,p+10,2), crc=Get(in,p+16,4), size=Get(in,p+20,4);
        const auto nameSize=Get(in,p+28,2), extra=Get(in,p+30,2), comment=Get(in,p+32,2), offset=Get(in,p+42,4);
        if(method || (flags&~0x800u)) throw std::runtime_error("Use the original ZIP exported by Apex Radiance");
        if(size!=Get(in,p+24,4) || size>kLimit || Get(in,p+34,2) || p+46+nameSize+extra+comment>end) throw std::runtime_error("Invalid profile package");
        std::string name(in.substr(p+46,nameSize));
        if(!SafeName(name) || offset!=localEnd || Get(in,offset,4)!=0x04034b50 || Get(in,offset+6,2)!=flags || Get(in,offset+8,2)!=method || Get(in,offset+14,4)!=crc || Get(in,offset+18,4)!=size || Get(in,offset+22,4)!=size || Get(in,offset+26,2)!=nameSize) throw std::runtime_error("Invalid profile package");
        const size_t data=size_t(offset)+30+nameSize+Get(in,offset+28,2);
        if(data>dirOffset || size>dirOffset-data || in.substr(offset+30,nameSize)!=name) throw std::runtime_error("Invalid profile package");
        std::string bytes(in.substr(data,size));
        if(Crc(bytes)!=crc) throw std::runtime_error("The profile package is damaged");
        for(const auto& e:entries) if(e.name==name) throw std::runtime_error("Duplicate profile package entry");
        entries.push_back({std::move(name),std::move(bytes)}); localEnd=data+size;
        p+=46+nameSize+extra+comment;
    }
    if(p!=end || localEnd!=dirOffset) throw std::runtime_error("Invalid profile package");
    return entries;
}
}
