#include "citybuild.h"
#include "cityrecords.h"
#include "citypool.h"
#include <filesystem>
#include "trafficspots.h"
#include "own_spots.h"
#include <cstdio>
#include <fstream>
#include <iterator>

namespace {
struct Rec { std::string tag; std::uint32_t flags = 0, fid = 0, base = 0; float d[6] = {}; };
// The references of a FVO_CityTraffic.esm (they are the only REFR / ACHR records in it), and the bytes before the first one.
bool Parse(const std::vector<std::uint8_t>& b, std::vector<Rec>& out, std::size_t& firstRef) {
    firstRef = 0;
    for (std::size_t o = 0; o + 24 <= b.size();) {
        const std::string t(reinterpret_cast<const char*>(&b[o]), 4);
        std::uint32_t size; std::memcpy(&size, &b[o + 4], 4);
        if (t == "GRUP") { o += 24; continue; }
        if (t == "REFR" || t == "ACHR") {
            if (!firstRef) firstRef = o;
            Rec r; r.tag = t; std::memcpy(&r.flags, &b[o + 8], 4); std::memcpy(&r.fid, &b[o + 12], 4);
            std::memcpy(&r.base, &b[o + 24 + 6], 4); std::memcpy(r.d, &b[o + 24 + 10 + 6], 24);
            out.push_back(r);
        }
        o += 24 + size;
    }
    return firstRef != 0;
}
}

int main() {
    int checks=0,failed=0;
    auto check=[&](bool ok,const char* name) { ++checks; if(!ok) { ++failed; std::printf("FAIL %s\n",name); } };
    using namespace citybuild;
    std::vector<trafficspots::In> in; std::vector<Spot> baked;
    for(const auto& s:ownspots::kSpots){in.push_back({s.x,s.y,s.z,s.heading,s.key});baked.push_back({s.key,s.x,s.y,s.z,s.heading});}
    ringschedule::Spot homes[ringschedule::kVehicles];
    trafficspots::Assign(in.data(),int(in.size()),homes);

    // 1. Against the plugin scripts/make_traffic_esm.py built (plugin/build/FVO_CityTraffic.esm): the same file.
    const auto mine=Plugin(Traffic(homes));
    std::ifstream f(std::filesystem::path(__FILE__).parent_path()/"fixtures/citytraffic-reference.esm",std::ios::binary);
    check(bool(f),"mandatory Python reference fixture exists independently of current directory");
    if(f){
        const std::vector<std::uint8_t> py((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
        std::vector<Rec> a,b; std::size_t fa=0,fb=0;
        check(mine.size()==py.size(),"same size as the Python builder's plugin");
        check(Parse(mine,a,fa)&&Parse(py,b,fb)&&fa==fb&&std::equal(mine.begin(),mine.begin()+std::ptrdiff_t(fa),py.begin()),"the header, world and cell records are byte for byte the same");
        bool same=a.size()==b.size();
        for(std::size_t i=0;same&&i<a.size();++i){
            same=a[i].tag==b[i].tag&&a[i].flags==b[i].flags&&a[i].fid==b[i].fid&&a[i].base==b[i].base;
            for(int j=0;j<6;++j)same=same&&std::fabs(a[i].d[j]-b[i].d[j])<0.01f;
        }
        check(same,"the 64 traffic references match (ids, bases, flags, places within 1 cm)");
    }

    // 2. Merging: baked places win, new recordings join, removed ones go.
    std::vector<std::string> marks{"key\tworld_file\tworld_local\tworld_runtime\tworld_editor_id\tplayer_cell_runtime\tx\ty\tz\tyaw\tlabel",
        "00000000000000AA\tUNRESOLVED\t013BBE\tFF013BBE\tJemisonPlanetWorld1571\t00014CB2\t-500.0\t1500.0\t-160.0\t1.0\t",
        "00000000000000BB\tUNRESOLVED\t013BBE\tFF013BBE\tAkilaPlanetWorld99\t00014CB2\t1.0\t2.0\t3.0\t0.0\t"};
    char line[256];std::snprintf(line,sizeof(line),"%016llX\tUNRESOLVED\t013BBE\tFF013BBE\tJemisonPlanetWorld1571\t00014CB2\t0.0\t0.0\t0.0\t0.0\t",(unsigned long long)baked[0].key);
    marks.push_back(line);
    const auto rec=Recorded(marks);
    check(rec.size()==2,"only New Atlantis recordings are read");
    auto all=Merge(baked,rec,{});
    check(all.size()==baked.size()+1,"a new recording joins the baked spots");
    const auto same=std::find_if(all.begin(),all.end(),[&](const Spot& s){return s.key==baked[0].key;});
    check(same!=all.end()&&std::fabs(same->x-baked[0].x)<1e-9,"a baked spot keeps its aligned place over a re-recording");
    std::snprintf(line,sizeof(line),"%016llX\tJemisonPlanetWorld1571\t%.3f\t%.3f\t%.3f",(unsigned long long)baked[1].key,baked[1].x,baked[1].y,baked[1].z);
    const auto removed=RemovedFrom({"key\tworld_editor_id\tx\ty\tz",line});
    all=Merge(baked,rec,removed);
    check(std::none_of(all.begin(),all.end(),[&](const Spot& s){return s.key==baked[1].key;}),"a removed spot is left out");

    // 3. Ids: kept, retired, never reused; a returning spot gets its own id back.
    Ids ids;
    std::vector<Spot> parked(all.begin(),all.begin()+5);
    check(Allocate(ids,parked)&&ids.live.size()==5,"every parked spot gets an id");
    const auto first=ids.live;
    check(Allocate(ids,parked)&&ids.live==first,"a rebuild keeps every id");
    std::vector<Spot> fewer(parked.begin()+1,parked.end());
    Allocate(ids,fewer);
    check(!ids.live.count(parked[0].key)&&ids.retired.count(parked[0].key),"a spot that went retires its id");
    std::vector<Spot> more=fewer;more.push_back(all[6]);
    Allocate(ids,more);
    check(ids.live[all[6].key]!=first.at(parked[0].key),"a retired id is never given to another spot");
    Allocate(ids,parked);
    check(ids.live[parked[0].key]==first.at(parked[0].key),"a spot that comes back gets its own id back");
    Ids round=IdsFrom(IdsTo(ids));
    check(round.live==ids.live&&round.retired==ids.retired,"the id file round-trips");
    Ids full;std::vector<Spot> many;for(std::uint64_t k=1;k<=kLastSpot-kFirstSpot+2;++k)many.push_back({k,0,0,0,0});
    check(!Allocate(full,many),"running out of ids is reported");

    // 4. The plugin with parked spots and the pool.
    Ids pids; std::vector<Spot> p2(all.begin(),all.begin()+3); Allocate(pids,p2);
    auto refs=Traffic(homes); auto pk=Parked(p2,pids); auto pool=Pool();
    refs.insert(refs.end(),pk.begin(),pk.end()); refs.insert(refs.end(),pool.begin(),pool.end());
    std::vector<Rec> r; std::size_t fr=0;
    check(Parse(Plugin(refs),r,fr)&&r.size()==64+3+kPool,"traffic, parked spots and the pool are all written");
    bool poolOk=true;for(const auto& x:r)if((x.fid&0xFFF)>=kFirstPool)poolOk=poolOk&&x.flags==(kPersistent|kInitiallyDisabled);
    check(poolOk,"the pool is persistent and initially disabled");
    bool persistent=true;for(const auto& x:r)persistent=persistent&&(x.flags&kPersistent);
    check(persistent,"every reference is persistent");
    double cx,cy,cz;ToCity(-858.7,1567.4,153.5,cx,cy,cz);
    check(std::fabs(cx-(-158.7))<1e-9&&std::fabs(cy-(-132.6))<1e-9&&std::fabs(cz-392.47)<1e-9,"the city frame is the planet frame shifted -7, +17 cells and -238.97 m");

    const auto trafficInspection=cityrecords::Inspect(mine);if(!trafficInspection.valid)std::printf("inspection: %s\n",trafficInspection.error.c_str());
    check(trafficInspection.valid,"production parser validates Python-compatible traffic output");
    auto bad=mine;bad.resize(31);check(!cityrecords::Inspect(bad).valid,"truncated input rejected");
    bad=mine;bad[4]=0xFF;bad[5]=0xFF;bad[6]=0xFF;bad[7]=0x7F;check(!cityrecords::Inspect(bad).valid,"oversized record rejected");
    auto duplicate=Traffic(homes);duplicate.push_back(duplicate.front());check(!cityrecords::Inspect(Plugin(duplicate)).valid,"duplicate reference IDs rejected");
    auto tombstones=Retired(ids);auto identityRefs=Traffic(homes);auto placed=Parked(parked,ids);identityRefs.insert(identityRefs.end(),placed.begin(),placed.end());identityRefs.insert(identityRefs.end(),tombstones.begin(),tombstones.end());
    const auto inspected=cityrecords::Inspect(Plugin(identityRefs));check(inspected.valid&&inspected.ids.live==ids.live&&inspected.ids.retired==ids.retired,"plugin embeds recoverable live and retired keys");
    check(!IdsFrom({"AA 900","BB 900"}).valid,"two keys cannot share a local ID");
    check(!IdsFrom({"AA 900","AA 901 retired"}).valid,"a key cannot own two IDs");
    bool valid=true;auto badMarks=marks;badMarks[1].replace(badMarks[1].find("-500.0"),6,"oops");Recorded(badMarks,&valid);check(!valid,"malformed numeric recording rejected instead of converted to zero");
    check(RemovedFrom({"header","AA\tAkilaPlanetWorld99\t0\t0\t0"}).keys.empty(),"other-world removals cannot hide city spots");
    std::uint64_t key=0;check(!Key("-1",key)&&!Key("FFFFFFFFFFFFFFFFF",key),"signed and overflowing keys rejected");
    std::array<bool,kPool> busy{};citypool::Assignments poolIds{{100,4},{200,8}};
    auto planned=citypool::Plan(poolIds,{{50,0,0,0,0},{100,0,0,0,0},{200,0,0,0,0}},busy);
    check(planned.at(100)==4&&planned.at(200)==8,"inserting a lower-key spot never shifts existing pool slots");
    busy[4]=true;planned=citypool::Plan(poolIds,{{50,0,0,0,0},{200,0,0,0,0}},busy);
    check(planned.at(100)==4&&planned.at(50)!=4,"busy removed pool identity cannot be reused");
    busy.fill(true);planned=citypool::Plan({},{{50,0,0,0,0}},busy);check(planned.empty(),"pool exhaustion defers without stealing occupied vehicles");
    bool goodPool=false;check(citypool::Read({"AA 4","BB 4"},goodPool).size()==1&&!goodPool,"duplicate pool slot registry rejected");

    std::printf("citybuild: %d checks, %d failures\n",checks,failed);
    return failed?1:0;
}
