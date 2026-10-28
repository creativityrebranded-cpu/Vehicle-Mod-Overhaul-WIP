#include "npccensus.h"
#include "npcactivity.h"
#include "crowdprofiles.h"
#include "addresses.h"
#include "game.h"
#include "inputgate.h"
#include "log.h"
#include "npcvehicles.h"
#include "runtimecells.h"
#include "runtimeforms.h"
#include <Windows.h>
#include <algorithm>
#include <array>

namespace npccensus {
namespace {
using uptr=std::uintptr_t;
template<class T>T At(uptr p,unsigned offset){return *reinterpret_cast<const T*>(p+offset);}
npcactivity::Census<> census;
unsigned grid=0,cursor=0;
uptr cellAnchor=0;
ULONGLONG next=0,reportAt=0,nextRow=0;
unsigned row=0,rowsLeft=0,capacityDrops=0;
double worstMs=0;
std::array<std::uint32_t,std::size(crowdprofiles::kNewAtlantis)> profileIds{};
unsigned profilesResolved=0;
bool observing=false;

const char* Profile(std::uint32_t base) {
    for(unsigned i=0;i<profilesResolved;++i) {
        if(profileIds[i]&&profileIds[i]==base)return crowdprofiles::kNewAtlantis[i].name;
    }
    return nullptr;
}
bool Read(uptr ref,uptr player,std::uint64_t now,npcactivity::Observation& o) {
    __try {
        if(!ref||ref==player||At<std::uint8_t>(ref,0x2E)!='K')return false;
        o.actor=At<std::uint32_t>(ref,off::kFormId);o.at=now;
        const auto base=At<uptr>(ref,0x98),cell=At<uptr>(ref,off::kRefParentCell);
        if(!o.actor||!base||!cell)return false;
        o.base=runtimeforms::Id(base);o.cell=runtimeforms::Id(cell);
        const auto p=At<game::Vec3>(ref,off::kRefPosition),pp=At<game::Vec3>(player,off::kRefPosition);
        if(!std::isfinite(pp.x)||!std::isfinite(pp.y)||!std::isfinite(pp.z)||
            std::hypot(std::hypot(double(p.x)-pp.x,double(p.y)-pp.y),double(p.z)-pp.z)>200)return false;
        o.x=p.x;o.y=p.y;o.z=p.z;
        if(At<std::uint32_t>(ref,0x20)&0x820)return true;
        const auto data=At<uptr>(ref,off::kRefLoadedData);
        if(!data||!At<uptr>(data,off::kLoadedData3D))return true;
        // Existing validated package getter; no path query, package evaluation,
        // alias assignment, process enumeration, or actor mutation.
        const auto package=npcvehicles::CurrentPackage(ref);
        if(package){o.package=runtimeforms::Id(package);o.packageType=At<std::uint8_t>(package,0x3C);}
        o.crowd=Profile(o.base)!=nullptr;
        o.free=npcvehicles::ActorFree(ref)&&!npcvehicles::IsInCombat(ref);
        o.readable=true;return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){o.readable=false;return o.actor!=0;}
}
bool RefAt(uptr cell,unsigned index,uptr& ref) {
    __try {
        uptr* refs=nullptr;std::uint32_t count=0;
        if(!runtimecells::References(cell,refs,count)||index>=count)return false;
        ref=refs[index];return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){ref=0;return false;}
}
uptr Player() {
    __try {return At<uptr>(game::Base(),addr::kPlayerCharacterPtr);}
    __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
void Report(std::uint64_t now) {
    if(now>=reportAt) {
        unsigned current=0,crowd=0,low=0,unknown=0;
        for(const auto& e:census.Entries())if(e.Current(now)) {
            ++current;if(e.last.crowd)++crowd;if(e.status==npcactivity::Status::Low)++low;
            if(e.status==npcactivity::Status::Unknown)++unknown;
        }
        logger::Write("NPC census: %u recently observed / %u recognized New Atlantis crowd / %u low-activity review / %u unknown; %u capacity drops; slice worst %.3f ms (not a complete population count; quest/service eligibility unverified; no NPC changes)",current,crowd,low,unknown,capacityDrops,worstMs);
        reportAt=now+30000;row=0;rowsLeft=8;capacityDrops=0;worstMs=0;
    }
    if(!rowsLeft||now<nextRow)return;
    // One row per second through the existing buffered log. Prioritize low
    // activity, then other recognized crowd, then unclassified base identities.
    for(;row<census.Entries().size()*3;++row) {
        const auto& e=census.Entries()[row%census.Entries().size()];
        if(!e.Current(now))continue;
        const bool low=e.status==npcactivity::Status::Low;
        const unsigned group=unsigned(row/census.Entries().size());
        if((group==0&&(!low||!e.last.crowd))||(group==1&&(low||!e.last.crowd))||(group==2&&e.last.crowd))continue;
        const auto* name=Profile(e.last.base);if(!name)name="unclassified-template";
        logger::Write("NPC census row: actor %08X base %08X %s cell %08X package %08X type %d; %s; %u samples / %.1f s observed / %.1f s quiet / %.1f m moved / %u package changes; replacement eligibility UNVERIFIED",
            e.last.actor,e.last.base,name,e.last.cell,e.last.package,e.last.packageType,npcactivity::Name(e.status),e.samples,
            double(now-e.began)/1000,double(now-e.quiet)/1000,e.metres,e.changes);
        ++row;--rowsLeft;nextRow=now+1000;return;
    }
    rowsLeft=0;
}
}
void Tick() {
    if(!logger::Verbose()){if(observing)Reset();return;}
    observing=true;
    const auto now=GetTickCount64();if(now<next)return;next=now+50;
    if(!inputgate::GameplayAllowed()){census.Reset();grid=cursor=0;cellAnchor=0;return;}
    const auto player=Player();if(!player)return;
    LARGE_INTEGER start{},stamp{},frequency{};QueryPerformanceCounter(&start);QueryPerformanceFrequency(&frequency);
    // At most one named-form resolution per slice, cached until load reset.
    if(profilesResolved<profileIds.size()) {
        profileIds[profilesResolved]=runtimeforms::Resolve({"Starfield.esm",crowdprofiles::kNewAtlantis[profilesResolved].local});
        ++profilesResolved;
    }
    unsigned actors=0,refsSeen=0,hops=0;const auto cells=runtimecells::GridSize();
    while(cells&&actors<1&&refsSeen<128&&hops<8) {
        grid%=cells;const auto cell=runtimecells::Attached(grid);
        if(cell!=cellAnchor){cellAnchor=cell;cursor=0;}
        uptr ref=0;
        if(!cell||!RefAt(cell,cursor,ref)){grid=(grid+1)%cells;cellAnchor=0;cursor=0;++hops;continue;}
        ++cursor;++refsSeen;
        npcactivity::Observation sample;
        if(Read(ref,player,now,sample)){++actors;if(!census.Observe(sample))++capacityDrops;}
        QueryPerformanceCounter(&stamp);
        if(double(stamp.QuadPart-start.QuadPart)*1000/double(frequency.QuadPart)>=0.2)break;
    }
    QueryPerformanceCounter(&stamp);
    worstMs=std::max(worstMs,double(stamp.QuadPart-start.QuadPart)*1000/double(frequency.QuadPart));
    Report(now);
}
void Reset() {
    census.Reset();grid=cursor=row=rowsLeft=capacityDrops=0;cellAnchor=0;
    next=reportAt=nextRow=0;worstMs=0;
    profileIds={};profilesResolved=0;
    observing=false;
}
}
