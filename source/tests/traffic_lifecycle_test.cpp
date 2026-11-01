#define FVO_GENERATOR_TEST
#include "../src/traffic/generator.cpp"
#include <map>
namespace {
std::map<unsigned,FvoTrafficRef> refs;
bool vehiclePending=false,driverPending=false,vehicleDone=false,driverDone=false;
int vehicleCommands=0,driverCommands=0,enables=0,deletes=0;
unsigned moveId=0;bool moveDone=false;int moveCommands=0;
unsigned Create(unsigned base,double x,double y,double z,double){
    if(!vehiclePending){vehiclePending=true;++vehicleCommands;return 0;}
    if(!vehicleDone)return 0;vehiclePending=false;
    refs[800]={800,base,1,0,1,0,0,x,y,z};return 800;
}
unsigned Driver(unsigned,unsigned,double x,double y,double z,double,int*){
    if(!driverPending){driverPending=true;++driverCommands;return 0;}
    if(!driverDone)return 0;driverPending=false;
    refs[801]={801,1,1,0,1,1,0,x,y,z};return 801;
}
int Pending(unsigned id,const char* operation){
    return (std::strcmp(operation,"create")==0&&vehiclePending)||
        (std::strcmp(operation,"driver")==0&&driverPending)||
        (std::strcmp(operation,"move")==0&&moveId==id);
}
int Move(unsigned id,double,double,double,double){
    if(!moveId){moveId=id;++moveCommands;return 0;}
    if(!moveDone)return 0;moveId=0;moveDone=false;return 1;
}
void Show(unsigned id,int on){if(on)++enables;refs[id].disabled=on?0:1;refs[id].usable=on?1:0;}
}
int main(){
    using namespace generator;int checks=0,failures=0;
    auto check=[&](bool ok,const char* message){++checks;if(!ok){++failures;std::printf("FAIL %s\n",message);}};
    FvoTrafficApi api{};api.version=kFvoTrafficApiVersion;
    api.presentation=+[]{return 3;};api.airSite=+[](unsigned,double,double,double,double){return 1;};
    api.formExists=+[](unsigned){return 1;};api.createVehicle=Create;api.createSeatedDriver=Driver;api.mutationPending=Pending;
    api.ref=+[](unsigned id,FvoTrafficRef* out){auto it=refs.find(id);if(it==refs.end())return 0;*out=it->second;return 1;};
    api.makeTraffic=+[](unsigned,int){return 1;};api.show=Show;api.usesVehicle=+[](unsigned){return 0;};
    api.endJob=+[](unsigned,const char*){};api.moveTo=Move;api.holdBirth=+[](unsigned){};
    api.noticeFar=+[](unsigned){return 400.0;};api.deleteRef=+[](unsigned){++deletes;return 1;};
    Attach(&api,nullptr);g_world="AsyncFixture";g_pairs.assign(1,{});g_pairs[0].st=St::Empty;
    g_spots={{100,0,80,0}};g_reuse={-1};g_names={"async.0"};FvoPoolSpot origin{100,0,80,0};SetInitialScene(&origin,1);
    Snap empty;Pos player{};
    InitialScene(empty,player,1000);
    check(g_pairs[0].vehiclePending&&g_pairs[0].st==St::Created&&vehicleCommands==1,"pending vehicle reserves one admission");
    for(int n=0;n<50;++n){FinishBirth(0,g_pairs[0],1100+n);InitialScene(empty,player,1100+n);}
    check(vehicleCommands==1&&driverCommands==0&&g_initialFails.empty(),"delayed creation neither duplicates a vehicle nor counts as a failure");
    vehicleDone=true;FinishBirth(0,g_pairs[0],2000);
    check(g_pairs[0].vehicle==800&&!g_pairs[0].vehiclePending&&g_pairs[0].driverPending&&driverCommands==1,"vehicle acknowledgement advances to one pending driver");
    for(int n=0;n<50;++n)FinishBirth(0,g_pairs[0],2100+n);
    check(driverCommands==1&&g_pairs[0].driverPending,"driver delay cannot create another actor");
    g_rushClear=true;InitialScene(empty,player,3000);
    check(g_rushClear&&g_pairs[0].vehicle==800&&!refs[800].disabled,"reset waits to collect a pending driver before hiding its pair");
    driverDone=true;InitialScene(empty,player,3100);
    check(!g_rushClear&&refs[800].disabled&&refs[801].disabled&&g_spares.size()==1,"reset collects both results and retires the complete pair together");
    Reset();
    // A hidden spare is never enabled between its two asynchronous moves.
    Pair spare;spare.st=St::Created;spare.vehicle=800;spare.driver=801;spare.birth=origin;spare.relocating=1;
    enables=0;FinishBirth(0,spare,4000);FinishBirth(0,spare,4100);
    check(moveCommands==1&&enables==0&&refs[800].disabled&&refs[801].disabled,"spare vehicle move waits with both models disabled");
    moveDone=true;FinishBirth(0,spare,4200);FinishBirth(0,spare,4300);
    check(moveCommands==2&&moveId==801&&enables==0,"driver relocation starts only after vehicle completion");
    moveDone=true;FinishBirth(0,spare,4400);
    check(spare.relocating==0&&enables==2&&!refs[800].disabled&&!refs[801].disabled,"both moves acknowledge before either enable");
    // Enable acknowledgement is separate from move completion. A queued enable is not a broken pair.
    refs[800].disabled=refs[801].disabled=1;refs[800].usable=refs[801].usable=0;
    spare.relocating=2;moveId=801;moveDone=true;
    api.show=+[](unsigned,int){};
    check(!FinishBirth(0,spare,4500)&&spare.relocating==3,"queued enables keep the spare in relocation instead of failing its health check");
    refs[800].disabled=0;refs[800].usable=1;
    check(!FinishBirth(0,spare,4501)&&spare.relocating==3,"one enabled half cannot release a reused pair");
    refs[801].disabled=0;refs[801].usable=1;
    check(FinishBirth(0,spare,4502)&&spare.relocating==0,"both enables must be observed before reuse proceeds");
    api.show=Show;
    // A pair cannot belong to both the retirement queue and reusable stock (the 18:10 FF014591 crash).
    g_world="RetirementFixture";g_pairs.assign(1,{});g_spares.clear();g_sparesWorld=g_world;g_clock=5000;
    AddSpare(800,801,refs[800].baseId);DeletePair(800,801);DeletePair(800,801);
    check(g_doomed.size()==2&&g_spares.empty()&&Retiring(800)&&Retiring(801),"retirement removes reusable ownership and deduplicates both native deletions");
    AddSpare(800,801,refs[800].baseId);
    check(g_spares.empty(),"a retired pair cannot be adopted back onto the spare shelf");
    api.linkedPairs=+[](unsigned* drivers,unsigned* vehicles,int){drivers[0]=801;vehicles[0]=800;return 1;};
    refs[800].disabled=refs[801].disabled=0;refs[800].usable=refs[801].usable=1;
    SweepOrphans(9000);
    check(g_spares.empty(),"orphan discovery cannot re-adopt an id still owned by retirement");
    api.linkedPairs=nullptr;
    // Native deletion is deferred until disabled references have actually lost their scene models.
    g_doomed={{800,1},{801,1}};Show(800,0);Show(801,0);deletes=0;
    DeleteTick(10000);DeleteTick(10100);
    check(deletes==0&&g_doomed.size()==2,"native delete never races a model still being used by the engine");
    refs[800].has3D=refs[801].has3D=0;DeleteTick(10200);DeleteTick(10300);
    check(g_doomed.empty()&&deletes==2,"observed teardown allows one deletion per reference");
    AddSpare(800,801,refs[800].baseId);
    check(g_spares.empty(),"retired ids remain excluded after native completion until save reset");
    // The selected target admits a thirteenth pair, without duplicating its pending asynchronous birth.
    Reset();g_world="BudgetFixture";g_pairs.assign(13,{});g_spots.assign(13,origin);g_reuse.assign(13,-1);g_names.assign(13,"budget");
    for(int n=0;n<12;++n){g_pairs[n].st=St::Started;g_pairs[n].vehicle=unsigned(900+n);}
    g_pairs[12].st=St::Empty;std::vector<FvoPoolSpot> origins(13,origin);SetInitialScene(origins.data(),13);
    const int before=vehicleCommands;InitialScene(empty,player,20000);
    check(vehicleCommands==before+1&&g_pairs[12].vehiclePending&&!g_initialRejected.count(12),"twelve admitted pairs do not undercut a rule target of thirteen");
    InitialScene(empty,player,20001);
    check(vehicleCommands==before+1,"the thirteenth pending birth remains exactly one native command");
    Reset();DeleteFileW(g_testIni.c_str());
    std::printf("traffic lifecycle: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
