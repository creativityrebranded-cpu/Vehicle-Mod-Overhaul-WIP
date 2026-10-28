#include "featureinstaller.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <sstream>

namespace {
struct Feature {const wchar_t* section;const wchar_t* key;};
constexpr Feature features[]={{L"context",L"enabled"},{L"flight",L"physicalVtol"},{L"hud",L"healthBars"}};
bool Set(const std::wstring& file,const wchar_t* section,const wchar_t* key,const wchar_t* value){
    return WritePrivateProfileStringW(section,key,value,file.c_str())!=0;
}
std::string Bytes(const std::wstring& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
int Integration(const char* directory,const char* specification){
    std::array<std::wstring,15> sections,keys,expected;
    Feature actual[15];std::ifstream in(specification);std::string line;unsigned count=0;
    while(std::getline(in,line)){
        if(count>=15)return 1;
        std::istringstream fields(line);std::string section,key,value;
        if(!std::getline(fields,section,'\t')||!std::getline(fields,key,'\t')||!std::getline(fields,value))return 1;
        sections[count]={section.begin(),section.end()};keys[count]={key.begin(),key.end()};expected[count]={value.begin(),value.end()};
        actual[count]={sections[count].c_str(),keys[count].c_str()};++count;
    }
    if(count!=15)return 1;
    const auto profile=std::filesystem::absolute(directory).wstring(),settings=profile+L"\\integration-settings.ini";
    Set(settings,L"custom",L"preserved",L"77");
    const auto result=featureinstaller::Apply(profile,settings,actual);
    if(result.status!=featureinstaller::Status::Applied||result.changed!=15)return 1;
    for(unsigned i=0;i<count;++i)if(featureinstaller::Read(settings,sections[i].c_str(),keys[i].c_str())!=expected[i]){
        std::printf("FAIL native-plan import field %u\n",i);return 1;
    }
    if(featureinstaller::Read(settings,L"custom",L"preserved")!=L"77")return 1;
    std::printf("PASS native FOMOD plan -> production importer: 15 selected settings and unrelated preference preserved\n");return 0;
}
}
int main(int argc,char** argv){
    if(argc==3)return Integration(argv[1],argv[2]);
    using namespace featureinstaller;
    int checks=0,failed=0;
    const auto check=[&](bool ok,const char* label){++checks;if(!ok){++failed;std::printf("FAIL %s\n",label);}};
    const auto root=(std::filesystem::current_path()/std::filesystem::path(L"feature-installer-test-"+std::to_wstring(GetCurrentProcessId()))).wstring();
    const auto profile=root+L"\\profile",settings=root+L"\\settings.ini",mode=profile+L"\\mode.ini",ledger=settings+L".fomod-applied";
    check(CreateDirectoryW(root.c_str(),nullptr)!=0,"create isolated test folder");
    check(CreateDirectoryW(profile.c_str(),nullptr)!=0,"create isolated profile folder");
    check(Apply(profile,settings,features).status==Status::Disabled&&!Exists(settings),"no FOMOD profile leaves legacy startup unchanged");
    Set(settings,L"flight",L"physicalVtol",L"1");Set(settings,L"custom",L"untouched",L"77");
    Set(mode,L"installer",L"enabled",L"0");
    check(Apply(profile,settings,features).status==Status::Disabled&&Read(settings,L"flight",L"physicalVtol")==L"1","keep-existing mode preserves explicit preferences");
    for(const auto& f:features){
        const auto path=profile+L"\\"+Filename(f.section,f.key);
        Set(path,L"feature",L"section",f.section);Set(path,L"feature",L"key",f.key);Set(path,L"feature",L"value",L"0");
    }
    Set(mode,L"installer",L"enabled",L"1");
    auto result=Apply(profile,settings,features);
    check(result.status==Status::Applied&&result.changed==3,"first configured launch imports all selected features");
    check(Read(settings,L"flight",L"physicalVtol")==L"0"&&Read(settings,L"context",L"enabled")==L"0","FOMOD can disable existing enabled features");
    check(Read(settings,L"custom",L"untouched")==L"77","unrelated settings survive import");
    check(Read(settings+L".before-fomod",L"flight",L"physicalVtol")==L"1","original settings backed up before first import");
    Set(settings,L"flight",L"physicalVtol",L"1");Set(settings,L"custom",L"untouched",L"88");
    const auto before=Bytes(settings);
    check(Apply(profile,settings,features).status==Status::Unchanged&&Bytes(settings)==before,"same installer choices preserve later gameplay-menu edits without rewriting");
    const auto health=profile+L"\\"+Filename(L"hud",L"healthBars");
    Set(health,L"feature",L"value",L"1");
    result=Apply(profile,settings,features);
    check(result.status==Status::Applied&&result.changed==1,"changing one installer choice imports only that feature");
    check(Read(settings,L"flight",L"physicalVtol")==L"1"&&Read(settings,L"hud",L"healthBars")==L"1","new selection leaves unchanged feature's gameplay override intact");
    check(Read(settings+L".before-fomod",L"custom",L"untouched")==L"77","subsequent imports retain original backup");
    const auto stable=Bytes(settings),stableLedger=Bytes(ledger);
    Set(health,L"feature",L"value",L"2");
    check(Apply(profile,settings,features).status==Status::Invalid&&Bytes(settings)==stable&&Bytes(ledger)==stableLedger,"malformed value rejects the entire profile without writes");
    Set(health,L"feature",L"value",L"1");Set(health,L"feature",L"section",L"custom");
    check(Apply(profile,settings,features).status==Status::Invalid&&Bytes(settings)==stable,"fragment cannot redirect writes to another INI section");
    Set(health,L"feature",L"section",L"hud");DeleteFileW(health.c_str());
    check(Apply(profile,settings,features).status==Status::Invalid&&Bytes(settings)==stable,"incomplete profile does not partially apply");
    Set(health,L"feature",L"section",L"hud");Set(health,L"feature",L"key",L"healthBars");Set(health,L"feature",L"value",L"0");
    HANDLE held=CreateFileW(settings.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    check(held!=INVALID_HANDLE_VALUE,"hold settings without delete sharing");
    check(Apply(profile,settings,features).status==Status::Failed&&Bytes(ledger)==stableLedger,"failed settings replacement does not commit ledger");
    if(held!=INVALID_HANDLE_VALUE)CloseHandle(held);
    check(Bytes(settings)==stable,"failed atomic replacement leaves settings intact");
    result=Apply(profile,settings,features);
    check(result.status==Status::Applied&&result.changed==1&&Read(settings,L"hud",L"healthBars")==L"0","next launch safely retries a failed import");
    Set(mode,L"installer",L"enabled",L"0");Set(health,L"feature",L"value",L"1");
    check(Apply(profile,settings,features).status==Status::Disabled&&Read(settings,L"hud",L"healthBars")==L"0","keep-existing mode ignores changed feature fragments");
    Set(mode,L"installer",L"enabled",L"broken");
    check(Apply(profile,settings,features).status==Status::Invalid,"invalid setup mode rejected");
    Flush(settings);Flush(ledger);
    for(const auto& file:{settings,ledger,settings+L".before-fomod",settings+L".fomod-new",ledger+L".new"})DeleteFileW(file.c_str());
    Set(mode,L"installer",L"enabled",L"1");
    result=Apply(profile,settings,features);
    check(result.status==Status::Applied&&result.changed==3&&Exists(settings),"fresh install creates a readable settings file");
    // Explicit known files only; no recursive deletion or computed parent cleanup.
    for(const auto& f:features)DeleteFileW((profile+L"\\"+Filename(f.section,f.key)).c_str());
    DeleteFileW(mode.c_str());
    for(const auto& file:{settings,ledger,settings+L".before-fomod",settings+L".fomod-new",ledger+L".new"}){Flush(file);DeleteFileW(file.c_str());}
    RemoveDirectoryW(profile.c_str());RemoveDirectoryW(root.c_str());
    std::printf("Feature installer: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
