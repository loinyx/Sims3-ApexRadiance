// Real package and profile persistence, isolated from the game's files.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <cstdio>
#include <random>
#include "profile_package.h"
#include "apex_config.h"
#include "apex_log.h"
#include "apex_util.h"
namespace ApexLog { void Write(Level,const std::string&,const std::source_location&) noexcept {} }
std::wstring testRoot;
namespace ApexPaths {
const std::wstring& ApexDirectory() {return testRoot;}
bool EnsureApexDirectory() {std::filesystem::create_directories(testRoot);return true;}
}
int checks=0, failures=0;
void Check(bool ok,const char* what) {++checks;if(!ok){++failures;printf("FAIL %s\n",what);}}
bool Refuses(std::string_view bytes) {try {ProfilePackage::Read(bytes);return false;}catch(const std::exception&){return true;}}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2) return 2;
    testRoot=std::wstring(argv[1])+L"\\run-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"\\";
    ApexPaths::EnsureApexDirectory();
    const std::string lut="TITLE \"Test\"\nLUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
    const std::string preset="[meta]\nprofile='Cinema'\n[qol.picture]\nenabled=true\n[qol.picture.filters]\nlut=true\nlut_file='Film.cube'\nlut_amount=0.75\n";
    const auto zip=ProfilePackage::Write({{"profile.toml",preset},{"LUTs/Film.cube",lut}});
    const auto entries=ProfilePackage::Read(zip);
    Check(entries.size()==2 && entries[0].bytes==preset && entries[1].bytes==lut,"exact round trip");
    Check(ProfilePackage::Crc("123456789")==0xcbf43926,"standard CRC");
    for(size_t n=0;n<zip.size();++n) Check(Refuses(std::string_view(zip).substr(0,n)),"truncated archive refused");
    for(size_t n=0;n<zip.size();++n) {auto corrupt=zip;corrupt[n]^=1;try {auto decoded=ProfilePackage::Read(corrupt);Check(decoded[0].bytes==preset && decoded[1].bytes==lut,"changed payload never accepted");}catch(const std::exception&){++checks;}}
    for(const char* name:{"../bad","/bad","LUTs/../bad","LUTs//bad","C:/bad","LUTs\\bad"}) Check(!ProfilePackage::SafeName(name),"path traversal refused");
    std::mt19937 random(123);
    for(int i=0;i<3000;++i) {
        std::string value(size_t(random()%256),'\0');for(auto& c:value)c=char(random());
        const auto bytes=ProfilePackage::Write({{"profile.toml",value}});
        Check(ProfilePackage::Read(bytes)[0].bytes==value,"binary payload exact");
    }
    std::string error;
    Check(ApexUtil::WriteFileAtomic(testRoot+L"shared.zip",zip,&error),"write package");
    ApexConfig::ProfileImport imported;
    Check(ApexConfig::ReadProfileImport(testRoot+L"shared.zip",imported,&error),"read package through production API");
    Check(ApexConfig::SaveImportedProfile("Cinema",imported,ApexConfig::kPartColor,"bookmark",&error),"import preset plus LUT");
    toml::table loaded;Check(ApexConfig::ReadProfile("Cinema",loaded,&error),"legacy profile reader");
    Check(loaded["qol"]["picture"]["filters"]["lut_amount"].value_or(0.0)==.75,"settings retained");
    Check(ApexConfig::SaveImportedProfile("Cinema 2",imported,ApexConfig::kPartColor,"bookmark",&error),"identical LUT reuse");
    Check(!std::filesystem::exists(testRoot+L"LUTs\\Film-1.cube"),"identical LUT not duplicated");
    auto different=imported;different.lutBytes+="# Different\n";
    Check(ApexConfig::SaveImportedProfile("Cinema 3",different,ApexConfig::kPartColor,"bookmark",&error),"collision import");
    Check(ApexConfig::ReadProfile("Cinema 3",loaded,&error) && loaded["qol"]["picture"]["filters"]["lut_file"].value_or(std::string())=="Film-1.cube","collision rewritten");
    std::string original;ApexUtil::ReadFileBytes(testRoot+L"LUTs\\Film.cube",original);Check(original==lut,"original LUT untouched");
    Check(!ApexConfig::SaveImportedProfile("Cinema",different,ApexConfig::kPartColor,"bookmark",&error),"existing preset protected");
    Check(ApexConfig::ExportProfileFile(testRoot+L"export.zip",loaded,&error),"export automatic bundle");
    ApexConfig::ProfileImport again;Check(ApexConfig::ReadProfileImport(testRoot+L"export.zip",again,&error) && again.lutBytes==different.lutBytes,"export imports exact LUT");
    Check(ApexUtil::WriteFileAtomic(testRoot+L"legacy.toml",preset,&error) && ApexConfig::ReadProfileImport(testRoot+L"legacy.toml",again,&error) && again.lutName.empty(),"old TOML still accepted");
    Check(!ApexConfig::SaveImportedProfile("../bad",imported,ApexConfig::kPartColor,"bookmark",&error),"bad profile name refused");
    Check(ApexConfig::SaveImportedProfile("No Color",imported,ApexConfig::kPartNightLights,"bookmark",&error)==false,"empty selection refused");
    auto rollback=imported;rollback.lutName="Rollback.cube";
    std::filesystem::create_directory(testRoot+L"Profiles\\Rollback.toml");
    Check(!ApexConfig::SaveImportedProfile("Rollback",rollback,ApexConfig::kPartColor,"bookmark",&error),"failed preset write reported");
    Check(!std::filesystem::exists(testRoot+L"LUTs\\Rollback.cube"),"new LUT rolled back on failed preset write");
    auto invalid=ProfilePackage::Write({{"profile.toml",preset},{"LUTs/Other.cube",lut}});
    ApexUtil::WriteFileAtomic(testRoot+L"invalid.zip",invalid,&error);
    Check(!ApexConfig::ReadProfileImport(testRoot+L"invalid.zip",again,&error),"mismatched LUT reference refused");
    auto noLut=toml::parse("[qol.picture]\nenabled=true\n");
    Check(ApexConfig::ExportProfileFile(testRoot+L"plain.toml",noLut,&error),"no-LUT export");
    std::string plain;ApexUtil::ReadFileBytes(testRoot+L"plain.toml",plain);
    Check(!plain.starts_with("PK") && toml::parse(plain)==noLut,"no-LUT export stays TOML");
    // Looks: the look's .ini travels as Looks/<file name>, with the same reuse and numbering rules as LUTs
    std::filesystem::create_directories(testRoot+L"Looks");
    const std::string ini="Techniques=Vibrance@Vibrance.fx\n[Vibrance.fx]\nVibrance=0.300000\n";
    ApexUtil::WriteFileAtomic(testRoot+L"Looks\\Warm.ini",ini,&error);
    auto withLook=toml::parse("[patches.Looks]\nenabled=true\npreset='looks:Warm.ini'\n");
    Check(ApexConfig::ProfileUsesPackage(withLook),"a look file makes a package");
    Check(ApexConfig::ExportProfileFile(testRoot+L"look.zip",withLook,&error),"export look bundle");
    ApexConfig::ProfileImport look;
    Check(ApexConfig::ReadProfileImport(testRoot+L"look.zip",look,&error) && look.lookName=="Warm.ini" && look.lookBytes==ini && look.lutName.empty(),"look bundle reads back");
    Check(ApexConfig::SaveImportedProfile("Warm",look,ApexConfig::kPartColor,"bookmark",&error),"import preset plus look");
    Check(!std::filesystem::exists(testRoot+L"Looks\\Warm-1.ini"),"identical look reused");
    auto otherLook=look;otherLook.lookBytes+="; changed\n";
    Check(ApexConfig::SaveImportedProfile("Warm 2",otherLook,ApexConfig::kPartColor,"bookmark",&error) && ApexConfig::ReadProfile("Warm 2",loaded,&error) &&
          loaded["patches"]["Looks"]["preset"].value_or(std::string())=="looks:Warm-1.ini","look collision rewritten");
    std::string kept;ApexUtil::ReadFileBytes(testRoot+L"Looks\\Warm.ini",kept);Check(kept==ini,"original look untouched");
    const auto badLook=ProfilePackage::Write({{"profile.toml","[patches.Looks]\npreset='looks:Other.ini'\n"},{"Looks/Warm.ini",ini}});
    ApexUtil::WriteFileAtomic(testRoot+L"badlook.zip",badLook,&error);
    Check(!ApexConfig::ReadProfileImport(testRoot+L"badlook.zip",again,&error),"mismatched look reference refused");
    auto builtIn=toml::parse("[patches.Looks]\nenabled=true\npreset='apex:Soft'\n");
    Check(!ApexConfig::ProfileUsesPackage(builtIn),"a built-in look needs no file");
    // Shaders part: [patches.Looks] is its own part; a 3.3.0 development preset (no packs key) still goes with Color
    auto shadersOnly=toml::parse("[qol.picture]\nenabled=true\n[patches.Looks]\nenabled=true\npreset='apex:Soft'\npacks=['qUINT']\n");
    Check(ApexConfig::ProfilePartsOf(shadersOnly)==(ApexConfig::kPartColor|ApexConfig::kPartShaders),"Shaders part found");
    {auto t=shadersOnly;ApexConfig::KeepProfileParts(t,ApexConfig::kPartColor);Check(!t["patches"]["Looks"].as_table() && t.contains("qol"),"Color no longer carries the look");}
    {auto t=shadersOnly;ApexConfig::KeepProfileParts(t,ApexConfig::kPartShaders);Check(t["patches"]["Looks"].as_table() && !t.contains("qol"),"Shaders carries the look alone");}
    Check(ApexConfig::ProfilePacks(shadersOnly)==std::vector<std::string>{"qUINT"},"packs named by the preset");
    auto legacy=toml::parse("[qol.picture]\nenabled=true\n[patches.Looks]\nenabled=true\npreset='apex:Soft'\n");
    Check(ApexConfig::ProfilePartsOf(legacy)&ApexConfig::kPartShaders,"older preset's look shows as Shaders");
    {auto t=legacy;ApexConfig::KeepProfileParts(t,ApexConfig::kPartColor);Check(t["patches"]["Looks"].as_table()!=nullptr,"older preset: Color applies its look");}
    {auto t=legacy;ApexConfig::KeepProfileParts(t,ApexConfig::kPartShaders);Check(t["patches"]["Looks"].as_table() && !t.contains("qol"),"older preset: Shaders applies its look");}
    {auto t=legacy;ApexConfig::KeepProfileParts(t,ApexConfig::kPartNightLights);Check(!t["patches"]["Looks"].as_table(),"older preset: other parts leave the look");}
    // the player's own shader files: the look's .fx, the .fxh it includes and the texture it names, as Shaders/<name>
    std::filesystem::create_directories(testRoot+L"Shaders\\Shaders");std::filesystem::create_directories(testRoot+L"Shaders\\Textures");
    const std::string fx="#include \"ReShade.fxh\"\n#include \"MyLib.fxh\"\ntexture t < source = \"noise.png\"; > { Width = 4; Height = 4; };\ntechnique MyFx { }\n";
    const std::string fxh="float3 Grade(float3 c) { return c; }\n", png="PNG bytes";
    ApexUtil::WriteFileAtomic(testRoot+L"Shaders\\Shaders\\Mine.fx",fx,&error);ApexUtil::WriteFileAtomic(testRoot+L"Shaders\\Shaders\\MyLib.fxh",fxh,&error);
    ApexUtil::WriteFileAtomic(testRoot+L"Shaders\\Textures\\noise.png",png,&error);
    ApexUtil::WriteFileAtomic(testRoot+L"Looks\\Own.ini","Techniques=MyFx@Mine.fx,Vibrance@Vibrance.fx\n",&error);
    auto own=toml::parse("[patches.Looks]\nenabled=true\npreset='looks:Own.ini'\npacks=['qUINT']\n");
    const auto ownFiles=ApexConfig::ProfileShaderFiles(own);
    Check(ownFiles==std::vector<std::string>{"Shaders/Mine.fx","Shaders/MyLib.fxh","Textures/noise.png"},"the look's own files found (packs' and built-in ones left out)");
    Check(ApexConfig::ExportProfileFile(testRoot+L"own-plain.zip",own,&error),"export without own files");
    ApexConfig::ProfileImport ownImport;
    Check(ApexConfig::ReadProfileImport(testRoot+L"own-plain.zip",ownImport,&error) && ownImport.shaderFiles.empty(),"own files stay out unless asked");
    Check(ApexConfig::ExportProfileFile(testRoot+L"own.zip",own,&error,true),"export with own files");
    {std::string zipBytes;ApexUtil::ReadFileBytes(testRoot+L"own.zip",zipBytes);const auto e=ProfilePackage::Read(zipBytes);
     Check(e.size()==5 && e[1].name=="Looks/Own.ini" && e[2].name=="Shaders/Shaders/Mine.fx" && e[3].name=="Shaders/Shaders/MyLib.fxh" && e[4].name=="Shaders/Textures/noise.png" && e[4].bytes==png,"ZIP layout");}
    Check(ApexConfig::ReadProfileImport(testRoot+L"own.zip",ownImport,&error) && ownImport.shaderFiles.size()==3 && ApexConfig::ProfilePacks(ownImport.state)==std::vector<std::string>{"qUINT"},"own files and packs read back");
    Check(ApexConfig::SaveImportedProfile("Own",ownImport,ApexConfig::kPartShaders,"bookmark",&error) && !std::filesystem::exists(testRoot+L"Shaders\\Shaders\\Mine-1.fx"),"identical shader files reused");
    auto changedFx=ownImport;changedFx.shaderFiles[0].bytes+="// changed\n";
    Check(ApexConfig::SaveImportedProfile("Own 2",changedFx,ApexConfig::kPartShaders,"bookmark",&error) && std::filesystem::exists(testRoot+L"Shaders\\Shaders\\Mine-1.fx"),"shader file clash gets a numbered name");
    {std::string kept;ApexUtil::ReadFileBytes(testRoot+L"Shaders\\Shaders\\Mine.fx",kept);Check(kept==fx,"existing shader file never overwritten");}
    std::filesystem::remove_all(testRoot+L"Shaders");
    Check(ApexConfig::SaveImportedProfile("Own 3",ownImport,ApexConfig::kPartShaders,"bookmark",&error) && std::filesystem::exists(testRoot+L"Shaders\\Textures\\noise.png"),"shader files written into the Shaders folder");
    Check(ApexConfig::SaveImportedProfile("Own 4",changedFx,ApexConfig::kPartColor,"bookmark",&error)==false,"no Shaders part, nothing of the look");
    for(const char* bad:{"Shaders/Shaders/../x.fx","Shaders/Other/x.fx","Shaders/Shaders/x.exe","Shaders/Textures/x.fx","Shaders/Shaders/sub/x.fx","Shaders/Shaders/CON.fx"}) {
        try {auto z=ProfilePackage::Write({{"profile.toml","[patches.Looks]\npreset='apex:Soft'\npacks=[]\n"},{bad,"x"}});ApexUtil::WriteFileAtomic(testRoot+L"bad-shader.zip",z,&error);}
        catch(const std::exception&) {++checks;continue;} // a traversal never even makes a package
        Check(!ApexConfig::ReadProfileImport(testRoot+L"bad-shader.zip",again,&error),"unsafe shader entry refused");
    }
    {const auto z=ProfilePackage::Write({{"profile.toml","[qol.picture]\nenabled=true\n"},{"Shaders/Shaders/ok.fx","x"}});ApexUtil::WriteFileAtomic(testRoot+L"orphan.zip",z,&error);
     Check(!ApexConfig::ReadProfileImport(testRoot+L"orphan.zip",again,&error),"shader files without a look refused");}
    {std::vector<ProfilePackage::Entry> many{{"profile.toml","[patches.Looks]\npreset='apex:Soft'\npacks=[]\n"}};
     for(int i=0;i<70;++i) many.push_back({"Shaders/Shaders/f"+std::to_string(i)+".fx","x"});
     try {auto z=ProfilePackage::Write(many);ApexUtil::WriteFileAtomic(testRoot+L"many.zip",z,&error);Check(!ApexConfig::ReadProfileImport(testRoot+L"many.zip",again,&error),"too many shader files refused");}
     catch(const std::exception&) {Check(false,"71 entries fit the package");}}
    auto capture=toml::parse("[capture]\nnotes=true\n");
    Check(ApexConfig::ProfilePartsOf(capture)==ApexConfig::kPartCapture,"capture part found");
    ApexConfig::KeepProfileParts(capture,ApexConfig::kPartColor);Check(!capture.contains("capture"),"capture part dropped unless picked");
    printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}

