#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

// Thruster-driven VTOL. The native solver supplies gravity, contacts, mass and inertia. This module returns
// impulses; it never prescribes position, orientation or velocity. There is no atmospheric/wing model here.
namespace vtol {
struct Vec { double x=0, y=0, z=0; };
inline Vec operator+(Vec a, Vec b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
inline Vec operator-(Vec a, Vec b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
inline Vec operator*(Vec a, double s) { return {a.x*s,a.y*s,a.z*s}; }
inline double Dot(Vec a, Vec b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline Vec Cross(Vec a, Vec b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
inline bool Finite(Vec a) { return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z); }
inline double Length(Vec a) { return std::sqrt(Dot(a,a)); }
inline Vec Limited(Vec a,double limit) { const double n=Length(a); return n>limit&&n>0 ? a*(limit/n):a; }
struct Quat { double x=0,y=0,z=0,w=1; };
inline Vec Rotate(Quat q,Vec v) { const Vec axis{q.x,q.y,q.z}; return v+Cross(axis,Cross(axis,v)+v*q.w)*2; }
inline Vec Unrotate(Quat q,Vec v) { q.x=-q.x;q.y=-q.y;q.z=-q.z; return Rotate(q,v); }
inline double Expand(std::uint16_t value) { return double(std::bit_cast<float>(std::uint32_t(value)<<16)); }
struct Motion {
    double inverseMass=0;
    Vec inverseInertia{};  // principal axes, packed at hknpMotion+0x40, followed by inverse mass
    Quat rotation{};      // principal motion frame -> world, hknpMotion+0x00
};
inline Motion Decode(std::uint64_t packed,const float quaternion[4]) {
    return {Expand(std::uint16_t(packed>>48)), {Expand(std::uint16_t(packed)),Expand(std::uint16_t(packed>>16)),Expand(std::uint16_t(packed>>32))},
        {quaternion[0],quaternion[1],quaternion[2],quaternion[3]}};
}
inline bool Valid(const Motion& m) {
    const double n=m.rotation.x*m.rotation.x+m.rotation.y*m.rotation.y+m.rotation.z*m.rotation.z+m.rotation.w*m.rotation.w;
    return std::isfinite(m.inverseMass)&&m.inverseMass>1e-9&&m.inverseMass<1000&&Finite(m.inverseInertia)&&
        m.inverseInertia.x>1e-12&&m.inverseInertia.y>1e-12&&m.inverseInertia.z>1e-12&&
        m.inverseInertia.x<1e6&&m.inverseInertia.y<1e6&&m.inverseInertia.z<1e6&&std::isfinite(n)&&std::fabs(n-1)<0.01;
}
inline Vec AngularDelta(const Motion& m,Vec impulse) {
    Vec p=Unrotate(m.rotation,impulse);
    return Rotate(m.rotation,{p.x*m.inverseInertia.x,p.y*m.inverseInertia.y,p.z*m.inverseInertia.z});
}
inline Vec AngularImpulse(const Motion& m,Vec acceleration,double dt) {
    const Vec p=Unrotate(m.rotation,acceleration);
    return Rotate(m.rotation,{p.x/m.inverseInertia.x,p.y/m.inverseInertia.y,p.z/m.inverseInertia.z})*dt;
}
struct Body {
    Motion motion;
    Vec right{1,0,0}, forward{0,1,0}, up{0,0,1}, velocity{}, spin{};
};
inline bool Valid(const Body& b) {
    return Valid(b.motion)&&Finite(b.velocity)&&Finite(b.spin)&&Length(b.velocity)<350&&Length(b.spin)<100&&
        Finite(b.right)&&Finite(b.forward)&&Finite(b.up)&&std::fabs(Dot(b.right,b.right)-1)<0.05&&
        std::fabs(Dot(b.forward,b.forward)-1)<0.05&&std::fabs(Dot(b.up,b.up)-1)<0.05&&
        std::fabs(Dot(b.right,b.forward))<0.05&&std::fabs(Dot(b.up,b.forward))<0.05&&std::fabs(Dot(b.up,b.right))<0.05;
}
struct Command {
    Vec rates{};             // desired angular rates in world axes, supplied by body-axis pilot controls
    double forwardAcceleration=0, manualLift=0, verticalSpeed=0;
    double brakeAcceleration=30, speedCap=60, radius=2.5;
    bool assist=true, brake=false;
    // AI pilots: a commanded push along the body's right (strafe), and the side thrusters' bound for slip damping.
    double strafeAcceleration=0, sideAcceleration=3;
};
struct State {
    double referenceMass=0;  // fixed thruster specification for this flight/body; payload changes do not create free thrust
    double trim=9.8;         // bounded collective trim; assist integrates vertical-speed error, never per-step "gravity"
};
struct Output { Vec linear{}, angular{}; double collective=0; bool saturated=false, valid=false; };
inline Output Solve(const Body& b,const Command& c,State& state,double dt) {
    Output out;
    if (!Valid(b)||!Finite(c.rates)||!std::isfinite(c.forwardAcceleration)||!std::isfinite(c.manualLift)||
        !std::isfinite(c.verticalSpeed)||!std::isfinite(c.brakeAcceleration)||!std::isfinite(c.speedCap)||!std::isfinite(c.radius)||
        !std::isfinite(c.strafeAcceleration)||!std::isfinite(c.sideAcceleration)||c.sideAcceleration<0||
        !(dt>0&&dt<=0.25)||c.brakeAcceleration<0||c.speedCap<0||c.radius<=0) return out;
    if (!state.referenceMass) state.referenceMass=1/b.motion.inverseMass;
    if (!std::isfinite(state.referenceMass)||state.referenceMass<=0||!std::isfinite(state.trim)) return out;
    constexpr double maxCollective=35, manualCollective=8, verticalGain=4, integralGain=1.5;
    state.trim=std::clamp(state.trim,0.0,30.0);
    const double error=std::clamp(c.verticalSpeed-b.velocity.z,-15.0,15.0);
    const double manual=std::clamp(c.manualLift,-1.0,1.0);
    double collective=manual<0?state.trim*(1+manual):state.trim+manual*manualCollective;
    if (c.assist&&b.up.z>0.35) {
        const double request=(state.trim+verticalGain*error)/b.up.z;
        collective=std::clamp(request,0.0,maxCollective);
        // Do not wind up when actuator limits or a steep/inverted attitude make the requested lift impossible.
        if ((request>0&&request<maxCollective)||(request>=maxCollective&&error<0)||(request<=0&&error>0))
            state.trim=std::clamp(state.trim+integralGain*error*dt,0.0,30.0);
        out.saturated=request<=0||request>=maxCollective;
    }
    collective=std::clamp(collective,0.0,maxCollective);
    const double along=Dot(b.velocity,b.forward);
    double forward=std::clamp(c.forwardAcceleration,-45.0,45.0);
    if (c.brake) forward=-std::clamp(along*2,-c.brakeAcceleration,c.brakeAcceleration);
    else if (c.assist&&std::hypot(b.velocity.x,b.velocity.y)>c.speedCap)
        forward=std::min(forward,-std::clamp(along,0.0,c.brakeAcceleration));
    Vec acceleration=b.forward*forward+b.up*collective;
    const double side=std::min(c.sideAcceleration,12.0);
    if (c.strafeAcceleration!=0) acceleration=acceleration+b.right*std::clamp(c.strafeAcceleration,-12.0,12.0);
    else if (c.assist) acceleration=acceleration+b.right*(-std::clamp(Dot(b.velocity,b.right)*1.2,-side,side)); // bounded side thrusters
    const Vec force=Limited(acceleration,65)*state.referenceMass;
    // Assist is an actuator controller, not an angular-velocity overwrite. Off: zero command applies zero torque.
    const Vec angularAcceleration=Limited((c.assist?c.rates-b.spin:c.rates)*5,6);
    const Vec angular=AngularImpulse(b.motion,angularAcceleration,dt);
    const double torqueBudget=state.referenceMass*std::max(0.25,c.radius*c.radius)*6;
    out.linear=force*dt;
    out.angular=Limited(angular,torqueBudget*dt);
    out.collective=collective;
    out.valid=Finite(out.linear)&&Finite(out.angular);
    return out;
}
}
