// Exercise the real addon's slot/step/lifecycle callbacks without a game process.
#include "../src/traffic/traffic_main.cpp"
#include "trafficcycle.h"

// The player's world as core would name it; the route of the test lives in every world (no world key).
int TestWorld(char* out, int size) { std::snprintf(out, std::size_t(size), "TestWorld"); return 1; }

int main()
{
    g_worldName = &TestWorld;
    int failed = 0, checks = 0;
    auto check = [&](bool ok, const char* what) {
        ++checks;
        if (!ok) { ++failed; std::printf("FAIL %s\n", what); }
    };
    trafficdata::Route route;
    route.shape = ringlogic::Clean({0,0,300,600,1,0,40});
    auto& slot = g_slots[0];
    slot = {};
    slot.used = slot.repeat = true;
    slot.line = trafficroute::Build(route);
    slot.route = "test";
    route.name = "test";
    g_data.routes = { route };  // a landing re-reads its route from the files (here: the same ring)
    trafficroute::Vec home{50,40,0}, p=home;
    slot.trip = trafficroute::StartLap(slot.line,0,home);
    for (int lap=0; lap<12; ++lap) {
        int result = 0;
        FvoTripGoal goal{};
        for (int frame=0; frame<5000 && !result; ++frame) {
            result = Step(&slot,p.x,p.y,p.z,&goal);
            if (result) break;
            const double dx=goal.x-p.x, dy=goal.y-p.y, dz=goal.z-p.z;
            const double distance=std::hypot(dx,dy,dz), scale=distance>10 ? 10/distance : 1;
            p.x+=dx*scale; p.y+=dy*scale; p.z+=dz*scale;
        }
        check(result==3 && slot.used,"lap completion retains slot and requests repeat at home");
        check(slot.trip.travelled>=slot.line.Span()*0.9,"each repeat traverses full circuit");
        int active=0;
        check(FreeSlot(active)!=&slot && active==1,"completed repeating slot cannot be reused during landing");
        for (int second=0; second<20; ++second) {
            check(Cycle(&slot,0,p.x,p.y,p.z)==1,"landing heartbeat retains completed trip");
            check(slot.trip.leg==trafficroute::Leg::Done,"heartbeat never starts takeoff");
        }
        p=home;
        check(Cycle(&slot,1,p.x,p.y,p.z)==1,"landed restart accepted");
        check(slot.trip.leg==trafficroute::Leg::Climb && slot.trip.travelled==0 && !slot.trip.uKnown,"restart clears lap progress");
    }
    slot.trip=trafficroute::StartTo(slot.line,2,home,{550,10,0},24);
    slot.trip.leg=trafficroute::Leg::Done;
    FvoTripGoal goal{};
    check(Step(&slot,550,10,0,&goal)==4 && goal.x==550 && goal.y==10,"place arrival requests repeat at destination");
    check(Cycle(&slot,1,550,10,0)==1 && slot.trip.lap && slot.trip.home.x==550,"after destination landing next trip is a full lap from there");
    slot.repeat=false; slot.trip.leg=trafficroute::Leg::Done;
    check(Step(&slot,550,10,0,&goal)==1 && !slot.used,"nonrepeating planner still releases slot at completion");
    check(Cycle(&slot,1,0,0,0)==0,"released slots cannot restart");
    check(Cycle(nullptr,1,0,0,0)==0,"invalid callback user is rejected");
    // Map route editor (2026-10-28): a landing takes up the saved route, or parks when nothing is active.
    {
        auto& s2 = g_slots[1];
        s2 = {}; s2.used = s2.repeat = true; s2.route = "test"; s2.line = slot.line; s2.trip = trafficroute::StartLap(s2.line, 0, home);
        s2.trip.leg = trafficroute::Leg::Done; s2.seen = GetTickCount64();
        trafficdata::Route moved = route; moved.shape.cx = 1000;
        g_data.routes = { moved };
        check(Cycle(&s2,1,50,40,0)==1 && s2.line.lane[0].front().x > 500, "landing restart uses the route as the files say now");
        s2.trip.leg = trafficroute::Leg::Done;
        trafficdata::Route other = route; other.name = "ed_night"; other.shape.cx = 100;
        g_data.routes = { other };
        check(Cycle(&s2,1,650,40,0)==1 && s2.route == "ed_night", "deleted route: the vehicle switches to the nearest active route");
        s2.trip.leg = trafficroute::Leg::Done;
        g_data.routes[0].schedule.allDay = false; g_data.routes[0].schedule.windows = { {60, 120, 1} };
        check(Cycle(&s2,1,50,40,0)==0 && !s2.used, "unknown local time: a scheduled route waits and the vehicle stays parked");
        trafficdata::Route full = route; full.name = "ed_full"; full.schedule.allDayVehicles = 1;
        g_data.routes = { full };
        auto& s3 = g_slots[2]; s3 = {}; s3.used = true; s3.route = "ed_full"; s3.seen = GetTickCount64();
        s2 = {}; s2.used = s2.repeat = true; s2.route = "x"; s2.line = slot.line; s2.trip = trafficroute::StartLap(s2.line, 0, home);
        s2.trip.leg = trafficroute::Leg::Done; s2.seen = GetTickCount64();
        check(Cycle(&s2,1,50,40,0)==0, "a full route takes no more vehicles");
        s3 = {}; s2 = {};
    }
    slot.used=true; slot.repeat=true; slot.seen=GetTickCount64()-kLostMs-1;
    int active=0;
    check(FreeSlot(active)==&slot && active==0,"cancelled trip expires when heartbeat stops");
    // Junctions (2026-10-29): a ring with a branch that leaves at 180 degrees and rejoins at 270, then a spur.
    {
        trafficdata::Data d;
        trafficdata::Read(
            "[route.ring]\ncentre = 0 0\nheight = 100\nradius = 500\nlanes = 1\n\n"
            "[route.ed_branch]\ntype = path\npoints = 0 -500 100, -700 -700 100, -500 0 100\nlanes = 1\ndensity = 0\n\n"
            "[link.in]\nfrom = ring\nat = 180\nto = ed_branch\nto_at = start\nshare = 1\n\n"
            "[link.out]\nfrom = ring\nat = 270\nto = ed_branch\nto_at = end\nshare = 0\n", "j.ini", d);
        g_data = d;
        for (auto& o : g_slots) o = {};
        auto run = [&](Slot& s, trafficroute::Vec& p, std::vector<std::string>& seen) {
            FvoTripGoal goal{};
            for (int frame = 0; frame < 20000; ++frame) {
                const int r = Step(&s, p.x, p.y, p.z, &goal);
                if (seen.empty() || seen.back() != s.route) seen.push_back(s.route);
                if (r) return r;
                const double dx = goal.x - p.x, dy = goal.y - p.y, dz = goal.z - p.z;
                const double dist = std::hypot(dx, dy, dz), k = dist > 10 ? 10 / dist : 1;
                p.x += dx * k; p.y += dy * k; p.z += dz * k;
            }
            return 0;
        };
        trafficroute::Vec p{ 500, 60, 0 };
        FvoTripClaim claim{};
        trafficdata::Traffic rule; rule.reach = 400; rule.gap = 0;
        g_cycleSupported = true;  // core with the repeat cycle
        Slot* free = &g_slots[0];
        check(Begin(rule, "TestWorld", free, p.x, p.y, p.z, 0x1234, &claim, false) == 1 && free->route == "ring" && free->junctions.size() == 2, "lap starts on the ring with its two junctions");
        std::vector<std::string> seen;
        const int r = run(*free, p, seen);
        check(seen.size() == 3 && seen[1] == "ed_branch" && seen[2] == "ring", "share 1: the lap turns into the branch and rejoins the ring at its end");
        check(r == 3 && std::hypot(p.x - 500, p.y - 60) < 40, "the lap still ends at home");
        for(auto& link:d.links)link.radius=20;
        g_data=d;for(auto& entry:g_slots)entry={};p={500,60,0};free=&g_slots[0];
        check(Begin(rule,"TestWorld",free,p.x,p.y,p.z,0x1234,&claim,false)==1,"rounded junction lap begins");
        bool sawTurn=false,firstTurnGoal=true;FvoTripGoal roundedGoal{};
        for(int frame=0;frame<20000;++frame){
            const bool turning=!free->turn.empty();const int result=Step(free,p.x,p.y,p.z,&roundedGoal);
            if(!turning&&!free->turn.empty()){
                sawTurn=true;const auto& expected=free->turn[std::min(free->turnAt+1,free->turn.size()-1)];
                firstTurnGoal=firstTurnGoal&&std::hypot(roundedGoal.x-expected.x,roundedGoal.y-expected.y)<0.01;
            }
            if(result)break;
            const double dx=roundedGoal.x-p.x,dy=roundedGoal.y-p.y,dz=roundedGoal.z-p.z;
            const double distance=std::hypot(dx,dy,dz),scale=distance>2?2/distance:1;
            p.x+=dx*scale;p.y+=dy*scale;p.z+=dz*scale;
        }
        check(sawTurn&&firstTurnGoal,"new junction immediately emits its arc goal without a straight chord frame");
        // A spur: no junction at the branch's far end -> it lands there (with a landing place near it).
        trafficdata::Read("[place.pad]\npoint = -520 10 0\n", "k.ini", d);
        d.links.erase(d.links.begin() + 1);  // drop "out"
        g_data = d;
        p = { 500, 60, 0 };
        free = &g_slots[1];
        check(Begin(rule, "TestWorld", free, p.x, p.y, p.z, 0x99, &claim, false) == 1, "spur lap begins");
        seen.clear();
        FvoTripGoal goal{};
        const int r2 = run(*free, p, seen);
        check(r2 == 4 && free->dest == "pad" && seen.back() == "ed_branch", "spur: lands at the landing place by its far end");
        check(Cycle(free, 1, p.x, p.y, p.z) == 1 && free->route == "ring", "after the dwell it flies home to its own route");
        for (auto& o : g_slots) o = {};
    }
    {
        for(auto& entry:g_slots)entry={};
        FvoTrafficApi api{};api.usesVehicle=+[](unsigned id){return id==77?1:0;};api.tripActive=+[](unsigned id){return id==88?1:0;};
        g_trafficApi=&api;
        g_slots[0].used=true;g_slots[0].vehicleId=77;g_slots[0].seen=GetTickCount64()-10000;
        g_slots[1].used=true;g_slots[1].vehicleId=88;g_slots[1].seen=GetTickCount64()-10000;
        int count=0;auto* free=FreeSlot(count);
        check(count==2&&free==&g_slots[2],"menus/missing callbacks cannot recycle NPC or player Auto route slots while their owners remain active");
        g_slots[0].vehicleId=99;g_slots[1].vehicleId=99;FreeSlot(count);
        check(count==0,"unowned stale callback slots remain reclaimable");
        g_trafficApi=nullptr;for(auto& entry:g_slots)entry={};
    }
    {
        for(auto& entry:g_slots)entry={};trafficdata::Route exact=route;exact.name="exact";exact.lanes=2;exact.schedule.allDay=true;exact.schedule.allDayVehicles=4;
        g_data={};g_data.routes={exact};trafficdata::Traffic rule;FvoTripClaim claim{};const auto line=trafficroute::Build(exact);
        const auto at=trafficroute::At(line,1,10);auto* free=&g_slots[0];
        check(Begin(rule,"TestWorld",free,at.x,at.y,at.z,0x123,&claim,true,"exact",1)==1&&free->route=="exact"&&free->trip.lane==1&&free->trip.leg==trafficroute::Leg::Follow&&free->trip.uKnown,
            "covered initial pair resumes the exact authored route and lane as already underway");
    }
    std::printf("trafficcycle: %d checks, %d failures\n",checks,failed);
    return failed ? 1 : 0;
}
