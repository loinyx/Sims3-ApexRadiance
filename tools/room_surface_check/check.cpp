// Native room-surface routing and wall-matrix checks. Credits: @loinyx
#define main RoomMapSuiteMain
#include "../room_map_smooth_check/check.cpp"
#undef main
#include <filesystem>

int main(int argc,char** argv) {
    if(argc<2)return 2;
    std::ifstream f(argv[1],std::ios::binary);
    std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(f),{}};
    std::vector<Blob> blobs;std::vector<Tech> techs;
    Check(LoadPrecomp(bytes,blobs,techs),"precomp readable");
    std::set<uint32_t> floorIds;
    const char* names[]={"InteriorFloor","FloorWithBlacklight","FloorWithStrobelight","FloorWithStrobelightAndBlacklight","InteriorFloorUnlit","FloorTileCeiling"};
    for(const auto& tech:techs)for(const auto* name:names)if(tech.name==Fnv1Lower(name))
        for(const auto& pass:tech.passes)floorIds.insert(pass.first);
    unsigned floors=0,walls=0,outside=0;
    for(size_t i=0;i<blobs.size();++i)if(blobs[i].vs){
        const auto& code=blobs[i].code;
        if(ShaderPatches::IsPackedRoomFloorVs(code)){
            ++floors;if(!floorIds.count(static_cast<uint32_t>(i+1)))++outside;
            ShaderPatches::RoomMapUv uv;Check(ShaderPatches::RoomMapUvScale(code,uv)&&uv.defined,"floor UV resolved");
            auto invalid=code;
            float changed=-127.0f;DWORD changedBits;std::memcpy(&changedBits,&changed,4);
            for(auto& word:invalid)if(word==0xC3000000u)word=changedBits;
            Check(!ShaderPatches::IsPackedRoomFloorVs(invalid),"changed packing rejected");
        }
        ShaderPatches::WallMatrices matrix;
        if(ShaderPatches::FindWallMatrices(code,matrix)){
            ++walls;Check(matrix.world==matrix.clip+4 && matrix.world+2<256,"wall rows bounded");
            Check(!ShaderPatches::IsPackedRoomFloorVs(code),"wall is not a packed floor");
            auto shifted=code,incomplete=code;bool removed=false;
            for(size_t at=1;at<code.size();){
                const DWORD op=code[at]&0xFFFF;if(op==0xFFFF)break;
                const size_t size=op==0xFFFE ? 1+((code[at]>>16)&0x7FFF) : 1+((code[at]>>24)&15);
                if(!size || at+size>code.size())break;
                if(op==9 && size==4){
                    const auto k=code[at+3]&0x7FF;
                    if(k>=static_cast<DWORD>(matrix.clip) && k<=static_cast<DWORD>(matrix.world+2)){
                        shifted[at+3]=(code[at+3]&~0x7FFu)|(k+4);
                        if(!removed){incomplete[at]=(code[at]&~0xFFFFu)|2;removed=true;}
                    }
                }
                at+=size;
            }
            ShaderPatches::WallMatrices shiftedRows;
            Check(ShaderPatches::FindWallMatrices(shifted,shiftedRows)&&shiftedRows.clip==matrix.clip+4&&shiftedRows.world==matrix.world+4,"relocated wall matrix rows");
            Check(!ShaderPatches::FindWallMatrices(incomplete,shiftedRows),"incomplete wall rows rejected");
        }
    }
    unsigned captureFloors=0,captureWalls=0,patched=0;
    Gpu gpu;Check(gpu.dev!=nullptr,"native D3D9 device");
    for(int a=2;a<argc;++a){std::error_code error;
        for(auto it=std::filesystem::recursive_directory_iterator(argv[a],error);!error&&it!=std::filesystem::recursive_directory_iterator();it.increment(error)){
            if(!it->is_regular_file() || it->path().extension()!=".bin")continue;
            std::ifstream input(it->path(),std::ios::binary);std::vector<unsigned char> raw{std::istreambuf_iterator<char>(input),{}};
            if(raw.size()<8 || raw.size()%4)continue;
            std::vector<DWORD> code(raw.size()/4);std::memcpy(code.data(),raw.data(),raw.size());
            if(it->path().filename().string().rfind("VS_",0)==0){
                captureFloors+=ShaderPatches::IsPackedRoomFloorVs(code);
                ShaderPatches::WallMatrices matrix;
                if(ShaderPatches::FindWallMatrices(code,matrix)){
                    ++captureWalls;
                    if(it->path().filename()=="VS_2C223600.bin")Check(matrix.clip==0&&matrix.world==4,"captured wall uses c0/c4");
                }
            }else if(it->path().filename()=="PS_23A750D8.bin"){
                ShaderPatches::RoomMapPatch patch;Check(ShaderPatches::PatchRoomMapSmooth(code,patch),"captured arch floor patch");
                Check(gpu.AcceptsPs(code),"captured arch floor copy creates natively");++patched;
            }
        }
    }
    Check(floors>0 && walls>0,"native room surfaces found");
    Check(outside==0,"packed floors confined to native floor techniques");
    Check(!ShaderPatches::IsPackedRoomFloorVs({}),"empty floor rejected");
    ShaderPatches::WallMatrices matrix;Check(!ShaderPatches::FindWallMatrices({},matrix),"empty wall rejected");
    std::printf("Precomp floors %u, outside floor techniques %u, wall matrices %u; captures floors %u walls %u arch copies %u\n%d checks, %d failures\n",floors,outside,walls,captureFloors,captureWalls,patched,g_checks,g_failures);
    return g_failures?1:0;
}
