#include "ringlogic.h"
#include <cstdio>

int main() {
    int checks=0,failed=0;
    auto check=[&](bool ok,const char* name) { ++checks; if(!ok) { ++failed; std::printf("FAIL %s\n",name); } };
    auto near=[](double a,double b,double e=1e-6){return std::fabs(a-b)<e;};
    using namespace ringlogic;
    const Ring r=Default(100,200,10);
    check(near(r.z,10+kDefaultHeight)&&near(r.radius,kDefaultRadius)&&Valid(r),"default ring clears the top by the default height");
    const auto n=Exit(r,0),e=Exit(r,1),s=Exit(r,2),w=Exit(r,3);
    check(near(n.x,100)&&near(n.y,200+600),"exit 0 is due north of the centre");
    check(near(e.x,700)&&near(e.y,200),"exit 1 is due east");
    check(near(s.y,200-600)&&near(w.x,100-600),"exits 2 and 3 are south and west");
    check(near(n.heading,kPi/2),"at the north end clockwise traffic heads east");
    check(near(e.heading,kPi)||near(e.heading,-kPi),"at the east end it heads south");
    check(near(Marker(r,0).x,n.x)&&near(Marker(r,6).x,e.x),"markers include the exit points");
    double maxGap=0;for(int i=0;i<kPoints;++i){const auto a=Marker(r,i),b=Marker(r,(i+1)%kPoints);maxGap=std::max(maxGap,std::hypot(a.x-b.x,a.y-b.y));}
    check(near(maxGap,2*600*std::sin(kPi/kPoints),1e-6),"markers are evenly spaced on a circle");

    auto t=Apply(r,Edit::TurnRight,true,0);
    check(near(Exit(t,0).x,100+600*std::sin(kPi/6))&&near(Exit(t,0).y,200+600*std::cos(kPi/6)),"turning right moves the north exit clockwise");
    auto st=Apply(r,Edit::Stretch,true,0);
    check(near(Exit(st,1).x,100+600*1.25)&&near(Exit(st,0).y,200+600),"stretch widens east-west, not north-south");
    check(near(Apply(r,Edit::Wider,false,0).radius,630),"wider grows the radius by 5 percent");
    check(near(Apply(r,Edit::Raise,true,0).z,r.z+25)&&near(Apply(r,Edit::Lower,false,0).z,r.z-5),"raise and lower step");
    auto f=Apply(r,Edit::Forward,false,kPi/2);
    check(near(f.cx,110)&&near(f.cy,200),"forward moves the centre where the player faces (east)");
    auto l=Apply(r,Edit::Left,false,0);
    check(near(l.cx,90)&&near(l.cy,200),"left moves the centre to the player's left");
    Ring big=r;for(int i=0;i<60;++i)big=Apply(big,Edit::Wider,true,0);
    check(near(big.radius,kMaxRadius),"radius is capped");
    Ring small=r;for(int i=0;i<60;++i)small=Apply(small,Edit::Squeeze,true,0);
    check(near(small.aspect,kMinAspect),"aspect is capped");
    Ring back;
    check(Parse(Format(t),back)&&near(back.cx,t.cx,0.1)&&near(back.rotation,t.rotation,0.01)&&near(back.radius,t.radius,0.1),"format and parse round-trip");
    check(!Parse("1 2 3",back)&&!Parse("nan 0 0 600 1 0",back),"bad text is refused");
    // Two parallel rings.
    const auto lo=Marker(r,3,0),hi=Marker(r,3,1);
    check(near(hi.x,lo.x)&&near(hi.y,lo.y)&&near(hi.z,lo.z+kDefaultGap),"the upper ring is straight above the lower by the gap");
    check(near(std::fabs(Wrap(hi.heading-lo.heading)),kPi),"the upper ring is flown the other way");
    check(near(Exit(r,1,1).z,r.z+r.gap)&&near(Exit(r,1,1).x,Exit(r,1,0).x),"both rings share the exit points");
    check(near(Apply(r,Edit::GapUp,true,0).gap,kDefaultGap+25)&&near(Apply(r,Edit::GapDown,false,0).gap,kDefaultGap-5),"the gap steps like the height");
    Ring g=r;for(int i=0;i<60;++i)g=Apply(g,Edit::GapDown,true,0);
    check(near(g.gap,kMinGap),"the gap is capped");
    Ring old;
    check(Parse("1 2 3 600 1 0",old)&&near(old.gap,kDefaultGap),"a one-ring line gets the default gap");
    std::printf("ringlogic: %d checks, %d failures\n",checks,failed);
    return failed?1:0;
}
