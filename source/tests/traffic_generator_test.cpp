#define FVO_GENERATOR_TEST
#include "../src/traffic/generator.cpp"
#include <map>
namespace {std::map<unsigned,FvoTrafficRef> refs;unsigned linked=1;int pose=3,snaps=0,tripOffers=0,starts=0,hidden=0;bool groundKnown=true;int screen=2,visible=2,siteResult=1,births=0,moves=0;
int Presentation(){return screen;}int Exposure(double,double,double){return visible;}int Site(unsigned,double,double,double,double,double* z){*z=0;return siteResult;}
int Form(unsigned){return 1;}int Persist(unsigned,int){return 1;}int Move(unsigned,double,double,double,double){++moves;return 1;}
unsigned Birth(unsigned base,double x,double y,double z,double){++births;unsigned id=100+unsigned(births)*2;refs[id]={id,base,1,0,1,0,0,x,y,z};return id;}
unsigned Driver(unsigned,unsigned vehicle,double x,double y,double z,double,int*){unsigned id=vehicle+1;refs[id]={id,1,1,0,1,1,0,x,y,z};return id;}
int Ref(unsigned id,FvoTrafficRef* out){auto it=refs.find(id);if(it==refs.end())return 0;*out=it->second;return 1;}
unsigned Vehicle(unsigned){return linked;}int Sit(unsigned){return pose;}int SnapSeat(unsigned,unsigned){++snaps;pose=3;return 1;}
int Ground(double,double,double,double* n,double* b){*n=*b=0;return groundKnown?3:0;}
int Uses(unsigned){return 0;}int Offer(int,unsigned,double,double,double,FvoTripClaim* out){++tripOffers;out->step=+[](void*,double,double,double,FvoTripGoal*){return 0;};return 1;}
int Start(unsigned,unsigned,const FvoTripClaim*){++starts;return 1;}int Refusal(unsigned,unsigned,char*,int){return 0;}void Show(unsigned id,int on){if(!on)++hidden;auto it=refs.find(id);if(it!=refs.end()){it->second.usable=on?1:0;it->second.disabled=on?0:1;}}
}
int main(){using namespace generator;int checks=0,failed=0;auto check=[&](bool ok,const char* why){++checks;if(!ok){++failed;std::printf("FAIL %s\n",why);}};
    FvoTrafficApi api{};api.ref=&Ref;api.vehicleOf=&Vehicle;api.sitState=&Sit;api.snapSeat=&SnapSeat;api.ground=&Ground;api.usesVehicle=&Uses;api.startTrip=&Start;api.tripRefusal=&Refusal;api.show=&Show;
    api.presentation=&Presentation;api.exposure=&Exposure;api.birthSite=&Site;api.formExists=&Form;api.makeTraffic=&Persist;api.createVehicle=&Birth;api.createSeatedDriver=&Driver;api.moveTo=&Move;
    wchar_t dir[MAX_PATH]{};GetTempPathW(MAX_PATH,dir);g_testIni=std::wstring(dir)+L"fvo-generator-continuity-test.ini";DeleteFileW(g_testIni.c_str());
    g_api=&api;g_trip=&Offer;refs[1]={};refs[1].id=1;refs[1].usable=refs[1].has3D=1;refs[2]=refs[1];refs[2].id=2;refs[2].isActor=1;
    FvoPoolSpot spot{};Pos player{};Snap snapshot(1);snapshot[0].id=1;snapshot[0].hasBody=1;
    Pair p;p.st=St::Created;p.vehicle=1;p.driver=2;p.at=1;
    auto step=[&](ULONGLONG now){Step(0,p,spot,player,snapshot,now);};
    snapshot[0].hasBody=0;step(100);check(p.st==St::Created&&snaps==0&&tripOffers==0,"no chassis: wait without reseat churn or trip claims");
    snapshot[0].hasBody=1;pose=0;refs[2].x=2.5;step(200);check(p.st==St::Created&&snaps==1,"logical link without pose, driver standing beside the vehicle, is repaired before departure");
    refs[2].x=0;
    refs[2].z=100;step(1300);check(p.st==St::Created&&tripOffers==0,"seated link but airborne detached actor cannot depart");
    check(p.floatingSince==1300&&hidden==0&&moves==0,"a driver a moment away from the vehicle: not disbanded yet, and never moved");
    refs[2].z=0;step(2400);check(p.st==St::Ready,"complete pair advances to ready");
    refs[2].has3D=0;step(4500);check(p.st==St::Created&&tripOffers==0&&starts==0,"driver unload between ready and claim never starts empty traffic");
    refs[1].z=100;step(9500);check(hidden==0,"driver is not disabled/reloaded mid-air");
    refs[1].z=0;groundKnown=false;step(10500);check(hidden==0,"unknown ground cannot authorize driver reload");
    groundKnown=true;step(11500);check(hidden==1&&p.reloadAt==11500,"grounded repair may reload the driver");
    p=Pair{};p.st=St::Ready;p.vehicle=1;p.driver=2;refs[2].has3D=1;refs[2].z=100;
    step(15000);check(p.st==St::Created&&tripOffers==0&&starts==0,"driver detached after ready cannot claim a route or start a trip");
    {
        refs.clear();g_pairs.assign(2,{});g_pairs[0].st=g_pairs[1].st=St::Empty;
        g_spots.assign(2,{250,250,80,0});g_names={"fleet.0","fleet.1"};g_reuse={0,0};g_world="FixtureOnly";g_admission=0;g_createdAt=0;Snap empty;
        screen=1;visible=1;Step(0,g_pairs[0],g_spots[0],player,empty,100000);
        check(births==0&&g_pairs[0].observed,"observable origin is remembered, without a birth");
        visible=2;Step(0,g_pairs[0],g_spots[0],player,empty,103000);
        check(births==0,"turning away never authorizes a previously observable birth");
        g_admission=1;Step(1,g_pairs[1],g_spots[1],player,empty,106000);
        check(births==0,"a different pool slot cannot forget observation of the same origin");
        screen=2;g_admission=0;siteResult=0;Step(0,g_pairs[0],g_spots[0],player,empty,109000);
        check(births==0,"covered map still cannot authorize unknown physical support");
        siteResult=1;Step(0,g_pairs[0],g_spots[0],player,empty,112000);
        check(births==1&&g_pairs[0].birth.z==0&&g_pairs[0].driver,"birth uses validated ground, never airborne authored height");
        g_admission=1;Step(1,g_pairs[1],g_spots[1],player,empty,115000);
        check(births==1,"newly created refs reserve their footprint despite stale vehicle snapshot");
        const auto first=g_pairs[0].vehicle;g_pairs[0].st=St::Started;
        Rush(nullptr,0);RushStep();
        check(moves==0&&births==1&&g_pairs[0].vehicle==first&&g_pairs[0].st==St::Started,"Start/Swap retains physical identities and jobs; no teleport or cold reset");
        FvoPoolSpot next{900,900,0,1};const char* label="fleet.0";SetSpots("FixtureOnly",&next,1,nullptr,&label,&Offer);Reconcile();
        check(g_pairs[0].vehicle==first&&g_pairs[0].st==St::Started&&g_spots[0].x==900,"editing origins updates configuration without discarding a running pair");
        const int beforeHidden=hidden;Fail(0,g_pairs[0],"fixture");check(hidden==beforeHidden,"failure never visibly disables or replaces a pair");
        screen=0;g_admission=0;g_pairs[0]={};g_pairs[0].st=St::Empty;Step(0,g_pairs[0],next,player,empty,120000);
        check(births==1,"load/teardown presentation never permits creation");
        Reset();check(g_pairs.empty()&&g_worldPools.empty()&&!g_rushRequest.load()&&g_observedOrigins.empty(),"save reset clears cached worlds, pending updates and observation state");
    }
    {
        api.endJob=+[](unsigned,const char*){};
        refs.clear();refs[1]={1,10,1,0,1,0,0,0,0,-500};refs[2]={2,1,1,0,1,1,0,0,0,-500};
        g_pairs.assign(1,{});g_pairs[0].vehicle=1;g_pairs[0].driver=2;g_pairs[0].base=10;
        g_wantSpots={{250,250,80,0}};g_repair=-1;g_repairAt=0;Snap empty;
        screen=1;const int before=moves;RepairPoisoned(empty,200000);
        check(moves==before&&g_repair==-1,"below-terrain repairs cannot teleport in gameplay");
        screen=2;RepairPoisoned(empty,203000);siteResult=0;RepairPoisoned(empty,206000);
        check(moves==before,"covered repair still waits for physical support");
        siteResult=1;RepairPoisoned(empty,207000);
        check(moves==before+2&&g_pairs[0].vehicle==1&&g_pairs[0].driver==2&&g_pairs[0].st==St::Created,"covered poisoned repair moves the same pair, without replacement births");
        Reset();
    }
    {
        Reset();refs.clear();g_world="ColdFixture";g_pairs.assign(2,{});for(auto& pair:g_pairs)pair.st=St::Empty;
        g_spots.assign(2,{});g_reuse={-1,-1};g_names={"fleet.0","fleet.1"};
        const FvoPoolSpot scene[]={{100,100,120,0},{100,100,120,0}},alternative{300,100,120,0};
        SetInitialScene(scene,2);SetInitialAlternatives(1,&alternative,1);
        api.airSite=+[](unsigned,double,double,double,double){return siteResult;};
        api.arrivalProgress=+[](int,int){};api.releaseBirth=+[](unsigned){return 1;};
        api.usesVehicle=+[](unsigned){return 1;};
        Snap empty;const int before=births;
        screen=1;visible=1;siteResult=1;InitialScene(empty,player,300000);check(births==before,"initial lane population never appears where the player can see it");visible=2;
        screen=0;InitialScene(empty,player,300010);check(births==before,"cold scene cannot mutate a loading save");
        screen=3;siteResult=0;InitialScene(empty,player,300020);check(births==before,"covered arrival still waits for clear flight envelope");
        siteResult=1;InitialScene(empty,player,300030);
        check(births==before+1&&g_pairs[0].staged&&g_pairs[0].birth.z==120,"initial fleet uses covered authored lane height and stages physics, without waiting for ground origins");
        InitialScene(empty,player,300040);check(births==before+2&&g_initialAttempt[1]==1&&g_pairs[1].birth.x==300,"stale snapshots cannot cause overlapping initial births; the overlapping position moves to its separated alternative");
        InitialScene(empty,player,300050);check(births==before+2&&g_pairs[1].birth.x==300,"covered bootstrap admits a separated alternate without gameplay departure delay");
        const unsigned vehicle=g_pairs[0].vehicle;g_pairs[0].st=St::Started;
        // Confirmed handoff: no physics body and no seated driver -> still held; with both -> released.
        Step(0,g_pairs[0],g_spots[0],player,empty,300060);
        check(g_pairs[0].staged,"a staged chassis without a physics body and a seated driver is not released");
        Snap bodies(1);bodies[0].id=vehicle;bodies[0].hasBody=1;const unsigned linkedBefore=linked;linked=vehicle;
        Step(0,g_pairs[0],g_spots[0],player,bodies,300070);linked=linkedBefore;
        check(!g_pairs[0].staged&&g_pairs[0].vehicle==vehicle,"armed controller releases the same staged chassis once its body exists and its driver sits");
        const int count=births;InitialScene(empty,player,300070);check(births==count,"returning to a populated scene never creates a duplicate fleet");
        Pair restored;LoadIds(1,restored);check(restored.staged&&restored.vehicle==g_pairs[1].vehicle,"unfinished stage survives registry round-trip");
        // After the loading screen: an unfilled route position is completed only while the player cannot see it.
        g_pairs[1]=Pair{};g_pairs[1].st=St::Empty;screen=1;visible=1;const int shown=births;
        InitialScene(empty,player,300080);check(births==shown,"gameplay completion waits while the position is in view");
        visible=2;InitialScene(empty,player,300090);check(births==shown+1&&g_pairs[1].staged,"gameplay completion fills a hidden position, staged");
        Reset();
    }
    {
        // Start/Swap: the editor panel (map, screen 2) stays busy until every route position is filled or deferred.
        Reset();refs.clear();g_world="SwapFixture";g_pairs.assign(3,{});for(auto& pair:g_pairs)pair.st=St::Empty;
        g_spots.assign(3,{});g_reuse={-1,-1,-1};g_names={"fleet.0","fleet.1","fleet.2"};
        const FvoPoolSpot scene[]={{0,0,120,0},{100,0,120,0},{200,0,120,0}};SetInitialScene(scene,3);
        Snap empty;screen=2;siteResult=1;const int before=births;
        Rush(nullptr,0);RushStep();FvoRushState st{};RushStatus(&st);check(st.active==1,"Start/Swap keeps the panel up until positions are filled");
        InitialScene(empty,player,400000);check(births==before+3,"under the panel several separated positions fill in one update");
        RushStatus(&st);check(st.active==0&&st.total==3&&st.made==3,"the panel closes once every position holds a vehicle, with real counts");
        Reset();
    }
    {
        // Pre-armed pilot: a near birth is bound to its pilot and trip in the same call; a far one keeps the ordinary path.
        Reset();refs.clear();g_world="ArmedFixture";g_pairs.assign(2,{});for(auto& pair:g_pairs)pair.st=St::Empty;
        g_spots.assign(2,{});g_reuse={-1,-1};g_names={"fleet.0","fleet.1"};
        const FvoPoolSpot scene[]={{10,0,120,0},{5000,0,120,0}};SetInitialScene(scene,2);
        static int armedCalls=0;armedCalls=0;
        api.trafficPilotFree=+[]{return 1;};
        api.startArmed=+[](unsigned,unsigned,const FvoTripClaim* c){++armedCalls;return c&&c->step?1:0;};
        g_trip=+[](int,unsigned,double,double,double,FvoTripClaim* out){if(out){out->step=+[](void*,double,double,double,FvoTripGoal*){return 0;};out->name="test trip";}return 1;};
        Snap empty;screen=3;siteResult=1;
        InitialScene(empty,player,500000);
        check(armedCalls==1&&g_pairs[0].armed&&g_pairs[0].st==St::Started,"a birth near the player is bound to a pre-armed pilot and its trip at once");
        check(!g_pairs[1].armed&&g_pairs[1].st==St::Created,"a far birth keeps the ordinary path");
        api.startArmed=nullptr;api.trafficPilotFree=nullptr;g_trip=nullptr;
        Reset();
    }
    {
        // Start/Swap repair: a vehicle no pilot holds is put back on its route position, frozen and re-armed; a held one stays.
        Reset();refs.clear();g_world="RepairFixture";g_pairs.assign(2,{});g_spots.assign(2,{});g_reuse={-1,-1};g_names={"fleet.0","fleet.1"};
        const FvoPoolSpot scene[]={{0,0,120,0},{500,0,120,0}};SetInitialScene(scene,2);
        refs[11]={11,1,1,0,1,0,0,30,0,-20000};refs[12]={12,1,1,0,1,1,0,30,0,-20000};   // fell through the world
        refs[13]={13,1,1,0,1,0,0,480,0,118};refs[14]={14,1,1,0,1,1,0,480,0,118};       // flying, its pilot holds it
        g_pairs[0].vehicle=11;g_pairs[0].driver=12;g_pairs[0].st=St::Started;g_pairs[1].vehicle=13;g_pairs[1].driver=14;g_pairs[1].st=St::Started;
        static int holds=0,ends=0;holds=0;ends=0;
        api.pilotHolds=+[](unsigned id){return id==13?1:0;};
        api.holdBirth=+[](unsigned){++holds;};
        api.endJob=+[](unsigned,const char*){++ends;};
        Snap empty;screen=2;const int movesBefore=moves;
        Rush(nullptr,0);RushStep();InitialScene(empty,player,600000);
        check(moves==movesBefore+1&&g_pairs[0].staged&&holds==1&&!g_pairs[1].staged,"Start/Swap re-places the fallen vehicle on its route, frozen, and keeps the held one in place");
        check(g_pairs[0].birth.x==0&&g_pairs[0].frozenAt&&g_pairs[0].rearms==0,"the re-placed vehicle is handed to the pilot recovery at once");
        // Pilot recovery: with a model and no pilot, a fresh pre-armed pilot is requested straight away.
        static int armed=0;armed=0;refs[11].has3D=1;refs[11].z=120;
        api.startArmed=+[](unsigned,unsigned,const FvoTripClaim* c){++armed;return c&&c->step?1:0;};
        api.releaseBirth=+[](unsigned){return 0;};  // release needs a holding pilot
        g_trip=&Offer;
        g_pairs[0].fallCheckAt=600000;Step(0,g_pairs[0],g_spots[0],player,empty,600100);
        check(armed==1&&g_pairs[0].rearms==1,"a frozen vehicle without a pilot gets a fresh pre-armed pilot within the recovery delay");
        api.startArmed=nullptr;api.pilotHolds=nullptr;api.holdBirth=nullptr;api.endJob=nullptr;
        Reset();
    }
    {
        // Randomized models: drawn from whatever the load order holds, the same pick for a position every time, spread over models.
        Reset();g_world="TypesFixture";
        api.vehicleTypes=+[](unsigned* out,int max){const unsigned ids[]={0xFE001033,0xFE004001,0xFE005005,0x0600250F};int n=0;for(unsigned id:ids)if(n<max)out[n++]=id;return n;};
        std::set<unsigned> seen;bool stable=true,inPool=true;
        for(int slot=0;slot<40;++slot){const unsigned a=VehicleBase(slot),b=VehicleBase(slot);stable=stable&&a==b;
            inPool=inPool&&(a==0xFE001033||a==0xFE004001||a==0xFE005005||a==0x0600250F);seen.insert(a);}
        check(stable&&inPool&&seen.size()>=3,"traffic models come from the load order's pool, fixed per position and spread across models");
        {std::set<unsigned> round;for(int slot=4;slot<8;++slot)round.insert(VehicleBase(slot));
         check(round.size()==4,"each round of positions uses every model once (maximum variety, no neighbour repeats)");}
        api.vehicleTypes=nullptr;g_typePool.clear();g_typeWorld.clear();
        check(VehicleBase(3)==0xFE001033,"no pool: the base game's REV-8");
        Reset();
    }
    {
        // Cover pass: a leftover traffic vehicle from an older session is adopted into an empty slot, frozen for a fresh pilot;
        // a slot whose model is not its pick is emptied (hidden) so it is refilled with its position's model.
        Reset();refs.clear();g_world="CoverFixture";g_pairs.assign(2,{});for(auto& pair:g_pairs)pair.st=St::Empty;
        g_spots.assign(2,{});g_reuse={-1,-1};g_names={"fleet.0","fleet.1"};
        const FvoPoolSpot scene[]={{0,0,120,0},{900,0,120,0}};SetInitialScene(scene,2);
        api.vehicleTypes=+[](unsigned* out,int max){if(max<1)return 0;out[0]=0xFE001033;return 1;};
        static int holds=0,shows=0;holds=0;shows=0;
        api.holdBirth=+[](unsigned){++holds;};
        api.show=+[](unsigned,int on){if(!on)++shows;};
        api.usesVehicle=&Uses;  // an earlier fixture left "every vehicle has a job"
        Snap snap(1);snap[0].id=0xFF000100;snap[0].baseId=0xFE001033;snap[0].who=kFvoWhoNpc;snap[0].driver=0xFF000101;snap[0].x=50;snap[0].y=0;snap[0].z=-9000;
        const int movesBefore=moves;g_coverPassDone=false;CoverPass(snap,700000);
        check(!g_pairs[0].vehicle&&shows==2&&holds==0&&moves==movesBefore,"a leftover traffic vehicle from an older session is hidden under cover, never adopted");
        shows=0;
        g_pairs[1].vehicle=0xFF000200;g_pairs[1].driver=0xFF000201;g_pairs[1].base=0xFE004001;g_pairs[1].st=St::Started;
        g_coverPassDone=false;CoverPass(Snap{},700100);
        check(g_pairs[1].st==St::Empty&&!g_pairs[1].vehicle&&shows==2,"a slot whose model is not its pick is emptied under cover for its position's model");
        // Spares: the hidden leftover (model FE001033) fills an empty position needing that model instead of a new creation.
        refs[0xFF000100]={0xFF000100,0xFE001033,1,1,1,0,0,50,0,-9000};refs[0xFF000101]={0xFF000101,1,1,1,1,1,0,50,0,-9000};
        g_spares.clear();g_sparesWorld.clear();
        {const int birthsBefore=births;api.airSite=+[](unsigned,double,double,double,double){return 1;};screen=2;
         InitialScene(Snap{},player,700200);
         check(births==birthsBefore+1&&g_pairs[0].vehicle==0xFF000100&&g_spares.empty(),"a hidden traffic vehicle is reused as a spare for a position needing its model; only the position without a spare gets a new one");}
        api.vehicleTypes=nullptr;api.holdBirth=nullptr;api.show=&Show;g_typePool.clear();g_typeWorld.clear();
        Reset();
    }
    {   // 2026-10-30 09:10 run: a pair with no pilot: vehicle on the ground, driver left 36 m up at the lane.
        api.ref=&Ref;api.vehicleOf=&Vehicle;api.sitState=&Sit;api.snapSeat=&SnapSeat;api.moveTo=&Move;api.show=&Show;api.usesVehicle=&Uses;
        const unsigned kV=0xFF000300,kD=0xFF000301;
        refs[kV]={};refs[kV].id=kV;refs[kV].baseId=0xFE004001;refs[kV].usable=refs[kV].has3D=1;refs[kV].z=-61;
        refs[kD]={};refs[kD].id=kD;refs[kD].isActor=1;refs[kD].usable=refs[kD].has3D=1;refs[kD].z=-25;
        Snap ground(1);ground[0].id=kV;ground[0].hasBody=1;
        Pair stranded;stranded.st=St::Created;stranded.vehicle=kV;stranded.driver=kD;stranded.base=0xFE004001;stranded.at=1;
        FvoPoolSpot at{};Pos me{};
        const int snapsBefore=snaps,movesBefore=moves,hiddenBefore=hidden;
        const int screenBefore=screen;screen=1;visible=1;  // in play, the player can see it
        Step(0,stranded,at,me,ground,1000);Step(0,stranded,at,me,ground,6900);
        check(stranded.st==St::Created&&hidden==hiddenBefore,"away from the vehicle under 6 s: not disbanded");
        check(snaps>=snapsBefore+2&&moves==movesBefore&&stranded.broughtTold,"each frame the core is asked to bring them to it and seat them, never hidden, never the setter");
        Step(0,stranded,at,me,ground,7100);
        check(stranded.st==St::Created&&hidden==hiddenBefore,"6 s and still apart, in view: never vanishes in front of the player");
        visible=2;refs[kV].x=refs[kD].x=500;Step(0,stranded,at,me,ground,8200);refs[kV].x=refs[kD].x=0;
        check(stranded.st==St::Empty&&!stranded.vehicle&&!stranded.driver&&hidden==hiddenBefore+2,"out of sight: the pair is disbanded");
        check(refs[kV].disabled&&refs[kD].disabled,"the vehicle goes with its driver: no empty vehicle is left");
        check(g_spares.size()==1&&g_spares[0].vehicle==kV&&g_spares[0].driver==kD,"both ids stay together on the shelf");
        g_spares.clear();Show(kV,1);Show(kD,1);hidden=hiddenBefore;
        // FF01B0FD: the driver's model is loaded, the vehicle's is not.
        {Pair lone;lone.st=St::Created;lone.vehicle=kV;lone.driver=kD;lone.base=0xFE004001;lone.at=1;refs[kV].has3D=0;
         refs[kV].x=refs[kD].x=500;Step(0,lone,at,me,ground,5000);Step(0,lone,at,me,ground,8100);refs[kV].x=refs[kD].x=0;
         check(lone.st==St::Empty&&refs[kV].disabled&&refs[kD].disabled,"a driver showing with no vehicle model: the pair is disbanded");
         refs[kV].has3D=1;g_spares.clear();Show(kV,1);Show(kD,1);hidden=hiddenBefore;}
        // The driver reaches the vehicle: the wait ends, the seat goes on, nothing is hidden.
        {Pair joined;joined.st=St::Created;joined.vehicle=kV;joined.driver=kD;joined.base=0xFE004001;joined.at=1;
         const int before=hidden;Step(0,joined,at,me,ground,25000);check(joined.floatingSince==25000,"apart: counting");
         refs[kD].z=refs[kV].z;Step(0,joined,at,me,ground,25100);
         check(joined.floatingSince==0&&joined.st!=St::Empty&&hidden==before,"brought to the vehicle: no disband");
         refs[kD].z=-25;}
        // Before the models load: a driver drifted from its vehicle is put back unseen; no look at the ground.
        {const int m0=moves;Pos inside{100,0,0},distant{5000,0,0};refs[kV].has3D=0;refs[kD].has3D=0;refs[kD].z=-25;refs[kV].z=-61;
         auto fresh=[&]{Pair q;q.vehicle=kV;q.driver=kD;q.base=0xFE004001;return q;};
         {Pair q=fresh();PrecheckPair(0,q,distant,10000);check(moves==m0,"past the window: left");}
         {Pair q=fresh();PrecheckPair(0,q,inside,10000);check(moves==m0+1&&q.precheckTold,"36 m apart, no models: the driver is put back");PrecheckPair(0,q,inside,10500);check(moves==m0+1,"every 2 s at most");}
         {const int m1=moves;refs[kD].z=-61;Pair q=fresh();PrecheckPair(0,q,inside,10000);check(moves==m1,"at its vehicle: left");refs[kD].z=-25;}
         {const int m1=moves;refs[kD].has3D=1;Pair q=fresh();PrecheckPair(0,q,inside,10000);check(moves==m1,"a driver with a model can be seen: left");refs[kD].has3D=0;}
         {const int m1=moves;refs[kV].has3D=1;Pair q=fresh();PrecheckPair(0,q,inside,10000);check(moves==m1,"a vehicle with a model can be seen: left");refs[kV].has3D=0;}
         {const int m1=moves;api.usesVehicle=+[](unsigned){return 1;};Pair q=fresh();PrecheckPair(0,q,inside,10000);check(moves==m1,"a pilot's job keeps its own driver");api.usesVehicle=&Uses;}
         // The removal rule: a cover, or each half with no model, or far and out of sight.
         const int sb=screen,vb=visible;screen=1;visible=1;g_playerAt={};refs[kV].has3D=1;refs[kD].has3D=1;refs[kV].x=refs[kD].x=500;
         Pair q=fresh();check(!UnseenPair(q),"500 m away but in view: far alone is not enough");
         visible=2;check(UnseenPair(q),"500 m away and out of the line of sight: may go");
         refs[kV].x=refs[kD].x=100;check(!UnseenPair(q),"100 m away, out of the line of sight: out of sight alone is not enough");
         refs[kV].x=500;refs[kD].x=100;check(!UnseenPair(q),"the driver is near: the pair stays");
         visible=1;refs[kV].x=refs[kD].x=100;refs[kV].has3D=0;refs[kD].has3D=0;check(UnseenPair(q),"no models at all, even near: nothing to see");
         refs[kD].has3D=1;check(!UnseenPair(q),"the driver has a model, near: can be seen");
         refs[kV].has3D=1;screen=3;check(UnseenPair(q),"under a cover: nothing can be seen");
         // Live numbers: far per thing, and behind the camera counts as out of sight.
         {static double farVehicle;farVehicle=410;api.noticeFar=+[](unsigned base){return base?farVehicle:190.0;};api.inCameraView=+[](double,double,double){return 0;};
          screen=1;visible=1;refs[kV].has3D=1;refs[kD].has3D=1;
          refs[kV].x=refs[kD].x=500;check(UnseenPair(q),"past its own far distance, behind the camera, in clear line of sight: out of sight");
          refs[kV].x=300;check(!UnseenPair(q),"the vehicle is inside its own far distance: stays");
          refs[kV].x=500;refs[kD].x=100;check(!UnseenPair(q),"the driver is inside the loaded edge: stays");
          refs[kD].x=500;api.inCameraView=+[](double,double,double){return 1;};check(!UnseenPair(q),"on camera with a clear line of sight: stays");
          visible=2;check(UnseenPair(q),"on camera, line of sight blocked: out of sight");
          api.noticeFar=nullptr;api.inCameraView=nullptr;refs[kV].x=refs[kD].x=0;visible=1;}
         // The record: broken goes without the 10 s wait, only under the rule; a recovering pair is not touched.
         screen=1;visible=2;refs[kV].x=refs[kD].x=500;g_spares.clear();
         {api.pairHealth=+[](unsigned){return int(trafficpair::PairHealth::Broken);};Pair c=fresh();c.st=St::Created;const int hb=hidden;
          check(ShelveBad(0,c,50000)&&hidden==hb+2&&c.st==St::Empty,"broken, far and out of sight: removed as a pair, in any state");
          Show(kV,1);Show(kD,1);g_spares.clear();hidden=hb;
          visible=1;Pair d=fresh();d.st=St::Created;check(!ShelveBad(0,d,50000)&&d.vehicle==kV,"broken but in view: waits");visible=2;
          api.pairHealth=+[](unsigned){return int(trafficpair::PairHealth::Recovering);};Pair e=fresh();e.st=St::Created;check(!ShelveBad(0,e,50000),"recovering: not removed");api.pairHealth=nullptr;}
         refs[kV].x=refs[kD].x=0;screen=sb;visible=vb;refs[kV].z=-61;refs[kV].has3D=1;refs[kD].has3D=1;refs[kD].z=-25;}
        // The player's: traffic lets go, nothing is hidden.
        {static unsigned took;took=kV;api.playerTook=+[](unsigned v){return v==took?1:0;};const int hb=hidden;
         Pair mine;mine.st=St::Started;mine.vehicle=kV;mine.driver=kD;mine.base=0xFE004001;FvoPoolSpot here{};Pos me2{};
         Step(0,mine,here,me2,ground,70000);
         check(mine.st==St::Empty&&!mine.vehicle&&hidden==hb&&!refs[kV].disabled&&g_spares.empty(),"a vehicle the player took: the pair lets go, the vehicle is never hidden or shelved");
         api.playerTook=nullptr;}
        // A driver who got out and stands on the ground is not a broken pair.
        {const unsigned lb=linked;linked=0;refs[kD].z=refs[kV].z;Pair out;out.st=St::Started;out.vehicle=kV;out.driver=kD;out.base=0xFE004001;
         api.pairHealth=+[](unsigned){return int(trafficpair::PairHealth::Healthy);};
         check(PairVerdict(out,900000)==trafficpair::Bad::None,"got out on the ground: not bad");
         api.pairHealth=+[](unsigned){return int(trafficpair::PairHealth::Recovering);};
         check(PairVerdict(out,900000)==trafficpair::Bad::DriverOut,"out but not on the ground: still bad");
         api.pairHealth=nullptr;linked=lb;refs[kD].z=-25;}
        // A SebPod: the seat is 7.9 m from the vehicle's centre. Sitting and aboard: it goes on to Ready, never disbanded.
        {Pair pod;pod.st=St::Created;pod.vehicle=kV;pod.driver=kD;pod.base=0xFE004001;pod.at=1;
         const unsigned wasLinked=linked;const int wasPose=pose;linked=kV;pose=3;refs[kD].z=refs[kV].z+7.9;
         Step(0,pod,at,me,ground,30000);
         check(pod.st==St::Ready&&pod.vehicle==kV&&!refs[kV].disabled,"a driver sitting 7.9 m from a long vehicle's centre is seated");
         linked=wasLinked;pose=wasPose;refs[kD].z=-25;}
        // Frozen, no pilot, the driver's model out of the loaded cells: not restarted. Near: restarted.
        {static int armedCalls;armedCalls=0;Pair wait;wait.st=St::Started;wait.vehicle=kV;wait.driver=kD;wait.base=0xFE004001;wait.staged=true;wait.frozenAt=1;
         api.pilotHolds=+[](unsigned){return 0;};api.startArmed=+[](unsigned,unsigned,const FvoTripClaim*){++armedCalls;return 1;};
         refs[kD].has3D=0;Pos away{500,0,0};
         Step(0,wait,at,away,ground,40000);check(armedCalls==0&&wait.rearms==0&&wait.frozenAt==40000,"far with no driver model: no pilot restart, it just waits");
         Step(0,wait,at,me,ground,44000);check(armedCalls==1&&wait.rearms==1,"near: the pilot restart still runs");
         api.pilotHolds=nullptr;api.startArmed=nullptr;refs[kD].has3D=1;}
        // A pair that came off the shelf and failed again is not offered a third time.
        {Pair again;again.st=St::Created;again.vehicle=kV;again.driver=kD;again.base=0xFE004001;again.at=1;again.fromSpare=true;
         refs[kV].x=refs[kD].x=500;Step(0,again,at,me,ground,9000);Step(0,again,at,me,ground,15100);refs[kV].x=refs[kD].x=0;
         check(again.st==St::Empty&&refs[kV].disabled&&g_spares.empty(),"a reused pair that fails again stays hidden, off the shelf");
         Show(kV,1);Show(kD,1);hidden=hiddenBefore;}
        // A pair given up on.
        {Pair failed;failed.st=St::Failed;failed.vehicle=kV;failed.driver=kD;failed.base=0xFE004001;
         visible=1;Step(0,failed,at,me,ground,20000);
         check(failed.st==St::Failed&&!refs[kV].disabled,"a pair given up on, in view: stays until it cannot be seen");
         visible=2;refs[kV].x=refs[kD].x=500;Step(0,failed,at,me,ground,21100);refs[kV].x=refs[kD].x=0;
         check(failed.st==St::Empty&&refs[kV].disabled&&refs[kD].disabled,"out of sight: disbanded");
         g_spares.clear();Show(kV,1);Show(kD,1);hidden=hiddenBefore;}
        // A spot that keeps failing varies: the next lane alternative.
        {std::vector<FvoPoolSpot> scene(1),alts(2);SetInitialScene(scene.data(),1);SetInitialAlternatives(0,alts.data(),2);
         Pair a;a.vehicle=kV;a.driver=kD;a.base=0xFE004001;
         ShelveBadPair(0,a,trafficpair::Bad::DriverFar,"fixture");check(g_initialAttempt[0]==1,"the first failure moves to the first alternative");
         a=Pair{};a.vehicle=kV;a.driver=kD;a.base=0xFE004001;ShelveBadPair(0,a,trafficpair::Bad::DriverFar,"fixture");check(g_initialAttempt[0]==2,"then the second");
         a=Pair{};a.vehicle=kV;a.driver=kD;a.base=0xFE004001;ShelveBadPair(0,a,trafficpair::Bad::DriverFar,"fixture");check(!g_initialAttempt.count(0),"out of alternatives: back to its own lane spot");
         g_spares.clear();Show(kV,1);Show(kD,1);hidden=hiddenBefore;}
        // A flying pair the game has seated: its reference lag is not a bad pair.
        {Pair flying;flying.st=St::Started;flying.vehicle=kV;flying.driver=kD;flying.base=0xFE004001;
         const unsigned wasLinked=linked;const int wasPose=pose;linked=kV;pose=3;refs[kD].z=-25;refs[kV].z=-61;
         api.pilotHolds=+[](unsigned){return 1;};
         check(PairVerdict(flying,900000)==trafficpair::Bad::None,"36 m behind in flight, sitting, aboard, a pilot holds it: not bad");
         pose=0;check(PairVerdict(flying,900000)==trafficpair::Bad::DriverFar,"the same but not sitting: bad");
         api.pilotHolds=nullptr;linked=wasLinked;pose=wasPose;}
        refs[kD].z=-61;visible=2;screen=screenBefore;
        // The driver watch hid a driver whose pair is on a trip: the pair counts as bad, replaced at the next cover.
        {Pair started;started.st=St::Started;started.vehicle=kV;started.driver=kD;started.base=0xFE004001;refs[kD].disabled=1;refs[kD].usable=0;
         check(PairVerdict(started,800000)==trafficpair::Bad::DriverOut,"a started pair with a hidden driver is bad");
         const unsigned linkedBefore=linked;linked=kV;refs[kD].z=refs[kV].z;
         started.reloadAt=799900;check(PairVerdict(started,800000)!=trafficpair::Bad::DriverOut,"a driver in its model reload is not");
         linked=linkedBefore;refs[kD].disabled=0;refs[kD].usable=1;}
        // A spare whose queued disable has not run yet is not taken.
        {g_spares.clear();g_sparesWorld=g_world;g_spares.push_back({kV,kD,0xFE004001});Spare sp{};
         check(!TakeSpare(0xFE004001,sp)&&g_spares.size()==1,"a spare still enabled is left on the shelf");
         refs[kV].disabled=refs[kD].disabled=1;check(TakeSpare(0xFE004001,sp)&&sp.vehicle==kV&&g_spares.empty(),"a spare really disabled is taken");
         refs[kV].disabled=refs[kD].disabled=0;}
        const int hiddenBeforeGone=hidden;
        // Given up on: the cover pass replaces it, and it goes on the shelf.
        std::vector<FvoPoolSpot> one(1);SetInitialScene(one.data(),1);
        g_pairs.clear();g_pairs.resize(1);auto& gone=g_pairs[0];gone.st=St::Failed;gone.vehicle=kV;gone.driver=kD;gone.base=0xFE004001;
        api.holdBirth=+[](unsigned){};g_coverPassDone=false;CoverPass(Snap{},30000);api.holdBirth=nullptr;
        check(gone.st==St::Empty&&!gone.vehicle&&hidden==hiddenBeforeGone+2,"a pair given up on is hidden and its position emptied under cover");
        check(g_spares.size()==1&&g_spares[0].vehicle==kV,"it is kept as a spare, never deleted");
        Reset();
    }
    DeleteFileW(g_testIni.c_str());
    std::printf("traffic generator: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
