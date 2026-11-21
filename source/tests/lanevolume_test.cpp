#include "lanevolume.h"
#include "routecurve.h"
#include <cstdio>
#include <map>
#include <limits>

using namespace lanevolume;
int main() {
    int checks=0,failed=0;
    auto check=[&](bool ok,const char* message) { ++checks;if(!ok) { ++failed;std::printf("FAIL %s\n",message); } };
    auto near=[](double a,double b) { return std::fabs(a-b)<1e-7; };
    auto watertight=[](const Mesh& m) {
        std::map<std::pair<std::size_t,std::size_t>,int> edges;
        for(auto t:m.triangles) for(int k=0;k<3;++k) ++edges[std::minmax(t[k],t[(k+1)%3])];
        for(auto [e,n]:edges) if(n!=2) return false;
        return !edges.empty();
    };
    auto volume=[](const Mesh& m) {
        double sum=0;for(auto t:m.triangles) sum+=routemap::Dot(m.vertices[t[0]],routemap::Cross(m.vertices[t[1]],m.vertices[t[2]]))/6;
        return sum;
    };
    Profile p;p.width=4;p.height=3;
    auto box=Build({{{0,0,0},p},{{0,10,0},p}},false);
    check(bool(box)&&watertight(box),"straight road envelope is closed");
    check(near(volume(box),120),"outward faces enclose width times height times length");
    check(near(box.vertices[0].x,2)&&near(box.vertices[0].z,0),"road floor stays at authored centre height");
    check(near(box.vertices[1].z,3),"clearance is above road floor");
    auto wide=p;wide.width=8;wide.height=6;
    auto taper=Build({{{0,0,0},p},{{0,10,0},wide}},false);
    check(bool(taper)&&watertight(taper),"loft remains watertight when width and clearance change");
    check(near(volume(taper),280),"linear two-axis taper has expected frustum volume");
    auto trapezoid=p;trapezoid.shape=Shape::Trapezoid;trapezoid.topWidth=2;
    const auto sloped=Build({{{0,0,0},trapezoid},{{0,10,0},trapezoid}},false);
    check(bool(sloped)&&watertight(sloped)&&near(volume(sloped),90),"trapezoidal envelope has the expected area and outward faces");
    p.shape=Shape::RoundedRectangle;p.radius=1;
    auto rounded=Build({{{0,0,0},p},{{0,10,0},p}},false,Frame::Surface,16);
    check(bool(rounded)&&watertight(rounded),"rounded cross-section is closed");
    check(std::fabs(volume(rounded)-(12-4+kPi)*10)<.06,"rounded rectangle excludes its cut-off corners");
    auto square=p;square.radius=0;
    auto roundToSquare=Build({{{0,0,0},p},{{0,10,0},square}},false);
    check(bool(roundToSquare)&&watertight(roundToSquare),"rounding can taper continuously to square corners");
    p.shape=Shape::Ellipse;p.width=6;p.height=4;
    auto tube=Build({{{0,0,0},p},{{0,10,0},p}},false,Frame::Surface,16);
    check(bool(tube)&&watertight(tube),"elliptical tube has closed end caps");
    check(std::fabs(volume(tube)-kPi*3*2*10)<.4,"elliptical tube approximates analytic volume");
    auto vertical=Build({{{0,0,0},p},{{0,0,10},p},{{0,1,20},p}},false,Frame::Transport);
    check(bool(vertical)&&watertight(vertical),"hover transport frame passes through a vertical climb");
    for(std::size_t i=0;i<vertical.right.size();++i) {
        check(near(Length(vertical.right[i]),1)&&near(routemap::Dot(vertical.right[i],vertical.up[i]),0),"hover frame stays orthonormal");
    }
    check(!Build({{{0,0,0},p},{{0,0,10},p}},false),"vertical road support is reported invalid");
    check(!Build({{{0,0,0},p},{{0,0,0},p}},false),"duplicate centres do not invent a tangent");
    check(!Build({{{0,0,0},p},{{0,10,0},p},{{0,0,0},p}},false),"180 degree reversal needs an authored turn");
    auto bad=p;bad.width=std::numeric_limits<double>::quiet_NaN();
    check(!Build({{{0,0,0},p},{{0,10,0},bad}},false),"nonfinite profile rejected");
    bad=p;bad.shape=Shape::RoundedRectangle;bad.radius=9;
    check(!Valid(bad),"oversized corner radius is reported rather than silently changing data");
    check(!Build({{{-1e308,0,0},p},{{1e308,0,0},p}},false),"finite input whose arithmetic overflows produces a diagnostic");
    auto bank=p;bank.shape=Shape::Rectangle;bank.width=4;bank.height=3;bank.bank=kPi*.5;
    const auto banked=Build({{{0,0,0},bank},{{0,10,0},bank}},false);
    check(bool(banked)&&near(volume(banked),120),"bank rotates the envelope without changing its volume");
    const std::vector<trafficdata::Vec> controls{{0,0,0},{0,30,0},{30,30,0}};
    const auto curve=routecurve::Build(controls,{0,8,0},false);
    std::vector<Station> stations;
    p.shape=Shape::Rectangle;p.width=2;p.height=3;
    for(auto c:curve.points) stations.push_back({{c.x,c.y,c.z},p});
    const auto bent=Build(stations,false);
    check(bool(bent)&&stations.size()>3&&watertight(bent),"volume sweeps the existing rounded route, with no replacement spine");
    check(near(curve.authored[0],0)&&controls[1].y==30,"sweep leaves control identities and authored points intact");
    stations.clear();
    for(int i=0;i<64;++i) {const double a=2*kPi*i/64;stations.push_back({{30*std::cos(a),30*std::sin(a),3*std::sin(a*2)},p});}
    const auto loop=Build(stations,true,Frame::Transport);
    check(bool(loop)&&watertight(loop),"nonplanar closed sweep has a connected seam and no end caps");
    check(loop.triangles.size()==stations.size()*8,"closed rectangular sweep contains only wall faces");
    // A width below the bend radius has no inverted inner surface.
    check(volume(loop)>0,"loop retains outward winding");
    std::printf("lanevolume: %d/%d checks passed\n",checks-failed,checks);
    return failed?1:0;
}
