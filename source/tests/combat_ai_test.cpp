#include "combat_ai.h"
#include <cstring>
#include <chrono>
using namespace combatai;
using steer::V3;
namespace {
struct FakeWorld : steer::World
{
    double wallY = 1e9;  // a terrain wall rising north of wallY
    bool Solid(const V3&) const override { return false; }
    double Ground(double, double y) const override { return y >= wallY ? 50.0 : 0.0; }
    bool Blocked(const V3& a, const V3& b) const override { for (int k = 1; k < 30; ++k) { const V3 p = a + (b - a) * (k / 30.0); if (Ground(p.x, p.y) > p.z) return true; } return false; }
};
Situation Fight(double myX, double myY, double myVx, double myVy, double foeX, double foeY, double foeVx, double foeVy)
{
    Situation s;
    s.now = 100000; s.dt = 0.05;
    s.me.id = 1; s.me.unit.body.pos = { myX, myY, 5 }; s.me.unit.body.vel = { myVx, myVy, 0 }; s.me.unit.body.heading = steer::HeadingOf((myVx || myVy) ? V3{ myVx, myVy, 0 } : V3{ 0, 1, 0 });
    s.haveFoe = true; s.foe.id = 2; s.foe.inVehicle = true; s.foe.unit.body.pos = { foeX, foeY, 5 }; s.foe.unit.body.vel = { foeVx, foeVy, 0 };
    s.foe.unit.body.heading = steer::HeadingOf((foeVx || foeVy) ? V3{ foeVx, foeVy, 0 } : V3{ 0, 1, 0 });
    s.anchor = { myX, myY, 0 }; s.zone = 400;
    return s;
}
}
int main()
{
    int failures = 0;
    auto check = [&](bool ok, const char* msg) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", msg); if (!ok) ++failures; };
    FakeWorld w;
    Switches sw;
    {
        State st; Output o;
        Situation s = Fight(0, 0, 0, 20, 0, 60, 0, 15);  // right behind it
        Tick(st, s, w, sw, o);
        check(o.changed && std::strstr(o.log, "engage") && o.useCommands, "first tick: an engage decision, logged, flown by commands");
        check(o.aim && o.fire, "behind it with a solution: aim and fire");
        check(o.cmd.yawRate == o.cmd.yawRate && std::fabs(o.cmd.yawRate) < 0.5, "the nose stays on it");
        s.now += 50; Tick(st, s, w, sw, o);
        check(!o.changed, "the same situation: no new log line");
    }
    {
        State st; Output o;
        Situation s = Fight(0, 0, 0, 20, 0, -40, 0, 22);  // on my tail
        Tick(st, s, w, sw, o);
        s.hitRecently = true; s.now += 10; Tick(st, s, w, sw, o);
        s.now += 600; Tick(st, s, w, sw, o);  // after the reaction time
        check(std::strstr(o.log, "react") && std::strstr(o.log, "hit"), "a hit: the react branch answers after the reaction time (logged with the impulse)");
        check(!o.fire, "nothing to fire at behind me");
    }
    {
        State st; Output o;
        Situation s = Fight(0, 0, 0, 25, 0, 200, 0, 0);
        w.wallY = 30;  // terrain rising 30 m ahead
        Tick(st, s, w, sw, o);
        check(!o.useCommands, "the steering would meet terrain within 2.5 s: the planner's carrot wins");
        w.wallY = 1e9;
    }
    {
        State st; Output o; Switches off = sw; off.steering = false;
        Situation s = Fight(0, 0, 0, 20, 0, 60, 0, 15);
        Tick(st, s, w, off, o);
        check(!o.useCommands && std::hypot(o.carrot.x, o.carrot.y) >= 50.0, "steering switched off: only the carrot");
    }
    {
        State st; Output o;
        Situation s = Fight(0, 0, 0, 20, 0, 80, 0, 10);
        s.me.unit.health = 0.2;
        Tick(st, s, w, sw, o);
        check(std::strstr(o.log, "self-preservation") && !o.fire, "low health: self-preservation, no firing");
    }
    {
        State st; Output o;
        Situation s = Fight(0, 0, 0, 20, 0, 80, 0, 10);
        s.role = brain::Role::Overwatch;
        Tick(st, s, w, sw, o);
        check(std::strstr(o.log, "overwatch") && o.aim, "the squad's overwatch role: hold high, nose on it");
        Switches ns = sw; ns.squads = false; State st2;
        Tick(st2, s, w, ns, o);
        check(!std::strstr(o.log, "overwatch"), "squads off: the role is ignored");
    }
    {
        State st; Output o;
        Situation s = Fight(0, 0, 0, 0, 0, 100, 0, 0);
        s.foe.visible = false;
        Tick(st, s, w, sw, o);
        check(!std::strstr(o.log, "engage"), "never seen: no engage");
        State st2; Situation v = Fight(0, 0, 0, 10, 0, 100, 0, 0);
        Tick(st2, v, w, sw, o);
        bool searched = false;
        v.foe.visible = false; v.now += 4000; Tick(st2, v, w, sw, o); searched = searched || std::strstr(o.log, "search");
        v.now += 600; Tick(st2, v, w, sw, o); searched = searched || std::strstr(o.log, "search");
        check(searched, "seen then lost for 4 s: search (logged once)");
    }
    {
        State st; Output o;
        Situation s = Fight(0, 0, 0, 20, 0, 60, 0, 15);
        s.mates[0].pos = { 0.5, 30, 5 }; s.mateCount = 1;
        Tick(st, s, w, sw, o);
        check(!o.fire, "an ally in the line: hold fire");
    }
    {
        State st; Output o; Switches nt = sw; nt.tactics = false;
        Situation s = Fight(-40, 0, 25, 0, 0, 0, 0, 5);  // perfect T-bone geometry
        Tick(st, s, w, nt, o);
        check(!std::strstr(o.log, "ram"), "tactics off: no rams");
        State st2; Tick(st2, s, w, sw, o);
        check(std::strstr(o.log, "ram") || std::strstr(o.log, "sideswipe") || std::strstr(o.log, "tactic"), "tactics on: a vehicle tactic is chosen");
    }
    {
        State st; Output o; Switches nm = sw; nm.morale = false;
        Situation s = Fight(0, 0, 0, 20, 0, 80, 0, 10); s.enemyCount = 6;
        for (int i = 0; i < 200; ++i) { s.now += 100; s.dt = 0.1; Tick(st, s, w, nm, o); }
        check(st.mood == brain::Mood::Steady, "morale off: never shaken");
        State st2;
        for (int i = 0; i < 2000; ++i) { s.now += 100; s.dt = 0.1; Tick(st2, s, w, sw, o); }
        check(st2.morale.value < 0.6, "morale on and badly outnumbered: morale drops");
    }
    {
        // Cost of the logic itself (the fake world is cheap; the nav map adds its lookups): ticks of one vehicle in a running fight.
        State st; Output o; Situation s = Fight(0, 0, 0, 20, 30, 80, -10, 5); s.otherCount = 4; s.mateCount = 2; s.enemyCount = 2;
        const auto t0 = std::chrono::steady_clock::now();
        const int n = 2000;
        for (int i = 0; i < n; ++i) { s.now += 16; s.dt = 0.016; s.foe.unit.body.pos.x -= 0.2; Tick(st, s, w, sw, o); }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / n;
        std::printf("INFO one vehicle-tick costs %.1f us of logic (budget: 200 us for all vehicles)\n", us);
        check(us < 100.0, "one vehicle-tick of logic stays well under the budget");
    }
    {
        // On-foot combat must arrive at a finite position, not orbit a moving carrot.
        Situation s = Fight(0, 0, 0, 0, 0, 60, 0, 0);
        s.foe.inVehicle = false; s.foe.unit.body.pos.z = 0; s.firingHeading = 3.141592653589793;
        Switches options = sw; options.morale = false;
        State st; Output o;
        Tick(st, s, w, options, o);
        check(st.dec.action == brain::Action::FirePosition && o.hold && o.aim,
            "on-foot target: choose a stationary firing lane and track the target immediately");
        check(o.fire, "safe aligned shot can fire while the vehicle is repositioning");
        const V3 spot = o.carrot;
        s.me.unit.body.pos = spot; s.me.unit.body.vel = {};
        s.me.unit.body.heading = steer::HeadingOf(s.foe.unit.body.pos-spot);
        for (int i=0;i<120;++i) { s.now+=50; Tick(st,s,w,options,o); }
        check(o.hold && steer::Len(o.carrot-spot)<1e-6 && o.speed<0.01 && !o.useCommands,
            "at the firing lane: no minimum cruise speed or orbit and no altitude creep");
        Situation flank=s; flank.role=brain::Role::Flanker; flank.roleSlot=1;
        State fs; Output fo; Tick(fs,flank,w,options,fo);
        check(fo.hold && steer::Len2D(fo.carrot-spot)>15,
            "flanker receives a separate firing lane instead of chasing an infantry tail");
        Situation support=s; support.role=brain::Role::Suppressor;
        State ss; Output so; support.weapon.range=30; Tick(ss,support,w,options,so);
        check(so.hold && steer::Len(so.carrot-support.foe.unit.body.pos)<30,
            "support position fits the actual weapon range instead of a fixed 90 m perch");
        Situation hidden=s; hidden.foe.visible=false; hidden.now+=4000;
        Tick(st,hidden,w,options,o); check(!o.fire,"hidden target still blocks fire");
        State blocked; Situation friendly=s; friendly.mateCount=1;
        friendly.mates[0].pos=(friendly.me.unit.body.pos+friendly.foe.unit.body.pos)*0.5;
        Tick(blocked,friendly,w,options,o); check(!o.fire,"ally in the firing lane still blocks fire");
        State dead; Situation wreck=s; wreck.me.wrecked=true;
        Tick(dead,wreck,w,options,o); check(!o.fire,"wrecked vehicle cannot fire from a firing lane");
        coverfight::Burst burst;
        check(burst.Update(true,1000) && !burst.Update(false,1050) && burst.Update(true,1060),
            "brief unsafe shot stops immediately and resumes without a new artificial cooldown");
        check(!burst.Update(true,4060) && !burst.Update(true,4300) && burst.Update(true,4460),
            "completed burst still respects its normal rest interval");
    }
    {
        Switches options=sw; options.morale=false;
        Situation s=Fight(0,0,0,0,0,60,0,0); s.foe.inVehicle=false; s.foe.unit.body.pos.z=0;
        State st; Output o; Tick(st,s,w,options,o); const V3 spot=o.carrot;
        s.foe.unit.body.pos.x+=1.0; s.now+=600; Tick(st,s,w,options,o);
        check(steer::Len(o.carrot-spot)<1e-6,"small target movement leaves the firing position stable");
        s.me.unit.body.pos=spot; s.me.unit.body.vel={};
        s.me.unit.body.heading=steer::HeadingOf(s.foe.unit.body.pos-spot);
        s.otherCount=12;
        for(int i=0;i<12;++i)s.others[i].pos={500.0+10*i,500,5};
        s.others[11].pos=spot+V3{1,0,0};
        s.now+=50; Tick(st,s,w,options,o);
        check(o.avoiding&&o.useCommands,"a collision threat after the eighth snapshot entry overrides a firing hold");
        Situation support=Fight(0,0,0,0,0,60,0,0); support.weapon.range=30; support.role=brain::Role::Overwatch;
        State os; Tick(os,support,w,options,o);
        check(o.hold&&steer::Len(o.carrot-support.foe.unit.body.pos)<30,"overwatch against a vehicle also respects short weapon range");
        support.role=brain::Role::Reserve; State rs; Tick(rs,support,w,options,o);
        check(o.hold&&steer::Len(o.carrot-support.foe.unit.body.pos)<30,"reserve holds within usable weapon range rather than a fixed 150 metres");
    }
    {
        struct LaneWorld : FakeWorld {
            bool Solid(const V3& p) const override {return std::fabs(p.x)<4&&p.y>20&&p.y<40;}
            bool Blocked(const V3& a,const V3& b) const override {
                for(int k=0;k<=30;++k)if(Solid(a+(b-a)*(k/30.0)))return true;
                return FakeWorld::Blocked(a,b);
            }
        } lw;
        Switches options=sw; options.morale=false;
        Situation s=Fight(0,0,0,0,0,60,0,0); s.foe.inVehicle=false; s.foe.unit.body.pos.z=0;
        State st; Output o; Tick(st,s,lw,options,o);
        check(o.hold&&!lw.Solid(o.carrot)&&!lw.Blocked(o.carrot+V3{0,0,1.5},s.foe.unit.body.pos+V3{0,0,1.2}),
            "blocked preferred lane chooses a clear alternate instead of holding inside a wall");
        Situation clear=Fight(0,0,0,0,0,60,0,0); clear.foe.inVehicle=false; clear.foe.unit.body.pos.z=0;
        State cs; Tick(cs,clear,w,options,o); const V3 first=o.carrot;
        clear.mateCount=1; clear.mates[0].pos=(first+clear.foe.unit.body.pos)*0.5; clear.mates[0].radius=3;
        clear.now+=500; Tick(cs,clear,w,options,o);
        check(steer::Len(o.carrot-first)<1e-6,"briefly blocked lane does not trigger immediate repositioning");
        clear.now+=1000; Tick(cs,clear,w,options,o);
        check(steer::Len(o.carrot-first)>5,"persistent allied obstruction selects another firing lane");
    }
    {
        Situation s=Fight(0,0,0,0,0,60,0,0); s.reinforcements=true; s.me.person.courage=0.2;
        State st; Output o; Tick(st,s,w,sw,o); const double rallied=st.morale.value;
        for(int i=0;i<10;++i){s.now+=16;Tick(st,s,w,sw,o);}
        check(st.morale.value<=rallied+0.01,"one reinforcement event does not apply a rally every frame");
    }
    return failures ? 1 : 0;
}
