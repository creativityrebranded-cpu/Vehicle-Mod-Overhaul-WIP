#include "vtolai.h"
#include "bodyattitude.h"
#include <cstdio>

int main() {
    int checks=0,failed=0;
    auto check=[&](bool ok,const char* name) { ++checks; if(!ok) { ++failed; std::printf("FAIL %s\n",name); } };
    auto close=[](double a,double b,double e=1e-9) { return std::fabs(a-b)<e; };
    vtolai::Input level;
    auto r=vtolai::Rates(level);
    check(close(vtol::Length(r),0),"level hover with no turn asks for no rotation");
    check(close(vtolai::TurnBank(20,0.2),std::atan2(20*0.2,9.8)),"coordinated bank follows tan(bank) = v * yaw rate / g");
    check(vtolai::TurnBank(20,0.4)>0&&vtolai::TurnBank(20,-0.4)<0,"a right turn banks right side down, a left turn left side down");
    check(close(vtolai::TurnBank(80,3),vtolai::kMaxBank),"turn bank is bounded");
    check(close(vtolai::TurnBank(0,1),0),"no bank when hovering in place: the turn is a pure yaw");
    vtolai::Input turn=level; turn.speed=20; turn.yawRate=0.4;
    r=vtolai::Rates(turn);
    check(close(r.z,-0.4),"commanded yaw is about world up, turn right negative (stock convention)");
    check(r.y>0,"the chassis rolls toward the coordinated bank about its nose");
    turn.yawRate=5; r=vtolai::Rates(turn);
    check(close(r.z,-1),"yaw rate respects the hull limit");
    vtolai::Input pitched=level; pitched.pitch=0.3; r=vtolai::Rates(pitched);
    check(close(r.x,-0.3*vtolai::kLevelRate),"an unaimed nose levels at the level rate");
    pitched.pitchAimOn=true; pitched.pitchAim=0.3; r=vtolai::Rates(pitched);
    check(close(r.x,0),"an aimed nose holds its pitch aim");
    vtolai::Input aimed=level; aimed.rollAimOn=true; aimed.rollAim=-0.2; r=vtolai::Rates(aimed);
    check(close(r.y,-0.2*vtolai::kBankGain),"a roll aim from the planner wins over the turn bank");
    vtolai::Input bad=level; bad.yawRate=std::nan(""); r=vtolai::Rates(bad);
    check(vtol::Finite(r)&&close(r.z,0),"a non-finite yaw command flies straight");
    // The physical branch must publish actual heading, including when it starts
    // at a nonzero angle or crosses +/-pi. A frozen heading makes this loop spin.
    struct Feedback { double bodyHeading=0; float bodyPitch=0, bodyRoll=0; } feedback;
    for (double start : {0.0, 2.8, -2.8}) {
        const double goal = start == 0.0 ? 1.5 : -start;
        double heading = start;
        for (int step=0; step<300; ++step) {
            const float right[3]={float(std::cos(heading)),float(-std::sin(heading)),0};
            const float forward[3]={float(std::sin(heading)),float(std::cos(heading)),0};
            const float up[3]={0,0,1};
            bodyattitude::Publish(feedback,right,forward,up);
            auto pilot=level;
            pilot.yawRate=std::clamp(1.6*vtolai::Wrap(goal-feedback.bodyHeading),-1.0,1.0);
            const auto rates=vtolai::Rates(pilot);
            heading=vtolai::Wrap(heading-rates.z/60.0);
        }
        check(std::fabs(vtolai::Wrap(goal-heading))<0.01,"physical heading feedback closes the turn without circling");
    }
    const float right[3]={1,0,0}, forward[3]={0,0.8f,0.6f}, up[3]={0,-0.6f,0.8f};
    bodyattitude::Publish(feedback,right,forward,up);
    check(close(feedback.bodyHeading,0)&&close(feedback.bodyPitch,std::atan2(0.6,0.8),1e-6)&&close(feedback.bodyRoll,0),
        "orientation feedback includes pitch and roll in the same sample");
    std::printf("vtolai: %d checks, %d failures\n",checks,failed);
    return failed?1:0;
}
