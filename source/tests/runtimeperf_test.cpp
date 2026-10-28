#include "addresses.h"
#include "driverdiscovery.h"
#include "bodymass.h"
#include "hookcost.h"
#include "framepacing.h"
#include "updateschedule.h"
#include "inputevents.h"
#include "commandinput.h"
#include <thread>
#include <limits>
#include "controlmap.h"
#include "game.h"
#include "modscene.h"
#include "navseed.h"
#include "navmap.h"
#include "vehicleworld.h"
#include "runtimeforms.h"
#include "runtimecells.h"
#include "pilotquest.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <string>
#include <vector>

namespace {
std::uintptr_t g_base=0;
std::vector<std::uintptr_t> g_refs;
std::unordered_map<std::uint32_t,std::uintptr_t> g_lookup;
unsigned g_scans=0,g_lookups=0,g_completed=0;
std::unordered_map<std::string,std::uintptr_t> g_plugins;
std::uintptr_t g_sourceRef=0;
std::uintptr_t Lookup(std::uint32_t id){++g_lookups;const auto at=g_lookup.find(id);return at==g_lookup.end()?0:at->second;}
std::uintptr_t FindPlugin(std::uintptr_t,const char* name){const auto at=g_plugins.find(name);return at==g_plugins.end()?0:at->second;}
void ConvertId(std::uintptr_t file,std::uint32_t* id){std::uint32_t prefix=0;std::memcpy(&prefix,reinterpret_cast<const void*>(file+0x20),4);*id|=prefix;}
bool ScanVehicle(vehiclelinks::Visitor visit,void* ctx,framework::ScanCursor& cursor){visit(g_sourceRef,ctx);cursor={};return true;}
std::uintptr_t NoNode(std::uintptr_t){return 0;}
std::uintptr_t AsPhysics(std::uintptr_t object){return object;}
std::uint32_t* SceneBodyId(std::uintptr_t object,std::uint32_t* out){std::memcpy(out,reinterpret_cast<const void*>(object+8),4);return out;}
std::uint32_t Owned(){return 0;}
std::uint32_t Driver(std::uint32_t){return 0xABC;}
std::uint32_t EmptyDriver(std::uint32_t){return 0;}
bool Unused(std::uint32_t){return false;}
template<class T> void Put(void* object,std::size_t offset,T value){std::memcpy(static_cast<char*>(object)+offset,&value,sizeof(value));}
bool Commit(std::uint32_t rva,std::size_t size){
    const auto page=g_base+(rva&~0xFFFu);const auto bytes=((rva&0xFFFu)+size+0xFFFu)&~std::size_t(0xFFFu);
    return VirtualAlloc(reinterpret_cast<void*>(page),bytes,MEM_COMMIT,PAGE_READWRITE)!=nullptr;
}
bool Stub(std::uint32_t rva,std::uintptr_t function){
    if(!Commit(rva,32))return false;
    unsigned char jump[14]={0xFF,0x25,0,0,0,0};std::memcpy(jump+6,&function,8);
    auto entry=reinterpret_cast<void*>(g_base+rva);std::memcpy(entry,jump,sizeof(jump));DWORD old=0;
    if(!VirtualProtect(entry,32,PAGE_EXECUTE_READ,&old))return false;
    FlushInstructionCache(GetCurrentProcess(),entry,32);return true;
}
}
namespace game {std::uintptr_t Base(){return g_base;}}
namespace logger {void Write(const char*,...){} bool Verbose(){return false;}}
namespace modscene {std::vector<std::uintptr_t> LoadedRefs(unsigned limit){++g_scans;return {g_refs.begin(),g_refs.begin()+std::min(g_refs.size(),std::size_t(limit))};}}
namespace navmap {void NoteObject(double,double,double,double,double,double,double){} void NoteSeeded(double,double,double){++g_completed;}}
namespace controlmap::testing {void Invalidate();}
namespace navseed::testing {void Clock(ULONGLONG);}
namespace pilotquest::testing {void Clock(ULONGLONG);}

int main(){
    int checks=0,failures=0;auto check=[&](bool ok,const char* name){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",name);}};
    driverdiscovery::Cadence discovery;
    unsigned backgroundScans=0;
    for (unsigned ms=0;ms<10000;++ms) if (discovery.Due(ms)) ++backgroundScans;
    check(backgroundScans==40,"background driver discovery stays at 4 Hz under 1000 updates/s");
    discovery.Reset();
    check(discovery.Due(0)&&!discovery.Due(0)&&!discovery.Due(249)&&discovery.Due(250),"driver discovery starts immediately and respects the refresh boundary");
    check(discovery.Due(2000)&&!discovery.Due(2001),"driver discovery resumes once without catch-up scans after a stall");
    discovery.Reset();
    check(discovery.Due(2001),"save reset immediately rediscovers background drivers");
    check(discovery.Due(100),"clock rollback does not suspend driver discovery");
    using Work=updateschedule::Work;updateschedule::Gate gate;
    check(gate.Poll(0,0)==Work::Full&&gate.Poll(1,0)==Work::Skip,"one bootstrap, no duplicate full update");
    check(gate.Poll(17,0)==Work::Housekeeping,"native quiet period permits input sampling without world work");
    check(gate.Poll(18,1)==Work::Full&&gate.Poll(19,1)==Work::Skip,"native pulse admits a current update once");
    check(gate.Poll(20,101)==Work::Skip&&gate.Poll(25,101)==Work::Full,"burst pulses wait for the admission ceiling and coalesce");
    auto scheduleCounts=gate.TakeCounts();check(scheduleCounts.full==3&&scheduleCounts.coalesced==99,"missed native history is counted without creating catch-up work");
    gate.Reset();for(unsigned i=0;i<500;++i)gate.Poll(i*2.0,(i*60)/500);
    scheduleCounts=gate.TakeCounts();check(scheduleCounts.full==60&&scheduleCounts.skipped==440,"500 polls feeding 60 Hz native input still produce 60 full updates");
    check(gate.Poll(999.5,59)!=Work::Full,"reporting counters does not reopen the consumed native pulse");
    gate.Reset();for(unsigned i=0;i<1000;++i)gate.Poll(i,(i*144)/1000);
    scheduleCounts=gate.TakeCounts();check(scheduleCounts.full==144,"144 Hz native updates are preserved");
    gate.Reset();for(unsigned i=0;i<=30;++i)gate.Poll(i*1000.0/30,i*4);
    scheduleCounts=gate.TakeCounts();check(scheduleCounts.full==31&&scheduleCounts.coalesced==90,"slow consumer coalesces native updates rather than replaying them");
    gate.Reset();for(unsigned i=0;i<15000;++i)gate.Poll(i*2.0,0);
    scheduleCounts=gate.TakeCounts();check(scheduleCounts.full>1500&&scheduleCounts.full<=1801&&scheduleCounts.fallback==scheduleCounts.full-1,"on-foot gameplay remains live through bounded 60 Hz clock fallback");
    check(scheduleCounts.polls==scheduleCounts.full+scheduleCounts.housekeeping+scheduleCounts.skipped,"all raw polls are accounted for with fallback enabled");
    check(gate.Poll(1e9,500)==Work::Full&&gate.Poll(1e9+1,500)==Work::Skip,"long-stall resume has no catch-up burst");
    check(gate.Poll(1,500)==Work::Housekeeping,"clock reversal cannot replay the same native serial");
    gate.Reset();check(gate.Poll(2,500)==Work::Full&&gate.Poll(3,500)==Work::Skip,"load reset permits one bootstrap and restores duplicate protection");
    gate.Reset();check(gate.Poll(std::numeric_limits<double>::quiet_NaN(),0)==Work::Skip&&gate.Poll(std::numeric_limits<double>::infinity(),0)==Work::Skip&&gate.Poll(0,0)==Work::Full,"invalid timestamps do not poison scheduling");
    gate.Reset();for(unsigned i=0;i<10000;++i)gate.Poll(i/10.0,i);
    scheduleCounts=gate.TakeCounts();check(scheduleCounts.full<=241&&scheduleCounts.coalesced>9000,"even a runaway native producer cannot exceed the 240 Hz admission ceiling");
    updateschedule::Pulse pulse;std::thread producer([&]{for(unsigned i=0;i<20000;++i)pulse.Post();});producer.join();
    check(pulse.Serial()==20000,"native pulse publication loses no increments");
    commandinput::Bindings eventBindings{};commandinput::Keys eventKeys{};commandinput::Pads eventPads{};
    commandinput::Frame eventFrame;commandinput::Sample eventSample;
    inputevents::Buffer<commandinput::Count> events;
    const auto hover=std::size_t(bindings::Action::HoverToggle);
    eventBindings[hover]=bindlogic::Parse("V@vehicle");eventSample.context=bindlogic::kCtxGround;eventSample.driving=true;
    auto sample=[&](std::uint64_t now,bool gameplay){eventFrame.Update(eventBindings,eventKeys,eventPads,eventSample,now);
        events.Sample(eventFrame.triggered,eventFrame.enabled,eventSample.context,eventSample.allowed);if(gameplay)events.Publish();};
    sample(0,true);events.End();eventSample.keys['V']=true;sample(17,false);eventSample.keys['V']=false;sample(25,false);sample(33,true);
    check(events.Active(hover),"V press and release during housekeeping survive until the gameplay batch");
    check(events.Active(hover)&&events.Active(hover),"one gameplay batch broadcasts the event to all existing consumers");
    events.End();check(!events.Active(hover),"consumed action is not visible outside its gameplay batch");sample(50,true);
    check(!events.Active(hover),"next gameplay update cannot fire V a second time");
    eventSample.keys['V']=true;sample(67,false);eventSample.allowed=false;sample(70,false);eventSample.keys['V']=false;eventSample.allowed=true;sample(80,true);
    check(!events.Active(hover),"opening a menu or losing input permission cancels queued gameplay events");
    eventSample.keys['V']=true;sample(90,false);eventSample.context=bindlogic::kCtxFoot;eventSample.keys['V']=false;sample(100,true);
    check(!events.Active(hover),"leaving the vehicle scope drops a queued hover toggle");
    eventFrame.Reset();events.Reset();eventSample={};eventSample.context=bindlogic::kCtxGround;eventSample.driving=true;
    eventBindings[hover]=bindlogic::Parse("Pad:DLeft:tap@vehicle");sample(0,true);events.End();eventSample.pad=bindlogic::kDLeft;sample(10,false);eventSample.pad=0;sample(35,false);sample(50,true);
    check(events.Active(hover),"controller tap also survives housekeeping samples until gameplay consumes it");
    events.Reset();sample(60,true);check(!events.Active(hover),"save or binding reset discards old controller events");
    inputevents::Buffer<2> small;std::array<bool,2> flags{true,false},enabled{true,true};
    small.Sample(flags,enabled,1,true);enabled[0]=false;small.Sample({},enabled,1,true);small.Publish();
    check(!small.Active(0),"action eligibility change cancels an event even without a context change");
    bodymass::Cache masses;const std::array<std::uint32_t,2> bodies{0x01000123,0x01000124};
    const auto epoch=masses.Epoch();
    check(masses.Read(bodies,10000)==0,"unobserved chassis falls back without a native query");
    check(masses.Observe(bodies[1],1200,10000,epoch)&&masses.Read(bodies,10001)==1200,"scene candidate list finds current native chassis mass");
    const std::array<std::uint32_t,1> recycled{0x02000124};
    check(masses.Read(recycled,10001)==0,"recycled body index cannot inherit previous generation mass");
    check(masses.Read(bodies,12000)==1200&&masses.Read(bodies,12001)==0&&masses.Read(bodies,9999)==0,"observations expire and reject a reversed clock");
    check(!masses.Observe(0,1200,10000,epoch)&&!masses.Observe(0xFFFFFF,1200,10000,epoch)&&
        !masses.Observe(bodies[0],std::numeric_limits<float>::quiet_NaN(),10000,epoch)&&!masses.Observe(bodies[0],0,10000,epoch),"invalid body identity or mass cannot enter the cache");
    masses.Reset();check(!masses.Read(bodies,10001)&&!masses.Observe(bodies[1],900,10001,epoch),"load epoch invalidates samples and an in-flight callback from the old load");
    check(masses.Observe(bodies[0],1800,10002,masses.Epoch())&&masses.Read(bodies,10003)==1800,"new load can immediately publish its own native mass");
    hookcost::Counter counter;std::atomic<bool> done=false;bool coherent=true;
    std::thread writer([&]{for(unsigned i=0;i<20000;++i){masses.Observe(bodies[0],1800,10002,masses.Epoch());counter.Add(2,3);}done=true;});
    hookcost::Samples totals{};
    while(!done.load()){const auto mass=masses.Read(bodies,10003);if(mass!=0&&mass!=1800)coherent=false;
        hookcost::Samples window;if(counter.Take(window)){totals.count+=window.count;totals.hook+=window.hook;totals.native+=window.native;totals.skipped+=window.skipped;}}
    writer.join();hookcost::Samples tail;counter.Take(tail);totals.count+=tail.count;totals.hook+=tail.hook;totals.native+=tail.native;totals.skipped+=tail.skipped;
    check(coherent&&totals.count+totals.skipped==20000&&totals.hook==2*totals.count&&totals.native==3*totals.count,"concurrent observation/reporting has coherent records and accounts for skipped telemetry");
    counter.Add(8,12);counter.Add(4,6);counter.Take(tail);
    check(tail.count==2&&tail.hook==12&&tail.native==18&&tail.maxHook==8&&tail.maxNative==12,"physics timing separates mod/native duration and preserves worst samples");
    counter.Take(tail);check(!tail.count&&!tail.hook&&!tail.native,"consumed timing window cannot be reported twice");
    hookcost::KeyedCounter<2> stepCounts;stepCounts.Note(0x100);stepCounts.Note(0x100);stepCounts.Note(0x200);stepCounts.Note(0x300);
    unsigned steps=0;const auto skippedSteps=stepCounts.Drain([&](std::uintptr_t key,unsigned count){if(key!=0x100&&key!=0x200)coherent=false;steps+=count;});
    check(coherent&&steps==3&&skippedSteps==1,"bounded physics step counters retain identity and account for a full table without allocation");
    steps=0;stepCounts.Drain([&](std::uintptr_t,unsigned count){steps+=count;});check(steps==0,"drained step counts restart without losing identity slots");
    hookcost::KeyedCounter<> concurrentSteps;done=false;unsigned concurrentTotal=0;
    std::thread stepWriter([&]{for(unsigned i=0;i<20000;++i)concurrentSteps.Note(0x100);done=true;});
    while(!done.load())concurrentSteps.Drain([&](std::uintptr_t,unsigned count){concurrentTotal+=count;});
    stepWriter.join();concurrentSteps.Drain([&](std::uintptr_t,unsigned count){concurrentTotal+=count;});
    check(concurrentTotal==20000,"concurrent step reporting neither loses nor duplicates published increments");
    framepacing::Window pacing;pacing.Begin(1000);pacing.Finish(2);pacing.Begin(1016);pacing.Finish(4);pacing.Begin(1080);
    check(pacing.count==2&&pacing.buckets[0]==1&&pacing.buckets[4]==1&&pacing.worst==64&&pacing.outside==60,"callback spacing separates previous mod work from the following delay");
    pacing.Begin(11001);check(pacing.Ready(),"callback pacing report becomes ready after ten seconds");pacing.Finish(3);pacing.Next();pacing.Begin(11021);
    check(pacing.count==1&&pacing.outside==17&&pacing.buckets[1]==1,"new pacing window retains the last callback/work anchor");
    pacing.Begin(1);check(pacing.count==0&&!pacing.Ready(),"clock reversal restarts the callback pacing window");
    g_base=reinterpret_cast<std::uintptr_t>(VirtualAlloc(nullptr,0x07000000,MEM_RESERVE,PAGE_NOACCESS));
    check(g_base&&Commit(0x061DE668,8)&&Commit(0x04A554E0,78*8)&&Commit(addr::kPlayerCharacterPtr,8)&&Commit(0x005DE3B0,32),"isolated fake engine storage is available");
    if(!g_base||failures)return 1;
    std::array<char,0x300> cm{};std::array<char,0x40> context{};std::array<char,40> key{},pad{};std::array<char,64> event{};
    std::memcpy(event.data()+0x18,"Forward",8);
    Put(key.data(),0,reinterpret_cast<std::uintptr_t>(event.data()));Put(key.data(),8,std::uint32_t(87));Put(key.data(),12,std::uint32_t(0xFF));
    Put(pad.data(),0,reinterpret_cast<std::uintptr_t>(event.data()));Put(pad.data(),8,std::uint32_t(0xB));Put(pad.data(),12,std::uint32_t(0xFF));
    Put(context.data(),0,std::uint32_t(1));Put(context.data(),8,reinterpret_cast<std::uintptr_t>(key.data()));
    Put(context.data(),0x20,std::uint32_t(1));Put(context.data(),0x28,reinterpret_cast<std::uintptr_t>(pad.data()));
    Put(cm.data(),0x10,reinterpret_cast<std::uintptr_t>(context.data()));
    Put(reinterpret_cast<void*>(g_base),0x061DE668,reinterpret_cast<std::uintptr_t>(cm.data()));
    Put(reinterpret_cast<void*>(g_base),0x04A554E0,"Vehicle");
    check(controlmap::KeyFor("Vehicle","Forward",9)==87&&controlmap::PadFor("Vehicle","Forward")==0xB,"production map loads keyboard and pad indexes from native-shaped entries");
    controlmap::testing::Invalidate();
    check(controlmap::KeyFor("Vehicle","Forward",9)==87&&controlmap::PadFor("Vehicle","Forward")==0xB,"unchanged refresh preserves both indexes");
    Put(key.data(),8,std::uint32_t(90));controlmap::testing::Invalidate();
    check(controlmap::KeyFor("Vehicle","Forward",9)==90,"production refresh observes keyboard rebinding");
    Put(pad.data(),8,std::uint32_t(0xC));controlmap::testing::Invalidate();
    check(controlmap::KeyFor("Vehicle","Forward",9)==90&&controlmap::PadFor("Vehicle","Forward")==0xC,"pad-only rebinding is observed while keyboard remains cached");
    Put(key.data(),8,std::uint32_t(0xFF));controlmap::testing::Invalidate();
    check(controlmap::KeyFor("Vehicle","Forward",9)==9,"cleared native keyboard entry keeps the requested fallback");
    Put(reinterpret_cast<void*>(g_base),0x061DE668,std::uintptr_t(0));controlmap::testing::Invalidate();
    check(controlmap::KeyFor("Vehicle","Forward",9)==9&&!controlmap::KeyFree(87),"unavailable native control map fails closed");

    // A local jump stub replaces only this test's fake engine lookup address.
    unsigned char jump[14]={0xFF,0x25,0,0,0,0};const auto target=reinterpret_cast<std::uintptr_t>(&Lookup);std::memcpy(jump+6,&target,8);
    auto entry=reinterpret_cast<void*>(g_base+0x005DE3B0);std::memcpy(entry,jump,sizeof(jump));DWORD previous=0;
    check(VirtualProtect(entry,32,PAGE_EXECUTE_READ,&previous)!=0,"test lookup trampoline is executable");FlushInstructionCache(GetCurrentProcess(),entry,32);
    std::array<char,0xD0> player{};Put(reinterpret_cast<void*>(g_base),addr::kPlayerCharacterPtr,reinterpret_cast<std::uintptr_t>(player.data()));
    std::vector<std::array<char,0xD0>> refs(1000);
    for(unsigned i=0;i<refs.size();++i){Put(refs[i].data(),off::kFormId,std::uint32_t(i+1));Put(refs[i].data(),0x2E,std::uint8_t('K'));const auto ref=reinterpret_cast<std::uintptr_t>(refs[i].data());g_refs.push_back(ref);g_lookup[i+1]=ref;}
    navseed::testing::Clock(10000);navseed::Tick();
    check(g_scans==1&&g_lookups>0&&g_lookups<=300&&g_completed==0,"first navigation slice captures IDs once and respects its work limit");
    g_lookup.erase(600);g_refs.clear(); // unloading after capture must not dereference the old raw list
    for(int frame=0;frame<100&&g_completed==0;++frame)navseed::Tick();
    check(g_completed==1&&g_scans==1&&g_lookups==1000,"all captured IDs are resolved across frames, including an unloaded reference");
    for(int frame=0;frame<20;++frame)navseed::Tick();
    check(g_scans==1&&g_lookups==1000,"idle interval performs no scene enumeration or reference lookup");
    navseed::testing::Clock(12000);navseed::Tick();navseed::Tick();
    check(g_scans==2&&g_completed==2,"next scheduled pass captures current references rather than reusing stale IDs");
    navseed::Reset();
    check(Commit(addr::kDataHandlerPtr,8)&&Stub(addr::kLookupPluginByName,reinterpret_cast<std::uintptr_t>(&FindPlugin))
        &&Stub(addr::kPluginLocalToRuntime,reinterpret_cast<std::uintptr_t>(&ConvertId)),"generic plugin adapter uses isolated native entry points");
    Put(reinterpret_cast<void*>(g_base),addr::kDataHandlerPtr,std::uintptr_t(1));
    std::array<char,0x230> full{},light{},unloaded{};
    Put(full.data(),0x20,std::uint32_t(0x05000000));Put(light.data(),0x20,std::uint32_t(0xFE032000));Put(unloaded.data(),0x218,std::uint8_t(0xFF));
    g_plugins["Full.esm"]=reinterpret_cast<std::uintptr_t>(full.data());g_plugins["Light.esm"]=reinterpret_cast<std::uintptr_t>(light.data());g_plugins["Off.esm"]=reinterpret_cast<std::uintptr_t>(unloaded.data());
    check(runtimeforms::Resolve({"Full.esm",0x800})==0x05000800&&runtimeforms::Resolve({"Light.esm",0x800})==0xFE032800,"same stable key resolves through full and light plugin adapters without consumer bit shifts");
    check(!runtimeforms::Resolve({"Missing.esm",0x800})&&!runtimeforms::Resolve({"Off.esm",0x800})&&!runtimeforms::Resolve({nullptr,0x800}),"missing, unloaded and invalid resource dependencies resolve empty");
    std::array<char,0x160> vanillaQuest{},vanillaPackage{},foreignQuest{},foreignPackage{},namedQuest{};
    Put(vanillaQuest.data(),0x2E,std::uint8_t(0x44));Put(vanillaPackage.data(),0x2E,std::uint8_t(0x55));
    Put(foreignQuest.data(),0x2E,std::uint8_t(0x44));Put(foreignPackage.data(),0x2E,std::uint8_t(0x55));Put(namedQuest.data(),0x2E,std::uint8_t(0x44));
    Put(foreignQuest.data(),0x114,std::uint8_t(1));Put(namedQuest.data(),0x114,std::uint8_t(1));
    g_lookup[0x1D307E]=reinterpret_cast<std::uintptr_t>(vanillaQuest.data());g_lookup[0x1C9CDA]=reinterpret_cast<std::uintptr_t>(vanillaPackage.data());
    g_lookup[0x05000800]=reinterpret_cast<std::uintptr_t>(foreignQuest.data());g_lookup[0x05000801]=reinterpret_cast<std::uintptr_t>(foreignPackage.data());
    pilotquest::testing::Clock(10000);
    check(!pilotquest::Available(),"unrelated plugin with matching quest/package offsets cannot claim the pilot connection");
    g_plugins["FVO_NPCPilots.esm"]=reinterpret_cast<std::uintptr_t>(light.data());
    g_lookup[0xFE032800]=reinterpret_cast<std::uintptr_t>(namedQuest.data());g_lookup[0xFE032801]=reinterpret_cast<std::uintptr_t>(foreignPackage.data());
    pilotquest::testing::Clock(15000);check(pilotquest::Available(),"production pilot quest binds its named dependency at a light-plugin runtime identity");
    Put(namedQuest.data(),0x114,std::uint8_t(0));g_plugins["FVO_NPCPilots.esm"]=reinterpret_cast<std::uintptr_t>(full.data());
    pilotquest::testing::Clock(20000);check(pilotquest::Available(),"pilot connection re-resolves changed load-order identity rather than retaining a stale quest");
    g_plugins.erase("FVO_NPCPilots.esm");pilotquest::testing::Clock(25000);
    check(!pilotquest::Available(),"removed pilot dependency disconnects instead of falling back to an unrelated plugin");
    g_plugins["FVO_NPCPilots.esm"]=reinterpret_cast<std::uintptr_t>(light.data());Put(namedQuest.data(),0x114,std::uint8_t(1));
    pilotquest::Reset();check(pilotquest::Available(),"save reset discards cached quest handles and forces immediate dependency resolution");
    g_lookup[77]=g_lookup[1];check(!runtimeforms::LookupChecked(77)&&runtimeforms::Id(runtimeforms::Lookup(1))==1,"checked lookup rejects mismatched identity");
    check(Commit(addr::kTESPtr,8),"isolated cell adapter storage is available");
    std::array<char,0x400> tes{},entryData{};std::array<char,0x200> cell{};
    std::array<std::uintptr_t,1> entries{reinterpret_cast<std::uintptr_t>(entryData.data())};
    std::array<std::uintptr_t,1> cellRefs{reinterpret_cast<std::uintptr_t>(refs[0].data())};
    Put(reinterpret_cast<void*>(g_base),addr::kTESPtr,reinterpret_cast<std::uintptr_t>(tes.data()));
    Put(tes.data(),off::kTESGridCount,unsigned(1));Put(tes.data(),off::kTESGridCells,reinterpret_cast<std::uintptr_t>(entries.data()));
    Put(entryData.data(),off::kGridEntryState,std::uint32_t(0x30000000));Put(entryData.data(),off::kGridEntryCell,reinterpret_cast<std::uintptr_t>(cell.data()));
    Put(cell.data(),off::kCellState,std::uint8_t(10));Put(refs[0].data(),off::kRefParentCell,reinterpret_cast<std::uintptr_t>(cell.data()));
    Put(cell.data(),0x80,unsigned(1));Put(cell.data(),0x84,unsigned(1));Put(cell.data(),0x88,reinterpret_cast<std::uintptr_t>(cellRefs.data()));
    runtimecells::Reset();std::uintptr_t* view=nullptr;std::uint32_t count=0;
    check(runtimecells::GridSize()==1&&runtimecells::Attached(0)==reinterpret_cast<std::uintptr_t>(cell.data())&&!runtimecells::Attached(1),"cell adapter applies native attached-state and grid bounds");
    Put(entryData.data(),off::kGridEntryState,std::uint32_t(0));
    check(!runtimecells::Attached(0)&&runtimecells::Cell(0,runtimecells::Policy::AttachedCell)==reinterpret_cast<std::uintptr_t>(cell.data()),"consumers explicitly select legacy cell-only or ready-grid eligibility");
    Put(entryData.data(),off::kGridEntryState,std::uint32_t(0x30000000));
    check(runtimecells::References(reinterpret_cast<std::uintptr_t>(cell.data()),view,count)&&view==cellRefs.data()&&count==1,"cell adapter validates the standard reference view");
    cell.fill(0);Put(cell.data(),off::kCellState,std::uint8_t(10));
    Put(cell.data(),0x80,unsigned(1));Put(cell.data(),0x84,unsigned(1));Put(cell.data(),0x88,unsigned(1));Put(cell.data(),0x8C,unsigned(1));Put(cell.data(),0x90,reinterpret_cast<std::uintptr_t>(cellRefs.data()));runtimecells::Reset();
    check(runtimecells::References(reinterpret_cast<std::uintptr_t>(cell.data()),view,count)&&view==cellRefs.data(),"alternate cell view is tried even if the first candidate faults");
    Put(cell.data(),0x8C,unsigned(300000));check(!runtimecells::References(reinterpret_cast<std::uintptr_t>(cell.data()),view,count)&&!view&&!count,"invalid cell capacity returns an empty view");
    Put(cell.data(),off::kCellState,std::uint8_t(9));check(!runtimecells::Attached(0),"unattached cells are not exposed to consumers");

    Put(reinterpret_cast<void*>(g_base),addr::kPlayerCharacterPtr,std::uintptr_t(0));
    Put(cell.data(),off::kCellState,std::uint8_t(10));Put(cell.data(),0x8C,unsigned(1));
    std::array<char,0x80> vehicleBase{};Put(vehicleBase.data(),off::kFormId,std::uint32_t(0x9000));
    float measuredHalf[3]{}, measuredCentre[3]{};
    check(!world::BaseSize(reinterpret_cast<std::uintptr_t>(vehicleBase.data()),measuredHalf,measuredCentre),"zero native bounds report an unmeasured fallback");
    for(unsigned axis=0;axis<3;++axis){Put(vehicleBase.data(),0x50+axis*4,-1.0f);Put(vehicleBase.data(),0x5C+axis*4,1.0f);}
    check(world::BaseSize(reinterpret_cast<std::uintptr_t>(vehicleBase.data()),measuredHalf,measuredCentre)&&measuredHalf[0]==1.0f,"valid native bounds are reported as measured");
    for(unsigned index=0;index<2;++index){Put(refs[index].data(),0x98,reinterpret_cast<std::uintptr_t>(vehicleBase.data()));Put(refs[index].data(),off::kRefParentCell,reinterpret_cast<std::uintptr_t>(cell.data()));}
    g_sourceRef=reinterpret_cast<std::uintptr_t>(refs[0].data());
    world::SetServices({ScanVehicle,Owned,Driver,Unused});world::Publish();world::Snapshot snapshot{};
    check(world::Vehicles(snapshot)&&snapshot.count==1&&snapshot.v[0].formId==1&&snapshot.v[0].driverId==0xABC&&snapshot.v[0].baseBoundsValid,"production world snapshot consumes injected discovery and identifies measured bounds");
    for(unsigned axis=0;axis<3;++axis){Put(vehicleBase.data(),0x50+axis*4,0.0f);Put(vehicleBase.data(),0x5C+axis*4,0.0f);}
    world::Publish();check(world::Vehicles(snapshot)&&!snapshot.v[0].baseBoundsValid,"production snapshot labels default bounds as unmeasured");
    g_sourceRef=reinterpret_cast<std::uintptr_t>(refs[1].data());world::SetServices({ScanVehicle,Owned,EmptyDriver,Unused});
    check(!world::Vehicles(snapshot),"reconnecting providers clears the old snapshot immediately");
    world::Publish();check(world::Vehicles(snapshot)&&snapshot.count==1&&snapshot.v[0].formId==2&&snapshot.v[0].who==world::VehicleInfo::Who::Parked,"replacement provider does not inherit prior source identities or driver state");
    bodymass::observations.Observe(0x01000123,1400,GetTickCount64(),bodymass::observations.Epoch());
    std::array<char,0x160> root{},rootVtable{},collisionVtable{};std::array<char,0x20> loaded{},collision{};
    Put(rootVtable.data(),0x20,reinterpret_cast<std::uintptr_t>(&NoNode));Put(root.data(),0,reinterpret_cast<std::uintptr_t>(rootVtable.data()));
    Put(collisionVtable.data(),0x138,reinterpret_cast<std::uintptr_t>(&AsPhysics));Put(collision.data(),0,reinterpret_cast<std::uintptr_t>(collisionVtable.data()));
    Put(collision.data(),8,bodies[0]);Put(root.data(),0x110,reinterpret_cast<std::uintptr_t>(collision.data()));
    Put(root.data(),0xBC,1.0f);Put(root.data(),0x10C,10.0f);
    for(unsigned axis=0;axis<3;++axis)Put(root.data(),0x80+axis*20,1.0f);
    Put(loaded.data(),off::kLoadedData3D,reinterpret_cast<std::uintptr_t>(root.data()));Put(refs[1].data(),off::kRefLoadedData,reinterpret_cast<std::uintptr_t>(loaded.data()));
    check(Stub(0x02376C70,reinterpret_cast<std::uintptr_t>(&SceneBodyId)),"scene body fixture exposes the native-shaped collision adapter");
    world::Publish();world::Vehicles(snapshot);
    check(snapshot.v[0].bodyId==bodies[0]&&snapshot.v[0].nativeMass&&snapshot.v[0].mass==1400,"production world uses callback mass for a matching live scene chassis without the retired native visitor");
    Put(collision.data(),8,recycled[0]);world::Publish();world::Vehicles(snapshot);
    check(snapshot.v[0].bodyId==recycled[0]&&!snapshot.v[0].nativeMass,"live body generation change falls back immediately instead of retaining prior chassis mass");
    world::Reset();check(bodymass::observations.Read(bodies,GetTickCount64())==0,"world reset drops physics observations from the old load");world::Publish();check(world::Vehicles(snapshot)&&snapshot.count==1&&snapshot.v[0].formId==2,"save reset retains service connections while clearing runtime state");
    world::SetServices({ScanVehicle});world::Publish();check(!world::Vehicles(snapshot),"incomplete service connections fail empty");
    runtimecells::Reset();VirtualFree(reinterpret_cast<void*>(g_base),0,MEM_RELEASE);
    std::printf("runtimeperf: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
