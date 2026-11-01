#define FVO_GENERATOR_TEST
#include "../src/traffic/generator.cpp"
#include <map>
namespace {std::map<unsigned,FvoTrafficRef> refs;unsigned linked=1;int pose=3,snaps=0,tripOffers=0,starts=0,hidden=0;bool groundKnown=true;int screen=2,visible=2,siteResult=1,births=0,moves=0;
int Presentation(){return screen;}int Exposure(double,double,double){return visible;}int Site(unsigned,double,double,double,double,double* z){*z=0;return siteResult;}
int Form(unsigned){return 1;}int Persist(unsigned,int){return 1;}int Move(unsigned,double,double,double,double){++moves;return 1;}
bool allowBirth=true;
unsigned Birth(unsigned base,double x,double y,double z,double){if(!allowBirth)return 0;++births;unsigned id=100+unsigned(births)*2;refs[id]={id,base,1,0,1,0,0,x,y,z};return id;}
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
    api.provingSpot=+[](unsigned,int slot,double* x,double* y,double* z,double* h){const auto at=trafficpair::ProveAt(slot,0);*x=at.x;*y=at.y;*z=at.z;*h=0;return 1;};
    static int sends=0,deletes=0,holds=0;
    api.holdDriver=+[](unsigned,unsigned){++holds;};
    api.sendPair=+[](unsigned,unsigned,double,double,double,double){++sends;return 1;};
    api.deleteRef=+[](unsigned){++deletes;return 1;};
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
        check(births==0&&!g_pairs[0].vehicle,"no vehicle waiting at a landing spot near the player: nothing enters traffic");
        // A landing spot's waiting vehicle: a driver is called to it only while the spot cannot be seen, and the pair must pass its check.
        refs[50]={50,0xFE001033,1,0,1,0,0,20,0,0};
        {Snap waiting(1);waiting[0].id=50;waiting[0].hasBody=1;Landing landing;landing.vehicle=50;landing.base=0xFE001033;landing.key="place.a";landing.loaded=true;g_landings={landing};
         screen=1;visible=1;g_createdAt=0;Step(0,g_pairs[0],g_spots[0],player,waiting,113000);
         check(!g_pairs[0].vehicle&&g_landings[0].vehicle==50,"the spot is in view: no driver is called");
         refs[50].x=500;refs[50].has3D=0;visible=2;Step(0,g_pairs[0],g_spots[0],player,waiting,114000);refs[50].has3D=1;
         check(!g_pairs[0].vehicle,"an unloaded landing vehicle cannot receive a driver");
         Step(0,g_pairs[0],g_spots[0],player,waiting,115000);
         check(g_pairs[0].vehicle==50&&g_pairs[0].driver==51&&g_pairs[0].proving&&!g_pairs[0].staged&&births==0&&!g_landings[0].vehicle,"loaded landing vehicle beyond 170 m gets a driver when concealed, without a distance cap");
         screen=2;}
        const auto first=g_pairs[0].vehicle;g_pairs[0].st=St::Started;
        Rush(nullptr,0);RushStep();
        check(moves==0&&births==0&&g_pairs[0].vehicle==first&&g_pairs[0].st==St::Started,"asking for a reset changes nothing until a cover hides it");
        g_rushClear=false;g_rushFill=false;
        FvoPoolSpot next{900,900,0,1};const char* label="fleet.0";SetSpots("FixtureOnly",&next,1,nullptr,&label,&Offer);Reconcile();
        check(g_pairs[0].vehicle==first&&g_pairs[0].st==St::Started&&g_spots[0].x==900,"editing origins updates configuration without discarding a running pair");
        const int beforeHidden=hidden;Fail(0,g_pairs[0],"fixture");check(hidden==beforeHidden,"failure never visibly disables or replaces a pair");
        screen=0;g_admission=0;g_pairs[0]={};g_pairs[0].st=St::Empty;Step(0,g_pairs[0],next,player,empty,120000);
        check(births==0,"load/teardown presentation never permits creation");
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
        siteResult=1;RepairPoisoned(empty,207000);FinishBirth(0,g_pairs[0],207001);
        check(moves==before+2&&g_pairs[0].vehicle==1&&g_pairs[0].driver==2&&g_pairs[0].st==St::Created,"covered poisoned repair moves the same pair, without replacement births");
        Reset();
    }
    {
        Reset();refs.clear();g_world="ColdFixture";g_pairs.assign(2,{});for(auto& pair:g_pairs)pair.st=St::Empty;
        g_spots.assign(2,{});g_reuse={-1,-1};g_names={"fleet.0","fleet.1"};
        const FvoPoolSpot scene[]={{100,100,120,0},{100,100,120,0}},alternative{-100,100,120,0};
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
        InitialScene(empty,player,300040);check(births==before+1&&g_initialAttempt[1]==1&&!g_pairs[1].vehicle,"stale snapshots cannot cause overlapping initial births; retry is deferred to the next update");
        InitialScene(empty,player,300050);check(births==before+2&&g_pairs[1].birth.x==-100,"covered bootstrap admits a separated local alternate on the next update");
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
        visible=2;InitialScene(empty,player,300090);check(births==shown&&!g_pairs[1].vehicle,"in play a lost position is never filled on its lane, in view or not");
        Reset();
    }
    {
        // Start/Swap: the editor panel (map, screen 2) stays busy until every route position is filled or deferred.
        Reset();refs.clear();g_world="SwapFixture";g_pairs.assign(3,{});for(auto& pair:g_pairs)pair.st=St::Empty;
        g_spots.assign(3,{});g_reuse={-1,-1,-1};g_names={"fleet.0","fleet.1","fleet.2"};
        const FvoPoolSpot scene[]={{0,0,120,0},{100,0,120,0},{200,0,120,0}};SetInitialScene(scene,3);
        Snap empty;screen=2;siteResult=1;const int before=births;
        Rush(nullptr,0);RushStep();FvoRushState st{};RushStatus(&st);check(st.active==1,"Start/Swap keeps the panel up until positions are filled");
        InitialScene(empty,player,399990); // reset submits retirement, admission resumes on a later update
        InitialScene(empty,player,400000);check(births==before+1,"covered admission is limited to one pair per update");
        InitialScene(empty,player,400010);InitialScene(empty,player,400020);
        RushStatus(&st);check(st.active==0&&st.total==3&&st.made==3&&st.skipped==0,"the panel fills the whole rule target, including a position beyond 170 m");
        Reset();
    }
    {
        // Every new pair is made over the player for its check, near or far, with no pilot until it passes; birth is where it goes afterwards.
        Reset();refs.clear();g_world="ArmedFixture";g_pairs.assign(2,{});for(auto& pair:g_pairs)pair.st=St::Empty;
        g_spots.assign(2,{});g_reuse={-1,-1};g_names={"fleet.0","fleet.1"};
        const FvoPoolSpot scene[]={{10,0,120,0},{5000,0,120,0}};SetInitialScene(scene,2);
        static int armedCalls=0;armedCalls=0;
        api.trafficPilotFree=+[]{return 1;};
        api.startArmed=+[](unsigned,unsigned,const FvoTripClaim* c){++armedCalls;return c&&c->step?1:0;};
        g_trip=+[](int,unsigned,double,double,double,FvoTripClaim* out){if(out){out->step=+[](void*,double,double,double,FvoTripGoal*){return 0;};out->name="test trip";}return 1;};
        Snap empty;screen=3;siteResult=1;
        InitialScene(empty,player,500000);
        check(armedCalls==0&&!g_pairs[0].armed&&g_pairs[0].st==St::Created&&g_pairs[0].proving,"no pair starts with a pilot: it is checked first");
        InitialScene(empty,player,500010);
        check(g_pairs[1].vehicle&&!g_initialRejected.count(1)&&refs[g_pairs[1].vehicle].x==5000,"remote authored route positions are admitted without a distance rejection");
        check(refs[g_pairs[0].vehicle].x==10&&refs[g_pairs[0].vehicle].z==120,"the local pair is created at its actual lane origin, without an overhead substitute");
        // A checked pair keeps its route claim and flies out without native
        // relocation: both unseen and covered destinations caused lock waits.
        {Pair q;q.st=St::Ready;q.vehicle=70;q.driver=71;q.base=0xFE001033;q.proving=q.staged=true;q.proveSlot=0;q.proveSince=q.proveTick=600000;q.birth={5000,0,120,0};
         refs[70]={70,0xFE001033,1,0,1,0,0,0,0,500};refs[71]={71,1,1,0,1,1,0,0,0,500};linked=70;pose=3;
         api.usesVehicle=&Uses;g_trip=&Offer;Snap body(1);body[0].id=70;body[0].hasBody=1;FvoPoolSpot none{};const Pos me{};const int startsBefore=starts;screen=1;
         Step(0,q,none,me,body,600500);check(q.st==St::Ready&&starts==startsBefore,"seated, but not yet for the hold: no trip");
         Step(0,q,none,me,body,601500);check(q.st==St::Ready&&starts==startsBefore,"still inside the hold");
         Step(0,q,none,me,body,602600);
         check(q.st==St::Started&&starts==startsBefore+1&&!q.proving&&q.sending,"it held its seat: it takes its route claim");
         const int sendsBefore=sends;q.watchAt=610000;
         const auto releaseBefore=api.releaseBirth;api.releaseBirth=+[](unsigned){return 0;};
         Step(0,q,none,me,body,605000);check(q.staged&&q.sending&&sends==sendsBefore,"no dispatch before physics handoff");
         q.staged=false;visible=2;Step(0,q,none,me,body,605100);
         check(!q.sending&&q.st==St::Started&&sends==sendsBefore&&starts==startsBefore+1,"unseen route: existing pilot flies without teleport or a second claim");
         q.sending=true;screen=2;Step(0,q,none,me,body,605200);
         check(!q.sending&&sends==sendsBefore&&q.vehicle==70&&q.driver==71,"map cover also cannot teleport the checked pair");
         screen=1;visible=1;q.sending=true;Step(0,q,none,me,body,605300);
         check(!q.sending&&sends==sendsBefore,"a visible destination uses the same flight path");api.releaseBirth=releaseBefore;}
        // The check failed: deleted, and it never joined traffic.
        // At a landing spot, in view: it waits, and goes only once it cannot be seen.
        {Pair q;q.st=St::Failed;q.vehicle=74;q.driver=75;q.base=0xFE001033;q.proving=true;q.proveSince=q.proveTick=690000;q.at=690000;
         refs[74]={74,0xFE001033,1,0,1,0,0,20,0,0};refs[75]={75,1,1,0,1,1,0,20,0,0};
         Snap none;FvoPoolSpot here{};const Pos me{};
         Step(0,q,here,me,none,690500);check(q.vehicle==74&&!refs[74].disabled,"a failed pair at a landing spot in view does not vanish");
         refs[74].x=refs[75].x=5000;refs[74].has3D=refs[75].has3D=0;Step(0,q,here,me,none,691000);
         check(q.st==St::Empty&&refs[74].disabled&&refs[75].disabled,"once it cannot be seen it is deleted");g_doomed.clear();}
        {Pair q;q.st=St::Created;q.vehicle=72;q.driver=73;q.base=0xFE001033;q.proving=q.staged=true;q.proveSlot=1;q.proveSince=q.proveTick=700000;
         refs[72]={72,0xFE001033,1,0,1,0,0,0,0,500};refs[73]={73,1,1,0,1,1,0,0,0,500};linked=72;pose=0;
         g_names={"fleet.0","fleet.1"};Snap none;FvoPoolSpot here{};const Pos me{};const int startsBefore=starts,deletesBefore=deletes;g_doomed.clear();
         ULONGLONG t=700000;for(;t<700000+trafficpair::kProveLimitMs+1500;t+=500)Step(0,q,here,me,none,t);
         check(q.st==St::Empty&&!q.vehicle&&starts==startsBefore&&refs[72].disabled&&refs[73].disabled,"past the limit it is hidden at once, with no trip");
         DeleteTick(t+100);check(deletes==deletesBefore,"not deleted before the hide has landed");
         DeleteTick(t+2500);check(deletes==deletesBefore&&g_doomed.size()==2,"native deletion waits until both models have retired");
         refs[72].has3D=refs[73].has3D=0;DeleteTick(t+2600);DeleteTick(t+2700);
         check(g_doomed.empty()&&deletes==deletesBefore+2,"model teardown precedes one native delete per reference");
         check(g_spares.empty(),"an unhealthy pair is not kept for reuse");}
        // Seated and waiting for route room: not a failed check, however long the route takes. A seat that breaks later gets the whole limit, then goes.
        {Pair q;q.st=St::Ready;q.vehicle=76;q.driver=77;q.base=0xFE001033;q.proving=q.staged=true;q.proveSlot=2;q.proveSince=q.proveTick=800000;
         refs[76]={76,0xFE001033,1,0,1,0,0,0,0,500};refs[77]={77,1,1,0,1,1,0,0,0,500};linked=76;pose=3;
         const auto tripBefore=g_trip;g_trip=[](int,unsigned,double,double,double,FvoTripClaim*)->int{return 0;};
         Snap body(1);body[0].id=76;body[0].hasBody=1;FvoPoolSpot here{};const Pos me{};const int startsBefore=starts;g_doomed.clear();
         ULONGLONG t=800000;for(;t<800000+trafficpair::kProveLimitMs*3;t+=500)Step(0,q,here,me,body,t);
         check(q.vehicle==76&&q.proving&&starts==startsBefore&&!refs[76].disabled,"a seated pair waiting for route room outlives the limit");
         check(q.routeWaitTold&&trafficpair::ProveHeld(q.seatedSince,t),"it says it waits on a route, past its seat hold");
         linked=0;const ULONGLONG broke=t;
         Step(0,q,here,me,body,t+=500);check(q.vehicle==76,"a broken seat is not deleted on the spot");
         for(;t<broke+trafficpair::kProveLimitMs+1500;t+=500)Step(0,q,here,me,body,t);
         check(!q.vehicle&&refs[76].disabled,"a seat that stays broken past the limit goes");
         g_trip=tripBefore;g_doomed.clear();}
        // A refused delete backs off, and a stuck one does not use up the turns of the ones behind it.
        {g_doomed.clear();g_retiring.clear();const auto deleteBefore=api.deleteRef;const auto pendingBefore=api.mutationPending;
         static int refusedCalls;refusedCalls=0;static int doneCalls;doneCalls=0;
         api.deleteRef=+[](unsigned id)->int{if(id==82){++doneCalls;return 1;}++refusedCalls;return 0;};
         api.mutationPending=+[](unsigned,const char*)->int{return 0;};
         for(unsigned id:{78u,79u,80u})refs[id]={id,0xFE001033,0,1,0,0,0,0,0,0};
         g_clock=900000;DeletePair(78,0);
         for(ULONGLONG t=902000;t<=919000;t+=500)DeleteTick(t);
         check(refusedCalls==3&&g_doomed.size()==1,"a refused delete is asked at 2, 7 and 17 s, not every update");
         g_doomed.clear();g_retiring.clear();refusedCalls=0;
         refs[82]={82,0xFE001033,0,1,0,0,0,0,0,0};
         g_clock=920000;DeletePair(78,0);DeletePair(79,0);DeletePair(80,0);DeletePair(82,0);
         DeleteTick(922000);DeleteTick(922100);
         check(doneCalls==1&&refusedCalls==3,"the delete behind refused ones still gets its turn");
         g_doomed.clear();g_retiring.clear();api.deleteRef=deleteBefore;api.mutationPending=pendingBefore;}
        // A failed lookup is not the end of a retirement. What comes back is hidden only if it is still what was retired.
        {g_doomed.clear();g_retiring.clear();const auto bv=0xFE001033u;
         refs[84]={84,bv,1,0,1,0,0,0,0,0};g_clock=1000000;DeletePair(84,0);refs.erase(84);
         DeleteTick(1002000);DeleteTick(1062000);check(g_doomed.size()==1&&Retiring(84),"a failed lookup keeps the vehicle on the list");
         refs[84]={84,bv,1,0,1,0,0,0,0,0};DeleteTick(1092000);
         check(refs[84].disabled&&g_doomed.size()==1,"when it comes back it is hidden at once");
         g_doomed.clear();g_retiring.clear();
         refs[86]={86,bv,1,0,1,0,0,0,0,0};g_clock=1100000;DeletePair(86,0);refs.erase(86);DeleteTick(1102000);
         refs[86]={86,0x01234567,1,0,1,0,0,0,0,0};DeleteTick(1103000);
         check(!refs[86].disabled&&g_doomed.empty()&&!Retiring(86),"an id reused by another base is left alone");
         refs.erase(85);g_clock=1200000;DeletePair(0,85);DeleteTick(1202000);
         refs[85]={85,1,1,0,1,1,0,0,0,0};DeleteTick(1203000);
         check(!refs[85].disabled&&g_doomed.empty()&&!Retiring(85),"an actor whose base was never read is left alone");
         refs.erase(87);g_clock=1300000;DeletePair(87,0);DeleteTick(1302000);DeleteTick(1302000+kDoomedMissingMs);
         check(g_doomed.empty(),"a reference that never comes back is given up on");
         g_doomed.clear();g_retiring.clear();}
        screen=3;
        api.startArmed=nullptr;api.trafficPilotFree=nullptr;g_trip=nullptr;
        Reset();
    }
    {
        // Reset traffic (X): every pair is cleared out, healthy or not, with its job let go; the player's vehicle stays.
        Reset();refs.clear();g_world="RepairFixture";g_pairs.assign(2,{});g_spots.assign(2,{});g_reuse={-1,-1};g_names={"fleet.0","fleet.1"};
        const FvoPoolSpot scene[]={{0,0,120,0},{500,0,120,0}};SetInitialScene(scene,2);
        refs[11]={11,1,1,0,1,0,0,30,0,-20000};refs[12]={12,1,1,0,1,1,0,30,0,-20000};   // fell through the world
        refs[13]={13,1,1,0,1,0,0,480,0,118};refs[14]={14,1,1,0,1,1,0,480,0,118};       // flying, its pilot holds it
        g_pairs[0].vehicle=11;g_pairs[0].driver=12;g_pairs[0].st=St::Started;g_pairs[1].vehicle=13;g_pairs[1].driver=14;g_pairs[1].st=St::Started;
        static int holds=0,cleared=0;holds=0;cleared=0;
        api.pilotHolds=+[](unsigned id){return id==13?1:0;};
        api.holdBirth=+[](unsigned){++holds;};
        api.endJob=+[](unsigned,const char*){};
        SetClearedHook(+[](){++cleared;});
        Snap empty;screen=1;const int hiddenBefore=hidden,birthsBefore=births;
        api.show=+[](unsigned,int){++hidden;};  // as in game: the disable is queued, it hasn't landed yet
        Rush(nullptr,0);RushStep();InitialScene(empty,player,600000);
        check(g_pairs[1].vehicle==13&&hidden==hiddenBefore&&!cleared,"in gameplay a reset waits for its cover: nothing vanishes in view");
        screen=2;InitialScene(empty,player,600100);
        check(!g_pairs[0].vehicle&&!g_pairs[1].vehicle&&g_pairs[0].st==St::Empty&&g_pairs[1].st==St::Empty,"a reset clears every pair: the fallen one and the healthy one");
        check(hidden==hiddenBefore+4&&cleared==1,"each vehicle and driver is hidden, never moved or left behind, and the trips are let go");
        check(g_spares.size()==2,"cleared pairs are kept to be used again, so the save does not grow");
        check(births==birthsBefore&&g_clearUntil!=0,"the fill waits for the hiding to land");
        api.show=+[](unsigned id,int on){refs[id].disabled=on?0:1;refs[id].usable=on?1:0;};
        for(const unsigned id:{11u,12u,13u,14u})refs[id].disabled=1;
        InitialScene(empty,player,600200);
        check(g_clearUntil==0,"once hidden, the positions are filled");
        api.show=&Show;SetClearedHook(nullptr);
        // Traffic from a save that no registry knows: hidden as a pair, whoever it is seated in or not.
        {refs[900]={};refs[900].id=900;refs[900].baseId=0xFE001033;refs[900].usable=refs[900].has3D=1;refs[900].z=-13;
         refs[901]={};refs[901].id=901;refs[901].isActor=1;refs[901].usable=refs[901].has3D=1;refs[901].z=72;
         refs[902]=refs[900];refs[902].id=902;refs[903]=refs[901];refs[903].id=903;
         api.show=+[](unsigned id,int on){refs[id].disabled=on?0:1;refs[id].usable=on?1:0;};
         api.linkedPairs=+[](unsigned* d,unsigned* v,int){d[0]=901;v[0]=900;d[1]=903;v[1]=902;return 2;};
         g_spares.clear();g_spares.push_back({902,903,0xFE001033});
         screen=2;SweepOrphans(900000);
         check(refs[900].disabled&&refs[901].disabled,"under a cover: a pair no registry knows is hidden as one");
         check(g_spares.size()==2&&g_spares.back().vehicle==900,"it is kept as a spare, so the save does not grow");
         check(!refs[902].disabled&&!refs[903].disabled,"a pair already known is left alone");
         refs[900].disabled=refs[901].disabled=0;refs[900].usable=refs[901].usable=1;g_spares.pop_back();
         screen=0;SweepOrphans(910000);
         check(!refs[900].disabled&&!refs[901].disabled,"with no way to know it is unseen, nothing vanishes");
         api.linkedPairs=nullptr;api.show=&Show;g_spares.clear();screen=2;}
        // Pilot recovery: with a model and no pilot, a fresh pre-armed pilot is requested straight away.
        g_pairs.assign(2,{});g_pairs[0].vehicle=11;g_pairs[0].driver=12;g_pairs[0].st=St::Started;g_pairs[0].staged=true;g_pairs[0].frozenAt=597000;
        refs[11].disabled=0;refs[12].disabled=0;refs[11].usable=1;refs[12].usable=1;
        static int armed=0;armed=0;refs[11].has3D=1;refs[11].z=120;
        const auto driverWas=refs[12];refs[12].x=refs[11].x;refs[12].y=refs[11].y;refs[12].z=120;  // at its seat: a restart can seat it
        api.startArmed=+[](unsigned,unsigned,const FvoTripClaim* c){++armed;return c&&c->step?1:0;};
        api.releaseBirth=+[](unsigned){return 0;};  // release needs a holding pilot
        g_trip=&Offer;
        g_pairs[0].fallCheckAt=600000;Step(0,g_pairs[0],g_spots[0],player,empty,600100);
        check(armed==1&&g_pairs[0].rearms==1,"a frozen vehicle without a pilot gets a fresh pre-armed pilot within the recovery delay");
        // Its driver out of reach of the seat (16:55 run, FF10E519 thrown about 1,000 km): never armed; straight to the set-down, the pair check retires it.
        {armed=0;g_pairs[1]=g_pairs[0];g_pairs[1].rearms=0;g_pairs[1].rearmAt=0;g_pairs[1].frozenAt=597000;refs[12].z=120+60;
         Step(0,g_pairs[1],g_spots[0],player,empty,600100);
         check(armed==0&&g_pairs[1].rearms==3,"driver 60 m from its seat: no pilot armed on the empty vehicle, no further restarts");}
        refs[12]=driverWas;
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
        {std::vector<std::vector<unsigned>> rules(40);const unsigned was7=VehicleBase(7),was8=VehicleBase(8);rules[8]={was8};
         SetModelRules(rules);
         check(VehicleBase(8)!=was8&&VehicleBase(8)!=0,"a position never holds a model its route leaves out");
         check(VehicleBase(7)==was7,"a position with no rule keeps the model it always had");
         rules[8].clear();for(int s=0;s<40;++s)rules[8].push_back(VehicleBase(s));
         {std::vector<std::vector<unsigned>> all(40);all[8]=rules[8];SetModelRules(all);}
         check(VehicleBase(8)==0,"every model left out: no vehicle at that position");
         SetModelRules({});check(VehicleBase(8)==was8,"rules lifted: back to its own model");}
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
        {const int birthsBefore=births;api.airSite=+[](unsigned,double,double,double,double){return 1;};screen=3;
         InitialScene(Snap{},player,700200);
         check(births==birthsBefore&&g_pairs[0].vehicle==0xFF000100&&g_pairs[0].relocating==1&&g_spares.empty(),"a hidden spare reserves its local position and waits for relocation before enabling");
         FinishBirth(0,g_pairs[0],700201);FinishBirth(0,g_pairs[0],700202);
         check(g_pairs[0].relocating==3&&births==birthsBefore,"both moves finish without a second vehicle; queued enables still await acknowledgement");
         Show(g_pairs[0].vehicle,1);Show(g_pairs[0].driver,1);FinishBirth(0,g_pairs[0],700203);
         check(!g_pairs[0].relocating,"observed enables complete spare relocation");}
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
        check(g_spares.empty()&&g_doomed.size()==2,"an unhealthy pair is deleted, both of it, not shelved");
        g_doomed.clear();g_spares.clear();Show(kV,1);Show(kD,1);hidden=hiddenBefore;
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
        {const int m0=holds,moved=moves;Pos inside{100,0,0},distant{5000,0,0};refs[kV].has3D=0;refs[kD].has3D=0;refs[kD].z=-25;refs[kV].z=-61;
         auto fresh=[&]{Pair q;q.vehicle=kV;q.driver=kD;q.base=0xFE004001;return q;};
         {Pair q=fresh();PrecheckPair(0,q,distant,10000);check(holds==m0,"past the window: left");}
         {Pair q=fresh();PrecheckPair(0,q,inside,10000);check(holds==m0+1&&moves==moved&&q.precheckTold,"36 m apart, no models: the driver is put back, never through the mover (it would release the seat)");PrecheckPair(0,q,inside,10500);check(holds==m0+1,"every 2 s at most");}
         {const int m1=holds;refs[kD].z=-61;Pair q=fresh();PrecheckPair(0,q,inside,10000);check(holds==m1,"at its vehicle: left");refs[kD].z=-25;}
         {const int m1=holds;refs[kD].has3D=1;Pair q=fresh();PrecheckPair(0,q,inside,10000);check(holds==m1,"a driver with a model can be seen: left");refs[kD].has3D=0;}
         {const int m1=holds;refs[kV].has3D=1;Pair q=fresh();PrecheckPair(0,q,inside,10000);check(holds==m1,"a vehicle with a model can be seen: left");refs[kV].has3D=0;}
         {const int m1=holds;api.usesVehicle=+[](unsigned){return 1;};Pair q=fresh();PrecheckPair(0,q,inside,10000);check(holds==m1,"a pilot's job keeps its own driver");api.usesVehicle=&Uses;}
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
         const auto kDWas=refs[kD];refs[kD].x=refs[kV].x;refs[kD].y=refs[kV].y;refs[kD].z=refs[kV].z;  // near and at its seat
         Step(0,wait,at,me,ground,44000);check(armedCalls==1&&wait.rearms==1,"near: the pilot restart still runs");
         refs[kD]=kDWas;
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
        { // Independent lifetime: prior cases deliberately retire the same fake ids; reset that fixture metadata here.
         g_retiring.clear();g_doomed.clear();
         g_spares.clear();g_sparesWorld=g_world;g_spares.push_back({kV,kD,0xFE004001});Spare sp{};
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
    {
        // A requested fleet larger than twelve, spread across a world. Distant pairs can await model loading without
        // occupying the active-check throttle or shrinking the target. Temporary retirement pressure must not skip slots.
        Reset();refs.clear();g_world="PopulationTargetFixture";
        constexpr int target=80;
        g_pairs.assign(target,{});for(auto& pair:g_pairs)pair.st=St::Empty;
        g_spots.assign(target,{});g_reuse.assign(target,-1);g_names.clear();
        std::vector<FvoPoolSpot> scene;
        for(int i=0;i<target;++i){scene.push_back({1000.0+40.0*i,0,120,0});g_names.push_back("fleet."+std::to_string(i));}
        SetInitialScene(scene.data(),target);screen=3;siteResult=1;api.airSite=+[](unsigned,double,double,double,double){return siteResult;};
        api.arrivalProgress=+[](int,int){};Snap empty;
        const int before=births;
        g_doomed.assign(2*target,{999,1000000,false});
        InitialScene(empty,player,1000000);
        check(births==before&&g_initialRejected.empty()&&InitialAdmissionsPending(),"temporary retirement pressure queues every requested slot without lowering the target");
        g_doomed.clear();
        for(int i=0;i<target+5;++i){
            InitialScene(empty,player,1000010+10*i);
            for(const auto& pair:g_pairs)if(pair.vehicle){refs[pair.vehicle].has3D=0;refs[pair.driver].has3D=0;}
        }
        check(births==before+target&&FleetCount()==target&&g_initialRejected.empty(),"80 requested positions create exactly 80 pairs across the sky, with no 12-pair or distance cutoff");
        InitialScene(empty,player,1002000);
        check(births==before+target,"a filled rule target never creates duplicate extra pairs");
        const unsigned first=g_pairs[0].vehicle;
        Step(0,g_pairs[0],g_spots[0],player,empty,1100000);
        check(g_pairs[0].vehicle==first&&g_pairs[0].proving&&g_doomed.empty(),"an unloaded distant pair waits frozen instead of failing a seating test before models load");
        Reset();
        refs.clear();g_world="RetryTargetFixture";g_pairs.assign(1,{});g_pairs[0].st=St::Empty;
        g_spots.assign(1,{});g_names={"fleet.0"};g_reuse={-1};SetInitialScene(scene.data(),1);
        allowBirth=false;const int beforeRetry=births;
        for(int i=0;i<3;++i)InitialScene(empty,player,1200000+5001*i);
        check(births==beforeRetry&&InitialAdmissionsPending(),"three unsuccessful creations do not abandon the requested position");
        allowBirth=true;InitialScene(empty,player,1215003);
        check(births==beforeRetry+1&&g_pairs[0].vehicle&&!g_initialRejected.count(0),"the same position resumes after creation becomes available, without a reset");
        Reset();
    }
    {   // Trust first: a pair shows and is judged later. A fault ahead of the player goes sooner, and never where it can be seen.
        Reset();refs.clear();g_world="TrustZoneFixture";g_names={"route.0"};g_pairs.assign(1,{});g_spots={{500,0,0,0}};g_wantSpots=g_spots;
        api.pilotHolds=+[](unsigned){return 1;};
        static int livePilot=1;api.pilotLive=+[](unsigned){return livePilot;};
        auto fresh=[&]{auto& q=g_pairs[0];q=Pair{};q.vehicle=800;q.driver=801;q.base=10;q.st=St::Started;q.bornAt=1;
            refs[800]={800,10,1,0,1,0,0,500,0,0};refs[801]={801,1,1,0,1,1,0,500,0,0};};
        fresh();auto& q=g_pairs[0];screen=1;visible=2;linked=800;pose=3;
        FvoTrafficVehicle seen{};seen.id=800;seen.hasBody=1;Snap withBody{seen};Pos me{};
        ZoneTick(me,withBody,100000);
        check(q.zone.stage==trustzone::Stage::Trusted&&q.vehicle==800,"a loaded pair is trusted as soon as it is seen, and nothing touches it");
        ZoneTick(me,withBody,102100);
        ZoneTick(me,withBody,103200);
        check(q.zone.stage==trustzone::Stage::Checked&&g_zoneRadius==500,"seated and held up: checked, and the zone reaches out to it");
        pose=0;ZoneTick(me,withBody,104300);
        check(q.zone.stage==trustzone::Stage::Trusted&&g_zoneLostChecks==1&&g_zoneRadius==0,"a checked pair that loses its seat is counted and pulls the zone back");
        pose=3;ZoneTick(me,Snap{},105400);ZoneTick(me,Snap{},107500);
        check(q.zone.stage==trustzone::Stage::Checked,"a held far vehicle left out of the vehicle list still passes: its physics is stepping");
        livePilot=-1;api.pilotHolds=+[](unsigned){return 0;};ZoneTick(me,Snap{},108600);
        check(q.zone.stage==trustzone::Stage::Trusted&&g_zoneLostChecks==2,"with no pilot, a vehicle with no chassis does not pass");
        livePilot=1;api.pilotHolds=+[](unsigned){return 1;};
        refs[801].has3D=0;ZoneTick(me,withBody,109700);
        check(q.zone.stage==trustzone::Stage::Unloaded&&g_zoneLostChecks==2,"a model lost to distance is not a lost check");
        refs[801].has3D=1;
        // Not aboard: a proven fault. Standing still it waits the full 10 s.
        linked=1;g_path={};
        check(!ShelveBad(0,q,200000)&&q.badSince==200000,"a fault starts its wait");
        check(!ShelveBad(0,q,206000)&&q.vehicle==800,"away from the player's path 6 s is too soon");
        check(ShelveBad(0,q,211000)&&q.vehicle!=800,"after 10 s it is swapped, unseen");
        // Ahead of a moving player it goes at 5 s.
        Reset();g_world="TrustZoneFixture";g_names={"route.0"};g_pairs.assign(1,{});g_spots={{500,0,0,0}};g_wantSpots=g_spots;fresh();
        auto& a=g_pairs[0];screen=1;visible=2;linked=1;
        g_path.Sample(0,0,0,299900);g_path.Sample(5,0,0,300000);
        check(g_path.Ahead(500,0,0,300000),"fixture: the pair is on the player's path");
        check(!ShelveBad(0,a,300000),"ahead, the fault starts its wait too");
        g_path.Sample(30,0,0,303000);g_path.Sample(35,0,0,303100);
        check(!ShelveBad(0,a,303100)&&a.vehicle==800,"not at once: the seat repair gets its time");
        g_path.Sample(55,0,0,305100);g_path.Sample(60,0,0,306000);g_path.Sample(65,0,0,306100);
        check(ShelveBad(0,a,306100)&&a.vehicle!=800,"after 5 s it is swapped before the player arrives");
        // Ahead and in plain view it stays: nothing is removed where it can be seen.
        Reset();g_world="TrustZoneFixture";g_names={"route.0"};g_pairs.assign(1,{});g_spots={{500,0,0,0}};g_wantSpots=g_spots;fresh();
        auto& v=g_pairs[0];screen=1;visible=1;linked=1;
        g_path.Sample(0,0,0,399900);g_path.Sample(5,0,0,400000);
        ShelveBad(0,v,400000);g_path.Sample(55,0,0,406000);g_path.Sample(60,0,0,406100);
        check(!ShelveBad(0,v,406100)&&v.vehicle==800,"a fault in view waits until it can go unseen");
        // A pilot flying a loaded vehicle that gets no physics steps: it hangs where it is, so it is a fault in its own right.
        Reset();g_world="TrustZoneFixture";g_names={"route.0"};g_pairs.assign(1,{});g_spots={{500,0,0,0}};g_wantSpots=g_spots;fresh();
        auto& n=g_pairs[0];screen=1;visible=2;linked=800;pose=3;livePilot=0;
        ZoneTick(me,withBody,500000);
        check(n.noPhysicsSince==500000&&n.zone.stage==trustzone::Stage::Trusted,"no physics steps starts its clock, and fails the check");
        check(PairVerdict(n,510000)==trafficpair::Bad::None,"a short spell is not a fault: chassis builds and hitches take seconds");
        livePilot=1;ZoneTick(me,withBody,511000);
        check(!n.noPhysicsSince,"steps coming back clear it");
        livePilot=0;ZoneTick(me,withBody,512000);screen=2;ZoneTick(me,withBody,513000);
        check(!n.noPhysicsSince,"under a loading screen or the map nothing steps, so it does not count");
        screen=1;ZoneTick(me,withBody,514000);
        refs[800].has3D=0;ZoneTick(me,withBody,515000);
        check(!n.noPhysicsSince,"nor does a vehicle whose model is not loaded");
        refs[800].has3D=1;ZoneTick(me,withBody,516000);
        check(PairVerdict(n,516000+trafficpair::kNoPhysicsFaultMs)==trafficpair::Bad::NoPhysics,"20 s without steps is a proven fault");
        n.badSince=0;check(!ShelveBad(0,n,516000+trafficpair::kNoPhysicsFaultMs)&&n.vehicle==800,"which then gets its normal wait");
        visible=1;check(!ShelveBad(0,n,516000+trafficpair::kNoPhysicsFaultMs+11000)&&n.vehicle==800,"in view it stays");
        visible=2;check(ShelveBad(0,n,516000+trafficpair::kNoPhysicsFaultMs+12500)&&n.vehicle!=800,"unseen it is swapped, which reloads its model and builds a new chassis");
        livePilot=1;api.pilotLive=nullptr;
        linked=1;pose=3;visible=2;api.pilotHolds=nullptr;Reset();
    }
    DeleteFileW(g_testIni.c_str());
    std::printf("traffic generator: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
