#include "citystartup.h"
#include "cityrecords.h"
#include "own_spots.h"
#include "trafficspots.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>

namespace citystartup {
namespace {
namespace fs=std::filesystem;
bool Read(const fs::path& path,std::vector<std::uint8_t>& out,bool optional=false){
    std::error_code ec;if(optional&&!fs::exists(path,ec)&&!ec){out.clear();return true;}
    const auto size=fs::file_size(path,ec);if(ec||size>32*1024*1024)return false;
    std::ifstream f(path,std::ios::binary);if(!f)return false;
    out.resize(std::size_t(size));if(!out.empty())f.read(reinterpret_cast<char*>(out.data()),std::streamsize(out.size()));return bool(f);
}
bool Lines(const fs::path& path,std::vector<std::string>& out){
    std::vector<std::uint8_t> bytes;if(!Read(path,bytes,true))return false;
    std::string text(bytes.begin(),bytes.end());std::size_t from=0;
    while(from<text.size()){const auto end=text.find('\n',from);auto line=text.substr(from,end==std::string::npos?end:end-from);if(!line.empty()&&line.back()=='\r')line.pop_back();out.push_back(line);from=end==std::string::npos?text.size():end+1;}return true;
}
bool Stage(const fs::path& path,const std::vector<std::uint8_t>& data){
    {std::ofstream f(path,std::ios::binary|std::ios::trunc);if(!f)return false;if(!data.empty())f.write(reinterpret_cast<const char*>(data.data()),std::streamsize(data.size()));f.flush();if(!f)return false;f.close();if(f.fail())return false;}
    std::vector<std::uint8_t> read;return Read(path,read)&&read==data;
}
bool Commit(const fs::path& staged,const fs::path& target){return MoveFileExW(staged.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;}
bool Reserve(citybuild::Ids& ids,const citybuild::Ids& embedded){
    for(const auto& entries:{embedded.live,embedded.retired})for(const auto& [key,local]:entries){
        const auto live=ids.live.find(key),retired=ids.retired.find(key);
        if((live!=ids.live.end()&&live->second!=local)||(retired!=ids.retired.end()&&retired->second!=local))return false;
        for(const auto& known:{ids.live,ids.retired})for(const auto& [other,id]:known)if(other!=key&&id==local)return false;
        if(live==ids.live.end()&&retired==ids.retired.end())ids.retired[key]=local;
    }
    return ids.valid;
}
void Active(Result& result,const citybuild::Ids& ids,const cityrecords::Inspection& file){
    result.active.clear();for(const auto& entries:{ids.live,ids.retired})for(const auto& [key,local]:entries)if(file.parked.count(local))result.active[key]=local;
}
}
Result RebuildAt(const std::wstring& plugin,const std::wstring& settings){
    using namespace citybuild;Result result;
    const auto fail=[&](const char* why){result.error=why;return result;};
    if(plugin.empty()||settings.empty())return fail("missing destination/settings path");
    const fs::path target(plugin),dir(settings),idsPath=dir/L"citybuild-ids.tsv";
    std::vector<std::uint8_t> old;if(!Read(target,old))return fail("plugin absent or unreadable");
    const auto existing=cityrecords::Inspect(old);if(!existing.valid)return fail("existing plugin failed validation");
    std::vector<std::string> ledger,marks,deleted;
    if(!Lines(idsPath,ledger))return fail("ID registry unreadable");
    auto ids=IdsFrom(ledger);if(!Reserve(ids,existing.ids))return fail("ID registry malformed or conflicts with plugin");
    Active(result,ids,existing);
    if(result.active.size()!=existing.parked.size())return fail("existing parked IDs have no recoverable keys");
    result.ok=true; // Verified old identities remain usable if staging/replacement subsequently fails.
    if(!Lines(dir/L"placement-markers.tsv",marks)||!Lines(dir/L"placement-removed.tsv",deleted))return fail("recordings unreadable; existing plugin retained");
    bool goodMarks=false,goodRemoved=false;
    const auto recorded=Recorded(marks,&goodMarks);const auto removed=RemovedFrom(deleted,&goodRemoved);
    if(!goodMarks||!goodRemoved)return fail("malformed city recording/removal; existing plugin retained");
    std::vector<Spot> baked;std::vector<trafficspots::In> in;
    for(const auto& s:ownspots::kSpots){baked.push_back({s.key,s.x,s.y,s.z,s.heading});in.push_back({s.x,s.y,s.z,s.heading,s.key});}
    ringschedule::Spot homes[ringschedule::kVehicles];trafficspots::Assign(in.data(),int(in.size()),homes);
    const auto all=Merge(baked,recorded,removed);std::vector<Spot> parked;
    for(const auto& s:all){bool home=false;for(const auto& h:homes)home=home||(h.valid&&h.x==s.x&&h.y==s.y&&h.z==s.z);if(!home)parked.push_back(s);}
    if(!Allocate(ids,parked))return fail("parked ID capacity exhausted; existing plugin retained");
    auto refs=Traffic(homes);for(const auto& more:{Parked(parked,ids),Retired(ids),Pool()})refs.insert(refs.end(),more.begin(),more.end());
    const auto bytes=Plugin(refs);const auto candidate=cityrecords::Inspect(bytes);
    if(!candidate.valid)return fail(("generated plugin: "+candidate.error).c_str());
    std::string ledgerText;for(const auto& line:IdsTo(ids))ledgerText+=line+'\n';
    const std::vector<std::uint8_t> ledgerBytes(ledgerText.begin(),ledgerText.end());
    std::error_code ec;fs::create_directories(dir,ec);if(ec)return fail("settings directory not writable");
    const auto idsTmp=fs::path(idsPath.wstring()+L".fvo-new"),pluginTmp=fs::path(target.wstring()+L".fvo-new");
    struct Cleanup{fs::path a,b;~Cleanup(){std::error_code ignored;fs::remove(a,ignored);fs::remove(b,ignored);}} cleanup{idsTmp,pluginTmp};
    if(old!=bytes){
        if(!Stage(pluginTmp,bytes))return fail("plugin staging write failed");
        if(!Stage(dir/L"citybuild-previous.esm",old))return fail("plugin backup failed");
    }
    std::vector<std::uint8_t> currentLedger;if(!Read(idsPath,currentLedger,true))return fail("registry read failed");
    if(currentLedger!=ledgerBytes){if(!Stage(idsTmp,ledgerBytes)||!Commit(idsTmp,idsPath))return fail("ID reservation commit failed");}
    // Reserve first: a failed plugin swap leaves harmless reservations, never recycled save identities.
    std::vector<std::uint8_t> beforeCommit;
    if(!Read(target,beforeCommit)||beforeCommit!=old){result.ok=false;return fail("plugin changed during rebuild; replacement cancelled");}
    if(old!=bytes&&!Commit(pluginTmp,target))return fail("plugin replacement failed; original and backup retained");
    Active(result,ids,candidate);result.changed=old!=bytes;result.parked=parked.size();result.recorded=recorded.size();result.removed=removed.keys.size();return result;
}
}
