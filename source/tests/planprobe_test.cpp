// Look-ahead doubt probe target picker (planprobe.h).
#include "planprobe.h"
#include <cstdio>
using namespace planprobe;
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
static planworld::Shape Hit(double x, double y, double z, int support, bool normal, double seen) {
    planworld::Shape s; s.centre = { x, y, z }; s.support = support; s.haveNormal = normal; s.seenAt = seen; return s;
}

int main() {
    const double now = 100; Recent recent; Target t[8];
    // The car at the origin heading +y at 10 m/s, brake 6: stopping distance 10^2/12 + 2 = 10.3 m.
    {   planworld::Shape s[] = { Hit(0, 20, 1, 1, false, now), Hit(0, 25, 1, 3, true, now), Hit(0, 5, 1, 1, false, now), Hit(0, 60, 1, 1, false, now), Hit(9, 20, 1, 1, false, now) };
        const int n = Pick(s, 5, nullptr, 0, 0, 0, 0, 10, 6, 4, now, recent, t, 8);
        Check(n == 1 && std::fabs(t[0].y - 20) < 1e-9 && t[0].kind == Kind::DoubtfulShape, "only the doubtful hit between stopping distance and 40 m, near the line");
    }
    {   planworld::Shape s[] = { Hit(0, 20, 1, 3, true, now - 10) };
        Check(Pick(s, 1, nullptr, 0, 0, 0, 0, 10, 6, 4, now, recent, t, 8) == 1, "a stale hit is doubtful");
        planworld::Shape box = Hit(0, 20, 1, 1, false, now); box.source = planworld::Source::Vehicle;
        Check(Pick(&box, 1, nullptr, 0, 0, 0, 0, 10, 6, 4, now, recent, t, 8) == 0, "live sources are never doubt");
        box.source = planworld::Source::Survey; Check(Pick(&box, 1, nullptr, 0, 0, 0, 0, 10, 6, 4, now, recent, t, 8) == 1, "a prediction is always worth checking");
    }
    {   Sample p[4]; for (int i = 0; i < 4; ++i) { p[i].x = 0; p[i].y = p[i].along = 12 + i * 2; p[i].wanted = true; p[i].measured = i == 1; }
        const int n = Pick(nullptr, 0, p, 4, 0, 0, 0, 10, 6, 4, now, recent, t, 8);
        Check(n == 3 && t[0].kind == Kind::UnmeasuredFloor && std::fabs(t[0].along - 12) < 1e-9, "unmeasured wanted floor ahead, nearest first; measured skipped");
    }
    {   planworld::Shape s[6]; for (int i = 0; i < 6; ++i) s[i] = Hit(0, 15 + i * 3, 1, 1, false, now);
        const int n = Pick(s, 6, nullptr, 0, 0, 0, 0, 10, 6, 4, now, recent, t, 8);
        Check(n == planbranch::rules::probeTargets && t[0].along < t[1].along, "at most probeTargets, nearest first");
        Recent r; r.Note(0, 15, now - .5);
        Check(Pick(s, 1, nullptr, 0, 0, 0, 0, 10, 6, 4, now, r, t, 8) == 0, "a spot probed within the cooldown is skipped");
        Check(Pick(s, 1, nullptr, 0, 0, 0, 0, 10, 6, 4, now + 2, r, t, 8) == 1, "after the cooldown it is probed again");
    }
    {   planworld::Shape s[] = { Hit(20, 0, 1, 1, false, now) };  // heading +x: atan2(1, 0) = pi/2
        Check(Pick(s, 1, nullptr, 0, 0, 0, 1.5707963, 10, 6, 4, now, recent, t, 8) == 1, "ahead is the heading, not +y");
    }
    {   Sample line[] = {{0,0,0,0,0,false,true}, {0,15,0,15,0,false,true}, {20,15,0,35,1.5707963,false,true}};
        auto s = Hit(10,15,1,1,false,now);
        const int n = Pick(&s,1,line,3,0,0,0,10,6,4,now,recent,t,8);
        Check(n == 1 && std::fabs(t[0].along-25) < 1e-9 && std::fabs(t[0].heading-1.5707963)<1e-6,
            "probe around bend using route distance and segment heading");
        s = Hit(0,30,1,1,false,now);
        Check(Pick(&s,1,line,3,0,0,0,10,6,4,now,recent,t,8)==0,
            "nose-line obstacle beyond turn is outside route corridor");
    }
    {   planworld::Shape s[65]; for(int i=0;i<64;++i) s[i]=Hit(0,30+i*.01,1,1,false,now);
        s[64]=Hit(0,12,1,1,false,now);
        Pick(s,65,nullptr,0,0,0,0,10,6,4,now,recent,t,8);
        Check(std::fabs(t[0].along-12)<1e-9,"nearest shape after first 64 is retained");
        Sample floor{0,11,0,11,0,true,false};
        Pick(s,65,&floor,1,0,0,0,10,6,4,now,recent,t,8);
        Check(t[0].kind==Kind::UnmeasuredFloor,"near unmeasured floor is not starved by shapes");
    }
    std::printf("%s\n", failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
