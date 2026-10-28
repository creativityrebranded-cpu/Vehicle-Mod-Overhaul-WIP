#include "citystartup.h"
#include "cityrecords.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdio>
namespace fs=std::filesystem;
static auto Bytes(const fs::path& p){std::ifstream f(p,std::ios::binary);return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),{});}
static void Text(const fs::path& p,const std::string& text){std::ofstream f(p,std::ios::binary|std::ios::trunc);f<<text;}
static std::string Mark(const char* key,double x){char b[256];std::snprintf(b,sizeof(b),"%s\tUNRESOLVED\t013BBE\tFF013BBE\tJemisonPlanetWorld1571\t00014CB2\t%.3f\t1500\t-160\t1\t\n",key,x);return b;}
int main(){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try {
    int checks=0,failed=0;const auto check=[&](bool ok,const char* msg){++checks;if(!ok){++failed;std::printf("FAIL %s\n",msg);}};
    const auto temp=fs::absolute(fs::current_path()/"build"/"cityfiles-tests");fs::create_directories(temp);
    const auto root=temp/("fvo-city-test-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    if(!fs::create_directory(root))return 2;const auto settings=root/"settings",plugin=root/"FVO_CityTraffic.esm";fs::create_directory(settings);
    const auto fixture=fs::path(__FILE__).parent_path()/"fixtures/citytraffic-reference.esm";
    fs::copy_file(fixture,plugin);
    auto r=citystartup::RebuildAt(plugin.wstring(),settings.wstring());check(r.ok&&r.error.empty()&&r.changed,"actual startup upgrades legacy traffic-only plugin");
    if(!r.ok||!r.error.empty()){std::printf("startup: %s\n",r.error.c_str());return 1;}
    const auto first=Bytes(plugin);check(cityrecords::Inspect(first).valid,"startup output structurally valid");
    r=citystartup::RebuildAt(plugin.wstring(),settings.wstring());check(r.ok&&r.error.empty()&&!r.changed&&Bytes(plugin)==first,"unchanged startup is idempotent");
    const std::string header="key\tworld_file\tworld_local\tworld_runtime\tworld_editor_id\tcell\tx\ty\tz\tyaw\tlabel\n";
    Text(settings/"placement-markers.tsv",header+Mark("AA",-500));
    r=citystartup::RebuildAt(plugin.wstring(),settings.wstring());check(r.ok&&r.error.empty()&&r.active.count(0xAA),"new recording becomes persistent placement");const auto id=r.active.at(0xAA);
    fs::remove(settings/"citybuild-ids.tsv");
    r=citystartup::RebuildAt(plugin.wstring(),settings.wstring());check(r.ok&&r.error.empty()&&r.active.at(0xAA)==id,"lost sidecar recovers stable IDs from plugin");
    Text(settings/"placement-markers.tsv",header);
    r=citystartup::RebuildAt(plugin.wstring(),settings.wstring());const auto removed=cityrecords::Inspect(Bytes(plugin));
    check(r.ok&&removed.ids.retired.at(0xAA)==id,"erased recording retains disabled tombstone identity");
    Text(settings/"placement-markers.tsv",header+Mark("BB",-510));const auto before=Bytes(plugin);
    HANDLE lock=CreateFileW(plugin.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    check(lock!=INVALID_HANDLE_VALUE,"replacement failure test holds real OS file lock");
    r=citystartup::RebuildAt(plugin.wstring(),settings.wstring());check(r.ok&&!r.error.empty()&&!r.changed&&Bytes(plugin)==before,"failed plugin swap leaves old bytes and identities usable");
    if(lock!=INVALID_HANDLE_VALUE)CloseHandle(lock);
    Text(settings/"placement-markers.tsv",header+Mark("CC",-520));
    r=citystartup::RebuildAt(plugin.wstring(),settings.wstring());
    check(r.ok&&r.error.empty()&&r.active.at(0xCC)!=id,"reservation/tombstones prevent reuse after failed commit");
    const auto stable=Bytes(plugin);Text(settings/"citybuild-ids.tsv","AA 900\nBB 900\n");
    r=citystartup::RebuildAt(plugin.wstring(),settings.wstring());check(!r.ok&&!r.error.empty()&&Bytes(plugin)==stable,"malformed registry cannot rewrite existing plugin");
    check(!fs::exists(plugin.wstring()+L".fvo-new"),"temporary plugin removed after failure");
    // Exact newly-created test root only; never delete a caller-supplied path.
    if(root.parent_path()==temp&&root.filename().string().rfind("fvo-city-test-",0)==0)fs::remove_all(root);
    std::printf("cityfiles: %d checks, %d failures\n",checks,failed);return failed?1:0;
    }catch(const std::exception& error){std::printf("cityfiles exception: %s\n",error.what());return 1;}
}
