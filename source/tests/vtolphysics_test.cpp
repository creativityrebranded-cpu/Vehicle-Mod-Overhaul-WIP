#include "vtolphysics.h"
#include <cstdio>
#include <limits>

int main() {
    int checks=0,failed=0;
    auto check=[&](bool ok,const char* name) { ++checks; if(!ok) { ++failed; std::printf("FAIL %s\n",name); } };
    auto close=[](double a,double b) { return std::fabs(a-b)<1e-7; };
    vtol::Body body;
    body.motion={0.001,{0.01,0.005,0.0025},{0,0,0,1}};
    vtol::State state; vtol::Command command;
    const double dt=1.0/60;
    auto output=vtol::Solve(body,command,state,dt);
    check(output.valid&&close(output.linear.z*body.motion.inverseMass/dt,9.8),"upright hover applies weight-compensating thrust as an impulse");
    check(close(output.linear.x,0)&&close(output.linear.y,0)&&close(vtol::Length(output.angular),0),"neutral upright craft gets no spurious horizontal force or torque");
    const auto unmodifiedVelocity=body.velocity;
    check(close(vtol::Length(body.velocity-unmodifiedVelocity),0),"solver does not mutate body velocity");
    const double a=3.141592653589793/3;
    auto banked=body; banked.right={std::cos(a),0,-std::sin(a)};banked.up={std::sin(a),0,std::cos(a)};
    state={}; output=vtol::Solve(banked,command,state,dt);
    check(output.valid&&close(output.collective,19.6),"60 degree bank requires twice the collective to support weight");
    check(close(output.linear.z*body.motion.inverseMass/dt,9.8)&&output.linear.x>0,"banked support comes from actual body-up thrust with lateral acceleration");
    auto inverted=body; inverted.right={-1,0,0};inverted.up={0,0,-1};
    state={}; output=vtol::Solve(inverted,command,state,dt);
    check(output.valid&&output.linear.z<0,"inverted thrusters push downward rather than providing unconditional world-up lift");
    check(close(state.trim,9.8),"inverted altitude error cannot wind up collective trim");
    command.assist=false; body.velocity={20,4,0}; body.spin={0.3,0.4,0.5};
    state={};output=vtol::Solve(body,command,state,dt);
    check(close(output.linear.x,0)&&close(output.linear.y,0),"assist off preserves drift with no horizontal thrust input");
    check(close(vtol::Length(output.angular),0),"assist off applies zero torque for zero command and preserves uncommanded spin");
    command.manualLift=1;state={};output=vtol::Solve(body,command,state,dt);
    check(close(output.collective,17.8),"manual lift changes bounded collective rather than setting vertical speed");
    command.manualLift=-1;state={};output=vtol::Solve(body,command,state,dt);
    check(close(output.collective,0),"full manual descent can cut collective to zero on a low-gravity world");
    command.manualLift=0;command.forwardAcceleration=12;state={};output=vtol::Solve(body,command,state,dt);
    check(close(output.linear.y*body.motion.inverseMass/dt,12),"forward thrust acts along the chassis nose");
    const auto fixed=state;auto heavy=body;heavy.motion.inverseMass=0.0005;
    auto loadedState=fixed;const auto heavyOutput=vtol::Solve(heavy,command,loadedState,dt);
    check(close(heavyOutput.linear.y,output.linear.y)&&close(heavyOutput.linear.y*heavy.motion.inverseMass/dt,6),"adding mass does not create free thruster force; the same thruster accelerates less");
    command.brake=true;command.forwardAcceleration=0;body.velocity={0,10,0};state={};output=vtol::Solve(body,command,state,dt);
    check(output.linear.y<0&&close(body.velocity.y,10),"braking uses reverse thrust without overwriting the incoming velocity");
    command={};command.verticalSpeed=15;state={};output=vtol::Solve(body,command,state,dt);
    check(output.saturated&&close(output.collective,35)&&close(state.trim,9.8),"saturated climb has bounded thrust and anti-windup");
    command={};body.velocity={};body.spin={};command.rates={0.2,0,0};state={};output=vtol::Solve(body,command,state,dt);
    auto response=vtol::AngularDelta(body.motion,output.angular);
    check(output.valid&&response.x>0&&close(response.y,0)&&close(response.z,0),"pitch command applies an angular impulse through native inertia");
    check(close(response.x,1.0*dt),"rate controller produces the requested acceleration through inertia rather than prescribing spin");
    const vtol::Vec impulse{1,2,3};const auto delta=vtol::AngularDelta(body.motion,impulse);
    check(close(delta.x,0.01)&&close(delta.y,0.01)&&close(delta.z,0.0075),"principal inertia determines the angular impulse response");
    auto rotated=body.motion;const double q=std::sqrt(0.5);rotated.rotation={0,0,q,q};
    const auto rotatedDelta=vtol::AngularDelta(rotated,{1,0,0});
    check(close(rotatedDelta.x,0.005)&&close(rotatedDelta.y,0),"rotated principal inertia is applied in the correct world frame");
    const vtol::Vec desired{0.5,-0.25,1};const auto roundtrip=vtol::AngularDelta(rotated,vtol::AngularImpulse(rotated,desired,dt));
    check(vtol::Length(roundtrip-desired*dt)<1e-8,"torque conversion round-trips through the native angular impulse response");
    command={};command.assist=false;command.forwardAcceleration=10;vtol::Vec total30{},total120{};
    for(int i=0;i<30;++i) { state={};total30=total30+vtol::Solve(body,command,state,1.0/30).linear; }
    for(int i=0;i<120;++i) { state={};total120=total120+vtol::Solve(body,command,state,1.0/120).linear; }
    check(vtol::Length(total30-total120)<1e-7,"equal simulation time produces equal total impulse at different step rates");
    const float quaternion[4]={0,0,0,1};const auto decoded=vtol::Decode(0x3A803B003B803C00ull,quaternion);
    check(close(decoded.inverseMass,0.0009765625)&&close(decoded.inverseInertia.x,0.0078125)&&
        close(decoded.inverseInertia.y,0.00390625)&&close(decoded.inverseInertia.z,0.001953125),"packed inverse inertia and inverse mass decode as upper float bits, not IEEE half floats");
    state={};check(!vtol::Solve(body,command,state,0).valid&&!vtol::Solve(body,command,state,0.5).valid,"paused and long steps apply no impulses");
    auto invalid=body;invalid.motion.inverseMass=0;
    check(!vtol::Solve(invalid,command,state,dt).valid,"static or unreadable mass is rejected");
    invalid=body;invalid.motion.inverseInertia.y=0;
    check(!vtol::Solve(invalid,command,state,dt).valid,"unusable inertia is rejected instead of guessed");
    invalid=body;invalid.motion.rotation.w=0;
    check(!vtol::Solve(invalid,command,state,dt).valid,"invalid motion frame is rejected");
    invalid=body;invalid.up={0,0,2};
    check(!vtol::Solve(invalid,command,state,dt).valid,"invalid chassis basis is rejected");
    invalid=body;invalid.velocity.x=std::numeric_limits<double>::quiet_NaN();
    check(!vtol::Solve(invalid,command,state,dt).valid,"nonfinite body velocity cannot produce an impulse");
    command.rates={1e6,1e6,1e6};command.forwardAcceleration=1e6;state={};output=vtol::Solve(body,command,state,dt);
    check(output.valid&&vtol::Length(output.linear)<=65*state.referenceMass*dt+1e-8&&
        vtol::Length(output.angular)<=state.referenceMass*command.radius*command.radius*6*dt+1e-8,"excessive commands remain within finite thruster and torque budgets");
    for(double gravity : {1.62,3.7,9.8,15.0}) {
        vtol::Body planet=body;planet.velocity={};planet.spin={};vtol::State trim;vtol::Command hold;
        for(int i=0;i<2400;++i) {
            const auto lift=vtol::Solve(planet,hold,trim,1.0/120);
            planet.velocity=planet.velocity+lift.linear*planet.motion.inverseMass-vtol::Vec{0,0,gravity/120};
        }
        check(std::fabs(planet.velocity.z)<0.002&&std::fabs(trim.trim-gravity)<0.01,"bounded hover assist trims to different world gravity through vertical-speed feedback");
    }
    {
        vtol::Body slip=body;slip.velocity={4,0,0};slip.spin={};vtol::State st;vtol::Command c;
        auto o=vtol::Solve(slip,c,st,dt);
        check(close(o.linear.x*slip.motion.inverseMass/dt,-3),"default side thrusters damp slip within 3 m/s^2");
        c.sideAcceleration=6;st={};o=vtol::Solve(slip,c,st,dt);
        check(close(o.linear.x*slip.motion.inverseMass/dt,-4.8),"AI side thrusters damp slip proportionally up to their larger bound");
        c.strafeAcceleration=9;st={};o=vtol::Solve(slip,c,st,dt);
        check(close(o.linear.x*slip.motion.inverseMass/dt,9),"strafe thrust acts along body right and is not cancelled by slip damping");
        c.strafeAcceleration=50;st={};o=vtol::Solve(slip,c,st,dt);
        check(close(o.linear.x*slip.motion.inverseMass/dt,12),"strafe thrust stays within its bound");
        c.strafeAcceleration=std::numeric_limits<double>::quiet_NaN();st={};
        check(!vtol::Solve(slip,c,st,dt).valid,"non-finite strafe is rejected");
    }
    std::printf("vtolphysics: %d checks, %d failures\n",checks,failed);
    return failed?1:0;
}
