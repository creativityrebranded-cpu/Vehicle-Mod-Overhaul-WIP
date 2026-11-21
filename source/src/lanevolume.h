#pragma once
// Mod-owned corridor geometry. Input centres are already rounded by the route
// evaluator; this module never moves them or asks the engine for geometry.
#include "routemap.h"
#include <array>
#include <string>

namespace lanevolume {
using routemap::Vec3;
using routemap::Vec2;
constexpr double kPi = 3.14159265358979323846;
enum class Shape { Rectangle, RoundedRectangle, Ellipse, Trapezoid, Polygon };
enum class Frame { Surface, Transport };
struct Profile {
    Shape shape = Shape::Rectangle;
    double width = 3.5, height = 4.0, radius = 0, topWidth = 3.5;
    std::vector<Vec2> polygon; // normalized x [-.5,.5], y [0,1], simple perimeter
    double offset = 0, elevation = 0, bank = 0; // bank in radians
};
struct Station { Vec3 centre; Profile profile; };
struct Mesh {
    std::vector<Vec3> vertices;
    std::vector<std::array<std::size_t, 3>> triangles;
    std::vector<Vec3> right, up;
    std::vector<std::size_t> faceStation;
    std::size_t sides = 0;
    std::string error;
    explicit operator bool() const { return error.empty() && !triangles.empty(); }
};
inline Vec3 Add(Vec3 a, Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
inline Vec3 Sub(Vec3 a, Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
inline Vec3 Mul(Vec3 a, double b) { return {a.x*b,a.y*b,a.z*b}; }
inline double Length(Vec3 a) { return std::sqrt(routemap::Dot(a,a)); }
inline Vec3 Unit(Vec3 a) { const auto n=Length(a); return n>1e-10?Mul(a,1/n):Vec3{}; }
inline double Cross2(Vec2 a,Vec2 b,Vec2 c){return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);}
inline double Area(const std::vector<Vec2>& points){double result=0;for(std::size_t i=0;i<points.size();++i){const auto a=points[i],b=points[(i+1)%points.size()];result+=a.x*b.y-a.y*b.x;}return result*.5;}
inline bool OnEdge(Vec2 a,Vec2 b,Vec2 p){return std::fabs(Cross2(a,b,p))<1e-9&&p.x>=std::min(a.x,b.x)-1e-9&&p.x<=std::max(a.x,b.x)+1e-9&&p.y>=std::min(a.y,b.y)-1e-9&&p.y<=std::max(a.y,b.y)+1e-9;}
inline bool Intersect(Vec2 a,Vec2 b,Vec2 c,Vec2 d){const double abC=Cross2(a,b,c),abD=Cross2(a,b,d),cdA=Cross2(c,d,a),cdB=Cross2(c,d,b);return ((abC>1e-9&&abD< -1e-9)||(abC< -1e-9&&abD>1e-9))&&((cdA>1e-9&&cdB< -1e-9)||(cdA< -1e-9&&cdB>1e-9))||OnEdge(a,b,c)||OnEdge(a,b,d)||OnEdge(c,d,a)||OnEdge(c,d,b);}
inline bool SimplePolygon(const std::vector<Vec2>& points){if(points.size()<3||points.size()>64)return false;for(std::size_t i=0;i<points.size();++i){const auto p=points[i],q=points[(i+1)%points.size()];if(!std::isfinite(p.x)||!std::isfinite(p.y)||p.x<-.50000001||p.x>.50000001||p.y< -1e-8||p.y>1.00000001||std::hypot(p.x-q.x,p.y-q.y)<1e-8)return false;for(std::size_t j=i+1;j<points.size();++j){if(j==i+1||(i==0&&j+1==points.size()))continue;if(Intersect(p,q,points[j],points[(j+1)%points.size()]))return false;}}return std::fabs(Area(points))>1e-8;}
inline bool Inside(const std::vector<Vec2>& polygon,Vec2 p){bool inside=false;for(std::size_t i=0,j=polygon.size()-1;i<polygon.size();j=i++){const auto a=polygon[j],b=polygon[i];if(OnEdge(a,b,p))return true;if((a.y>p.y)!=(b.y>p.y)&&p.x<(b.x-a.x)*(p.y-a.y)/(b.y-a.y)+a.x)inside=!inside;}return inside;}
inline bool Valid(const Profile& p) {
    const double values[]{p.width,p.height,p.radius,p.topWidth,p.offset,p.elevation,p.bank};
    for (auto v:values) if (!std::isfinite(v)) return false;
    if (!(p.width>0 && p.height>0) || p.radius<0) return false;
    switch(p.shape) {
    case Shape::Rectangle: case Shape::Ellipse: return true;
    case Shape::RoundedRectangle: return p.radius<=std::min(p.width,p.height)*0.5;
    case Shape::Trapezoid: return p.topWidth>0;
    case Shape::Polygon:return SimplePolygon(p.polygon);
    }
    return false;
}
inline bool Contains(const Profile& p,double across,double aboveFloor) {
    if(!Valid(p)||!std::isfinite(across)||!std::isfinite(aboveFloor))return false;
    const double x=std::fabs(across),y=std::fabs(aboveFloor-p.height*.5),w=p.width*.5,h=p.height*.5;
    if(aboveFloor<0||aboveFloor>p.height)return false;
    if(p.shape==Shape::Polygon)return Inside(p.polygon,{across/p.width,aboveFloor/p.height});
    if(p.shape==Shape::Ellipse)return x*x/(w*w)+y*y/(h*h)<=1+1e-10;
    if(p.shape==Shape::Trapezoid)return x<=(p.width+(p.topWidth-p.width)*aboveFloor/p.height)*.5+1e-10;
    if(p.shape==Shape::RoundedRectangle){const double dx=std::max(0.0,x-w+p.radius),dy=std::max(0.0,y-h+p.radius);return x<=w+1e-10&&y<=h+1e-10&&dx*dx+dy*dy<=p.radius*p.radius+1e-10;}
    return x<=w+1e-10;
}
// Fixed corner parameters keep loft topology stable. A shape change is an
// explicit profile boundary, not a silent interpolation between unrelated solids.
inline std::vector<Vec2> Section(const Profile& p, int quarterSteps=4) {
    std::vector<Vec2> out;
    if (!Valid(p)) return out;
    if(p.shape==Shape::Polygon){auto points=p.polygon;if(Area(points)<0)std::reverse(points.begin()+1,points.end());for(auto& point:points){point.x*=p.width;point.y*=p.height;}return points;}
    quarterSteps=std::clamp(quarterSteps,1,16);
    const double w=p.width*.5,h=p.height*.5;
    for (int corner=0;corner<4;++corner) {
        for (int j=0;j<=quarterSteps;++j) {
            const double a=(corner+double(j)/quarterSteps)*kPi*.5;
            Vec2 v;
            if (p.shape==Shape::Ellipse) v={w*std::cos(a),h*std::sin(a)};
            else {
                const double r=p.shape==Shape::RoundedRectangle?p.radius:0;
                // Each quadrant sweeps the matching corner from one edge to the next.
                const double cornerX=corner==0||corner==3?1:-1;
                const double cornerY=corner<2?1:-1;
                const double rw=p.shape==Shape::Trapezoid?(cornerY>0?p.topWidth*.5:w):w;
                v={cornerX*(rw-r)+r*std::cos(a),cornerY*(h-r)+r*std::sin(a)};
            }
            // Rectangular sections use four exact corners. Rounded sections
            // retain corner slots so a loft may reach zero corner radius.
            v.y+=h;
            out.push_back(v);
        }
    }
    if (p.shape==Shape::Rectangle || p.shape==Shape::Trapezoid) {
        const double t=p.shape==Shape::Trapezoid?p.topWidth*.5:w;
        return {{w,0},{t,p.height},{-t,p.height},{-w,0}};
    }
    // Preserve corner slots when rounding shrinks to zero during a loft.
    // Build welds coincident vertices and omits zero-area faces.
    if(p.shape==Shape::RoundedRectangle) return out;
    // Adjacent ellipse quadrants share an endpoint; remove only those duplicates.
    std::vector<Vec2> unique;
    for (auto v:out) if (unique.empty() || std::hypot(v.x-unique.back().x,v.y-unique.back().y)>1e-10) unique.push_back(v);
    if (unique.size()>1 && std::hypot(unique.front().x-unique.back().x,unique.front().y-unique.back().y)<1e-10) unique.pop_back();
    return unique;
}
// Rodrigues transport avoids world-up frame flips during vertical hover paths.
inline Vec3 Transport(Vec3 v,Vec3 from,Vec3 to) {
    const auto axis=routemap::Cross(from,to);const double s=Length(axis),c=std::clamp(routemap::Dot(from,to),-1.0,1.0);
    if(s<1e-9) return v; // exact reversals are rejected by Build
    const auto n=Mul(axis,1/s);
    return Add(Add(Mul(v,c),Mul(routemap::Cross(n,v),s)),Mul(n,routemap::Dot(n,v)*(1-c)));
}
inline std::vector<std::array<std::size_t,3>> Cap(const std::vector<Vec2>& polygon){std::vector<std::array<std::size_t,3>> out;std::vector<std::size_t> indices;for(std::size_t i=0;i<polygon.size();++i)indices.push_back(i);std::size_t budget=polygon.size()*polygon.size();while(indices.size()>2&&budget--){bool clipped=false;for(std::size_t k=0;k<indices.size();++k){const auto a=indices[(k+indices.size()-1)%indices.size()],b=indices[k],c=indices[(k+1)%indices.size()];const double cross=Cross2(polygon[a],polygon[b],polygon[c]);if(std::fabs(cross)<1e-10){indices.erase(indices.begin()+k);clipped=true;break;}if(cross<0)continue;bool occupied=false;for(auto p:indices)if(p!=a&&p!=b&&p!=c&&Cross2(polygon[a],polygon[b],polygon[p])>=-1e-9&&Cross2(polygon[b],polygon[c],polygon[p])>=-1e-9&&Cross2(polygon[c],polygon[a],polygon[p])>=-1e-9)occupied=true;if(occupied)continue;out.push_back({a,b,c});indices.erase(indices.begin()+k);clipped=true;break;}if(!clipped)return {};}return out;}
struct Basis {std::vector<Vec3> right,up;std::string error;};
inline Basis Frames(const std::vector<Vec3>& points,bool closed,Frame frame=Frame::Transport){Basis out;const auto n=points.size();if(n<(closed?3u:2u)){out.error="too few frame points";return out;}std::vector<Vec3> tangent;for(std::size_t i=0;i<n;++i){const auto prev=i?i-1:(closed?n-1:0),next=i+1<n?i+1:(closed?0:n-1);const auto a=Sub(points[i],points[prev]),b=Sub(points[next],points[i]);const auto t=Unit(Add(Unit(a),Unit(b)));if(!routemap::Finite(points[i])||Length(t)<.5){out.error="invalid or reversing tangent";return out;}tangent.push_back(t);}
    Vec3 right;
    for(std::size_t i=0;i<n;++i) {
        const auto t=tangent[i];
        if(frame==Frame::Surface || i==0) {
            right=Unit(routemap::Cross(t,{0,0,1}));
            if(Length(right)<.5) {
                if(frame==Frame::Surface) {out.error="vertical surface section";return out;}
                right=Unit(routemap::Cross(t,{0,1,0}));
            }
        } else right=Unit(Transport(right,tangent[i-1],t));
        out.right.push_back(right);
        out.up.push_back(Unit(routemap::Cross(right,t)));
    }
    // Distribute closed-loop transport holonomy by distance. The closing edge
    // receives its share too; there is no one-seam twist or duplicated end cap.
    if(closed && frame==Frame::Transport) {
        const auto finish=Transport(out.right.back(),tangent.back(),tangent.front());
        const double twist=std::atan2(routemap::Dot(tangent.front(),routemap::Cross(finish,out.right.front())),routemap::Dot(finish,out.right.front()));
        double total=0;std::vector<double> distance(n);
        for(std::size_t i=1;i<n;++i) { total+=Length(Sub(points[i],points[i-1]));distance[i]=total; }
        total+=Length(Sub(points.front(),points.back()));
        for(std::size_t i=1;i<n;++i) {
            const double a=twist*distance[i]/total;
            out.right[i]=Add(Mul(out.right[i],std::cos(a)),Mul(routemap::Cross(tangent[i],out.right[i]),std::sin(a)));
            out.up[i]=Unit(routemap::Cross(out.right[i],tangent[i]));
        }
    }
return out;}
inline Mesh Build(const std::vector<Station>& stations,bool closed,Frame frame=Frame::Surface,int detail=4,const Basis* supplied=nullptr) {
    Mesh out;
    const auto n=stations.size();
    auto fail=[&](const char* reason) { Mesh bad;bad.error=reason;return bad; };
    if(n<(closed?3u:2u)) return fail("too few stations");
    if(n>4096) return fail("station budget exceeded");
    std::vector<std::vector<Vec2>> sections;
    std::vector<Vec3> tangent;
    for(std::size_t i=0;i<n;++i) {
        if(!routemap::Finite(stations[i].centre)||!Valid(stations[i].profile)) return fail("invalid station or profile");
        auto section=Section(stations[i].profile,detail);
        if(!sections.empty() && section.size()!=sections.front().size()) return fail("incompatible loft topology");
        if(i && stations[i].profile.shape!=stations.front().profile.shape) return fail("shape transition requires explicit boundary");
        sections.push_back(std::move(section));
        const auto prev=i?i-1:(closed?n-1:0),next=i+1<n?i+1:(closed?0:n-1);
        const auto a=Sub(stations[i].centre,stations[prev].centre),b=Sub(stations[next].centre,stations[i].centre);
        if((i||closed) && Length(a)<1e-8) return fail("duplicate station");
        if((i+1<n||closed) && Length(b)<1e-8) return fail("duplicate station");
        const auto t=Unit(Add(Unit(a),Unit(b)));
        if(!routemap::Finite(t)||Length(t)<.5) return fail("invalid or reversing tangent");
        tangent.push_back(t);
    }
    std::vector<Vec3> centres;for(const auto& station:stations)centres.push_back(station.centre);const auto basis=supplied?*supplied:Frames(centres,closed,frame);if(!basis.error.empty())return fail(basis.error.c_str());if(basis.right.size()!=n||basis.up.size()!=n)return fail("frame count mismatch");out.right=basis.right;out.up=basis.up;
    out.sides=sections.front().size();
    for(std::size_t i=0;i<n;++i) {
        const auto& p=stations[i].profile;
        const auto r=Add(Mul(out.right[i],std::cos(p.bank)),Mul(out.up[i],std::sin(p.bank)));
        const auto u=Sub(Mul(out.up[i],std::cos(p.bank)),Mul(out.right[i],std::sin(p.bank)));
        for(auto v:sections[i]) {
            const auto vertex=Add(stations[i].centre,Add(Mul(r,v.x+p.offset),Mul(u,v.y+p.elevation)));
            if(!routemap::Finite(vertex)) return fail("envelope coordinate overflow");
            out.vertices.push_back(vertex);
        }
    }
    // Weld zero-radius rounded corners without changing ring indexing.
    std::vector<std::size_t> index(out.vertices.size());
    for(std::size_t i=0;i<n;++i) for(std::size_t j=0;j<out.sides;++j) {
        const auto at=i*out.sides+j;index[at]=at;
        for(std::size_t k=0;k<j;++k) if(Length(Sub(out.vertices[at],out.vertices[i*out.sides+k]))<1e-10) { index[at]=index[i*out.sides+k];break; }
    }
    std::size_t faceOwner=0;
    auto triangle=[&](std::size_t a,std::size_t b,std::size_t c) {
        a=index[a];b=index[b];c=index[c];
        if(Length(routemap::Cross(Sub(out.vertices[b],out.vertices[a]),Sub(out.vertices[c],out.vertices[a])))>1e-12) {out.triangles.push_back({a,b,c});out.faceStation.push_back(faceOwner);}
    };
    const auto sides=out.sides;
    for(std::size_t i=0;i<(closed?n:n-1);++i) for(std::size_t j=0;j<sides;++j) {
        faceOwner=i;const auto a=i*sides+j,b=i*sides+(j+1)%sides,c=((i+1)%n)*sides+j,d=((i+1)%n)*sides+(j+1)%sides;
        triangle(a,c,b);triangle(b,c,d);
    }
    if(!closed){if(stations.front().profile.shape==Shape::Polygon){const auto first=Cap(sections.front()),last=Cap(sections.back());if(first.empty()||last.empty())return fail("cannot triangulate profile caps");faceOwner=0;for(auto t:first)triangle(t[0],t[1],t[2]);const auto end=(n-1)*sides;faceOwner=n-1;for(auto t:last)triangle(end+t[0],end+t[2],end+t[1]);}else for(std::size_t j=1;j+1<sides;++j){faceOwner=0;triangle(0,j,j+1);const auto end=(n-1)*sides;faceOwner=n-1;triangle(end,end+j+1,end+j);}}

    return out;
}
} // namespace lanevolume
