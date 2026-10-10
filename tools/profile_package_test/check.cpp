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
    printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}

