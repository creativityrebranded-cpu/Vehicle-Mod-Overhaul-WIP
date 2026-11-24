#pragma once
// Trips along an FVO traffic route (trafficdata.h), 2026-10-28; replaces ringtrip.h's one-lap ring trip. A route is two lanes
// sampled as lines with one shared position u (point index + fraction): lane 0 travels with increasing u, lane 1 against it.
// A ring's lane 1 is lane_gap higher; a path's lanes sit width/2 to either side. Legs:
//   Climb   straight up over the start to the lane's height;
//   Join    across to the nearest point of the lane;
//   Follow  along the lane (a point kLead ahead) to the target: the exit nearest the destination, or home after a full lap;
//   Leave   across to above the destination at lane height;
//   Done    the caller lands and parks there.
// Pure: tests/trafficroute_test.cpp.
#include "trafficdata.h"
#include "routecurve.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <memory>

namespace trafficroute {
using trafficdata::Vec;
constexpr int kRingSamples = 96;
constexpr double kLead = 70.0, kReach = 25.0, kClimbSlack = 8.0, kClimbLean = 20.0;
class LaneSamples {
    using Storage=std::vector<std::vector<Vec>>;
    std::shared_ptr<Storage> data_=std::make_shared<Storage>(2);
    void Write(){if(data_.use_count()!=1)data_=std::make_shared<Storage>(*data_);}
public:
    std::size_t size()const{return data_->size();}
    void resize(std::size_t n){Write();data_->resize(n);}
    const std::vector<Vec>& operator[](std::size_t i)const{return (*data_)[i];}
    std::vector<Vec>& operator[](std::size_t i){Write();return (*data_)[i];}
};

// An edge band is the road's outer boundary offset inward. On the inside of a bend that offset folds over itself once the band is wider
// than the bend's radius, so its reach is limited to 90% of the local radius there (measured on the boundary through the neighbouring
// stations). Straight stretches, outer sides and the ends of an open route keep the full width. Drawing uses it per station.
template <class Stations>
inline double BandReach(const Stations& st, bool closed, std::size_t i, bool rightSide, double width) {
    const std::size_t n = st.size();
    if (n < 3 || width <= 0) return width;
    const bool end = !closed && (i == 0 || i + 1 >= n);
    if (end) return width;
    const std::size_t p = (i + n - 1) % n, q = (i + 1) % n;
    auto at = [&](std::size_t k) { const auto& v = rightSide ? st[k].boundaries.front() : st[k].boundaries.back(); return std::pair<double, double>{ v.x, v.y }; };
    const auto a = at(p), b = at(i), c = at(q);
    const double x1 = b.first - a.first, y1 = b.second - a.second, x2 = c.first - b.first, y2 = c.second - b.second;
    const double l1 = std::hypot(x1, y1), l2 = std::hypot(x2, y2);
    if (l1 < 1e-6 || l2 < 1e-6) return width;
    const double cross = x1 * y2 - y1 * x2, dot = x1 * x2 + y1 * y2, angle = std::fabs(std::atan2(cross, dot));
    if (angle < 1e-4) return width;
    const double radius = (l1 + l2) * .5 / angle;
    // the band's inward direction (right boundary -> left boundary), and whether that is the side the road bends toward
    const double ux = st[i].left.x - st[i].right.x, uy = st[i].left.y - st[i].right.y, ul = std::hypot(ux, uy);
    if (ul < 1e-9) return width;
    const double dx = (rightSide ? ux : -ux) / ul, dy = (rightSide ? uy : -uy) / ul;
    const double leftNormalX = -(y1 + y2), leftNormalY = x1 + x2;  // left of travel along the boundary
    const double towardCentre = (dx * leftNormalX + dy * leftNormalY) * (cross > 0 ? 1 : -1);
    return towardCentre > 0 ? std::min(width, radius * .9) : width;
}
struct Line {
    // primitive: this station's cross-section; blend: inside a taper join the previous tile's too (both must fit).
    struct RoadSection {Vec left,right,divider,top;std::vector<Vec> boundaries,tops;roadplan::Primitive primitive{roadplan::Flat,0},blend{roadplan::Default,0};double blendEnd=0;};  // blend applies from this station to blendEnd (0..1) of the cell
    std::vector<RoadSection> road;
    std::vector<double> direction;  // road lanes: 1 along the markers, -1 against; empty = lane parity
    double edgeLeft=0,edgeRight=0;  // no-go bands along the road edges (m): outside every lane (0 = none). Right = the first boundary side.
    double median=0;                // red divider width (m): the band either side of an interior divider is outside every lane
    bool closed = true;
    std::string routeName;
    const char* error=nullptr;
    LaneSamples lane;
    std::vector<int> lanes; // empty retains legacy one/two-way interpretation
    laneplan::Plan plan;
    splinekit::Plan space;
    std::vector<double> source;
    std::array<lanevolume::Basis,16> basis;
    std::string detailError;
    int sourceCount=0;
    std::vector<double> distance;
    std::vector<double> authored;
    // LB-1 (2026-11-12): stretches of a lane that folded at a tight corner and were cut straight (F7 lists them as "lane cut at a corner").
    struct Squeeze { int lane = 0, from = 0, to = 0; Vec at{}; double worstDeg = 0; };
    std::vector<Squeeze> squeezed;
    // Pre-baked uniform-arclength lane table (2026-11-23): built once per route load/edit, shared copy-on-write via shared_ptr.
    // Corridor envelope (2026-11-24): [minLat, maxLat] in metres from lane centre (+ = right of travel, - = left) from
    // authored yellow boxes minus route hazard boxes, baked once per station so spawn/rail/plan share one boundary.
    struct BakedSample {
        double u = 0, s = 0;
        Vec pos{};
        double curvature = 0, grade = 0;
        double minLat = -6.0, maxLat = 6.0;
        bool hazardBlocked = false;
        bool spawnSafe = true;
    };
    struct BakedLane { double stepM = 2.0, totalM = 0.0; std::vector<double> prefixU; std::vector<BakedSample> samples; };
    std::shared_ptr<std::vector<BakedLane>> baked;
    int Count() const { return int(lane[0].size()); }
    double Span() const { return closed ? double(Count()) : double(Count() - 1); }  // u runs [0, Span)
    double ToLine(double source) const {
        if(!this->source.empty()){auto it=std::lower_bound(this->source.begin(),this->source.end(),source);if(it==this->source.begin())return 0;const auto i=std::size_t(it-this->source.begin()-1);const double next=i+1<this->source.size()?this->source[i+1]:double(authored.size());return double(i)+(source-this->source[i])/std::max(1e-12,next-this->source[i]);}
        if(authored.empty())return source;
        const double span=closed?double(authored.size()):double(authored.size()-1);
        source=std::clamp(source,0.0,span);const auto i=std::min(std::size_t(source),authored.size()-1);
        const double end=i+1<authored.size()?authored[i+1]:Span();
        return authored[i]+(end-authored[i])*(source-double(i));
    }
    double ToSource(double u) const {
        if(!source.empty()){u=std::clamp(u,0.0,Span());const auto i=std::min(std::size_t(u),source.size()-1);const double next=i+1<source.size()?source[i+1]:double(authored.size());return source[i]+(next-source[i])*(u-double(i));}
        if(authored.empty())return u;
        u=std::clamp(u,0.0,Span());auto it=std::upper_bound(authored.begin(),authored.end(),u);
        const auto i=it==authored.begin()?0:std::size_t(it-authored.begin()-1);
        const double end=i+1<authored.size()?authored[i+1]:Span();
        return double(i)+(end>authored[i]?(u-authored[i])/(end-authored[i]):0);
    }
};
inline bool Spatial(const Line& l){return l.space.enabled||!l.plan.transforms.records.empty();}
inline double MetresAt(const Line& l,double u) {
    if(l.distance.empty())return 0;
    u=std::clamp(u,0.0,l.Span());const auto i=std::min(std::size_t(u),l.distance.size()-1);
    if(i+1>=l.distance.size())return l.distance.back();
    return l.distance[i]+(l.distance[i+1]-l.distance[i])*(u-double(i));
}
inline laneplan::Settings BaseSettingsAt(const Line& l,double u,int lane,double* centre=nullptr) {
    const int section=l.sourceCount?int(routepolicy::Segment(l.sourceCount,l.closed,l.ToSource(u),0)):-1;
    auto s=laneplan::Resolve(l.plan,section,lane);
    if(centre)*centre=laneplan::Centre(l.plan,section,lane);
    if(section<0||(!l.closed&&section==0)||s.taper<=0||l.distance.empty())return s;
    const int previous=section>0?section-1:l.sourceCount-1;
    const double start=MetresAt(l,l.ToLine(section)),end=MetresAt(l,l.ToLine(section+1));
    const double length=std::max(.001,end-start),at=MetresAt(l,u)-start;
    const double f=std::clamp(at/std::min(s.taper,length),0.0,1.0);
    if(f>=1)return s;
    const auto a=laneplan::Resolve(l.plan,previous,lane);
    auto mix=[&](double before,double after){return before+(after-before)*f;};
    s.profile.width=mix(a.profile.width,s.profile.width);s.profile.height=mix(a.profile.height,s.profile.height);
    s.profile.radius=mix(a.profile.radius,s.profile.radius);s.profile.topWidth=mix(a.profile.topWidth,s.profile.topWidth);
    s.profile.offset=mix(a.profile.offset,s.profile.offset);s.profile.elevation=mix(a.profile.elevation,s.profile.elevation);
    s.profile.bank=mix(a.profile.bank,s.profile.bank);s.gap=mix(a.gap,s.gap);
    if(centre)*centre=mix(laneplan::Centre(l.plan,previous,lane),*centre);
    return s;
}
inline laneplan::Settings SettingsAt(const Line& l,double u,int lane,double* centre=nullptr){auto s=BaseSettingsAt(l,u,lane,centre);if(l.space.enabled)s.profile=splinekit::ProfileAt(l.space,l.ToSource(u),lane,s.profile);return s;}
inline Line Densify(const Line& raw) {
    if(raw.Count()<2)return raw;
    Line out;out.closed=raw.closed;
    const int segments=raw.closed?raw.Count():raw.Count()-1;
    double length=0;for(int i=0;i<segments;++i){const auto a=raw.lane[0][i],b=raw.lane[0][(i+1)%raw.Count()];length+=std::hypot(b.x-a.x,b.y-a.y,b.z-a.z);}
    const double step=std::max(2.0,length/std::max(1,2048-segments));
    std::vector<double> starts;std::vector<int> counts;
    for(int i=0;i<segments;++i){const auto a=raw.lane[0][i],b=raw.lane[0][(i+1)%raw.Count()];const int count=std::max(1,int(std::ceil(std::hypot(b.x-a.x,b.y-a.y,b.z-a.z)/step)));
        starts.push_back(double(out.lane[0].size()));counts.push_back(count);
        for(int j=0;j<count;++j){const double f=double(j)/count;out.lane[0].push_back({a.x+(b.x-a.x)*f,a.y+(b.y-a.y)*f,a.z+(b.z-a.z)*f});}}
    if(!raw.closed){starts.push_back(double(out.lane[0].size()));counts.push_back(0);out.lane[0].push_back(raw.lane[0].back());}
    for(auto u:raw.authored){const auto i=std::min(std::size_t(u),starts.size()-1);out.authored.push_back(starts[i]+counts[i]*(u-double(i)));}
    out.lane[1]=out.lane[0];return out;
}

inline Line Build(const trafficdata::Route& r);
// LB-1 (2026-11-12, lc_ring_road edited to width 20 with corner rounding: the inside lane folded back at the hairpin, kinks of 74-127 deg
// within 1 m). A lane offset wider than a corner's radius folds back on the inside. After the lanes are built, each lane is walked
// against the spine: a step that turns more than kFoldDeg away from the spine's own step, or runs against it, marks a fold; each folded
// stretch is replaced by a straight cut between the good stations either side, spaced by the spine's distance. Station count, indices
// and the spine are unchanged; straight stretches and gentle bends are untouched. Every cut is recorded in Line::squeezed.
inline constexpr double kFoldDeg = 60.0;
inline constexpr int kCutSpread = 2;  // stations either side of a fold that the straight cut also covers
template <class Lanes> inline void UnfoldLanes(Lanes& lanes, const std::vector<int>& which, const std::vector<Vec>& spine, bool closed, std::vector<Line::Squeeze>& out) {
    const int n = int(spine.size()); if (n < 4) return;
    const double cosFold = std::cos(kFoldDeg * 3.14159265358979 / 180.0);
    auto at = [&](int i) { return closed ? ((i % n) + n) % n : std::clamp(i, 0, n - 1); };
    for (int lane : which) {
        if (lane < 0 || lane >= int(lanes.size()) || int(lanes[lane].size()) != n) continue;
        auto& L = lanes[lane];
        for (int pass = 0; pass < 4; ++pass) {
            std::vector<char> bad(n, 0); int anyBad = 0; double worst = 0;
            const int steps = closed ? n : n - 1;
            for (int i = 0; i < steps; ++i) {
                const int j = at(i + 1);
                const double lx = L[j].x - L[i].x, ly = L[j].y - L[i].y, sx = spine[j].x - spine[i].x, sy = spine[j].y - spine[i].y;
                const double ll = std::hypot(lx, ly), sl = std::hypot(sx, sy);
                if (sl < 1e-6) continue;
                const double c = ll < 1e-6 ? -1.0 : (lx * sx + ly * sy) / (ll * sl);
                if (c < cosFold) { bad[i] = bad[j] = 1; ++anyBad; worst = std::max(worst, std::acos(std::clamp(c, -1.0, 1.0)) * 57.29578); }
            }
            if (!anyBad) break;
            for (int i = 0; i < n; ++i) {
                if (!bad[i] || (closed ? bad[at(i - 1)] != 0 : (i > 0 && bad[i - 1] != 0))) continue;  // only at the start of a folded run
                int a = i, b = i; while (b - a < n - 2 && bad[at(b + 1)]) ++b;
                // The cut runs from kCutSpread stations before the fold to kCutSpread after it (where they exist), so it meets the lane softly.
                if (closed) { a -= kCutSpread; b += kCutSpread; } else { a = std::max(1, a - kCutSpread); b = std::min(n - 2, b + kCutSpread); }
                if (b - a > n - 3) continue;
                const int g0 = at(a - 1), g1 = at(b + 1);
                if (!closed && (a - 1 < 0 || b + 1 > n - 1)) continue;   // a fold at an open end has no good station beyond it
                if (bad[g0] || bad[g1]) continue;
                double total = 0; for (int k = a - 1; k <= b; ++k) total += std::hypot(spine[at(k + 1)].x - spine[at(k)].x, spine[at(k + 1)].y - spine[at(k)].y);
                if (total < 1e-6) continue;
                double run = 0; const Vec p0 = L[g0], p1 = L[g1];
                for (int k = a; k <= b; ++k) {
                    run += std::hypot(spine[at(k)].x - spine[at(k - 1)].x, spine[at(k)].y - spine[at(k - 1)].y);
                    const double f = run / total; auto& q = L[at(k)];
                    q.x = p0.x + (p1.x - p0.x) * f; q.y = p0.y + (p1.y - p0.y) * f;  // heights stay the spine's
                }
                out.push_back({ lane, at(a), at(b), L[at((a + b) / 2)], worst });
                i = std::max(i, b);
            }
        }
    }
}
inline Line BuildBase(const trafficdata::Route& r)
{
    Line l;
    l.routeName=r.name;
    if(!roadplan::Valid(r.road)){l.error="unsupported or invalid road tiles";return l;}
    if(r.road.enabled && !r.ring) {
        if(r.points.size()<2||!std::isfinite(r.corridor)||r.corridor<=0){l.error="road needs two points and a positive width";return l;}
        auto source=r;source.road={};source.lanes=1;source.lanePlan={};source.spacePlan={};
        source.rounding.resize(source.points.size());
        // The point's own radius (F7 Corner, Radius) comes first; a tile's Bend join rounds the corners that have none.
        for(std::size_t i=0;i<source.points.size();++i)if(!(source.rounding[i]>0))
            source.rounding[i]=roadplan::At(r.road,(i+source.points.size()-1)%source.points.size()).connector==1?
                std::min(r.corridor,routecurve::CornerLimit(source.points,i,source.closed)):0;
        l=BuildBase(source);l.routeName=r.name;l.sourceCount=int(r.points.size());
        const int roadLanes=r.road.dividers.empty()?std::clamp(r.lanes,1,2):int(r.road.dividers.size())+1;
        l.lanes.clear();for(int lane=0;lane<roadLanes;++lane)l.lanes.push_back(lane);
        l.lane.resize(roadLanes);for(int lane=1;lane<roadLanes;++lane)l.lane[lane]=l.lane[0];
        l.direction.clear();for(int lane=0;lane<roadLanes;++lane)l.direction.push_back(roadplan::Direction(r.road,lane,roadLanes));l.median=r.road.median;l.edgeLeft=r.road.edgeLeft;l.edgeRight=r.road.edgeRight;
        l.plan=r.lanePlan;
        const auto spine=l.lane[0];
        const auto sections=r.points.size()-(r.closed?0:1);
        for(int i=0;i<l.Count();++i){
            const double at=l.ToSource(i);
            const auto section=std::min(std::size_t(at),sections-1);
            const auto tile=roadplan::At(r.road,section);
            const double fraction=std::clamp(at-double(section),0.0,1.0);
            const auto& a=r.points[section];const auto& b=r.points[(section+1)%r.points.size()];
            const double length=std::hypot(b.x-a.x,b.y-a.y);
            if(length<1e-6){l.error="road tile has no horizontal extent";return l;}
            const auto centre=spine[i];
            const double half=r.road.width>0?r.road.width*.5:r.corridor*std::clamp(r.lanes,1,2)*.5;
            const double divider=half*(tile.side+tile.swing*(2*fraction-1));
            const auto previous=spine[i?i-1:(l.closed?l.Count()-1:0)];
            const auto next=spine[i+1<l.Count()?i+1:(l.closed?0:i)];
            auto normal=[](Vec a,Vec b){const double d=std::hypot(b.x-a.x,b.y-a.y);return Vec{d>1e-8?-(b.y-a.y)/d:0,d>1e-8?(b.x-a.x)/d:0,0};};
            auto before=normal(previous,centre),after=normal(centre,next);
            if(!l.closed&&i==0)before=after;if(!l.closed&&i+1==l.Count())after=before;
            const double dot=before.x*after.x+before.y*after.y;
            if(dot<-.95){l.error="road corner reverses; add a turning connector";return l;}
            const double nx=(before.x+after.x)/std::max(.05,1+dot),ny=(before.y+after.y)/std::max(.05,1+dot);
            auto offset=[&](double side,double z=0){return Vec{centre.x+nx*side,centre.y+ny*side,centre.z+z};};
            const double lean=r.road.height*std::tan(tile.tilt*3.141592653589793/180);
            l.road.push_back({offset(half),offset(-half),offset(divider),offset(divider+lean,r.road.height)});
            if(std::fabs(at-std::round(at))<1e-8&&(l.closed||(i>0&&i+1<l.Count()))&&source.rounding[section]==0){
                const auto prior=roadplan::At(r.road,(section+sections-1)%sections);
                const double end=half*(prior.side+prior.swing),start=half*(tile.side-tile.swing);
                auto join=[&](double d1,double d2,double z){
                    const double det=before.x*after.y-before.y*after.x;
                    if(std::fabs(det)<1e-6)return offset((d1+d2)*.5,z);
                    return Vec{centre.x+(d1*after.y-before.y*d2)/det,centre.y+(before.x*d2-d1*after.x)/det,centre.z+z};
                };
                l.road.back().divider=join(end,start,0);
                l.road.back().top=join(end+r.road.height*std::tan(prior.tilt*3.141592653589793/180),start+lean,r.road.height);
            }
            auto& station=l.road.back();
            station.primitive=roadplan::PrimitiveAt(r.road,section);
            if(section>0||r.closed){const auto before=(section+sections-1)%sections;
                if(roadplan::At(r.road,before).connector==3&&fraction<.25){station.blend=roadplan::PrimitiveAt(r.road,before);
                    const double next=i+1<l.Count()?l.ToSource(i+1):at+1;station.blendEnd=next>at+1e-9?std::clamp((double(section)+.25-at)/(next-at),0.0,1.0):1;}}
            station.boundaries.push_back(station.right);
            station.tops.push_back(offset(-half,r.road.height));
            const int dividers=roadLanes-1;
            double previousSide=-half;
            for(int d=0;d<dividers;++d){
                const auto rule=roadplan::DividerAt(r.road,d,section);
                const double side=half*(rule.side+rule.swing*(2*fraction-1));
                const double topSide=side+r.road.height*std::tan(rule.tilt*3.141592653589793/180);
                if(side<previousSide-1e-6||side>half+1e-6||topSide<-half||topSide>half){l.error="road dividers cross or leave field";return l;}
                auto bottom=offset(side),top=offset(topSide,r.road.height);
                if(dividers==1&&r.road.dividers.empty()){bottom=station.divider;top=station.top;}
                station.boundaries.push_back(bottom);station.tops.push_back(top);previousSide=side;
            }
            station.boundaries.push_back(station.left);station.tops.push_back(offset(half,r.road.height));
            for(int lane=0;lane<roadLanes;++lane){
                auto lo=station.boundaries[lane],hi=station.boundaries[lane+1];
                const double gap=r.road.median*.5,len=std::hypot(hi.x-lo.x,hi.y-lo.y);
                if((gap>0||r.road.edgeLeft>0||r.road.edgeRight>0)&&len>1e-6){const double ux=(hi.x-lo.x)/len,uy=(hi.y-lo.y)/len;
                    const double a=std::min(len*.45,(lane>0?gap:r.road.edgeRight)),b=std::min(len*.45,(lane+1<roadLanes?gap:r.road.edgeLeft));
                    lo={lo.x+ux*a,lo.y+uy*a,lo.z};hi={hi.x-ux*b,hi.y-uy*b,hi.z};}
                l.lane[lane][i]={(lo.x+hi.x)*.5,(lo.y+hi.y)*.5,centre.z};
            }
        }
        UnfoldLanes(l.lane,l.lanes,spine,l.closed,l.squeezed);  // LB-1: no lane folds back at a tight corner
        l.distance.push_back(0);for(int i=0;i<int(l.Span());++i){const auto a=spine[i],b=spine[(i+1)%spine.size()];l.distance.push_back(l.distance.back()+std::hypot(b.x-a.x,b.y-a.y,b.z-a.z));}
        return l;
    }
    if(const auto* problem=laneplan::Problem(r.lanePlan)){l.error=problem;l.plan=r.lanePlan;return l;}
    if(r.spacePlan.enabled||!r.spacePlan.unsupported.empty()) {
        l.closed=r.closed;l.plan=r.lanePlan;l.space=r.spacePlan;l.sourceCount=int(r.points.size());
        auto fail=[&](const std::string& why){l.detailError=why;l.error="invalid freeform corridor (see geometry diagnostic)";return l;};
        if(r.ring)return fail("Convert a procedural ring to a point path before enabling the toolkit");
        if(const auto* why=splinekit::Problem(l.space,r.points.size()))return fail(why);
        splinekit::Prepare(l.space,r.closed?double(r.points.size()):double(r.points.size()-1),r.closed);const auto curve=splinekit::Build(splinekit::Points(r.points),l.space,r.closed);if(!curve.error.empty())return fail(curve.error);
        l.source=curve.source;l.authored=curve.authored;for(auto p:curve.points)l.lane[0].push_back({p.x,p.y,p.z});
        l.distance.push_back(0);for(int i=0;i<int(l.Span());++i){const auto a=curve.points[i],b=curve.points[(i+1)%curve.points.size()];l.distance.push_back(l.distance.back()+lanevolume::Length(lanevolume::Sub(b,a)));}
        auto all=r.lanePlan.base;if(!r.lanePlan.enabled){all.forward=1;all.reverse=r.lanes>1?1:0;}
        else for(const auto& rule:r.lanePlan.overrides){auto s=laneplan::Resolve(r.lanePlan,rule.section);all.forward=std::max(all.forward,s.forward);all.reverse=std::max(all.reverse,s.reverse);}
        l.lanes=laneplan::Lanes(all);const int slots=std::max(2,l.lanes.back()+1);std::vector<std::vector<Vec>> generated(slots);const auto frame=lanevolume::Frames(curve.points,r.closed);if(!frame.error.empty())return fail(frame.error);
        for(int lane=0;lane<slots;++lane){const auto independent=std::find_if(l.space.lanes.begin(),l.space.lanes.end(),[&](const auto& p){return p.lane==lane;});for(std::size_t i=0;i<curve.points.size();++i){double offset=0;auto s=SettingsAt(l,double(i),lane,&offset);if(!r.lanePlan.enabled)offset=r.lanes>1?((lane&1)?-1:1)*r.width*.5:0;
            const auto right=lanevolume::Add(lanevolume::Mul(frame.right[i],std::cos(s.profile.bank)),lanevolume::Mul(frame.up[i],std::sin(s.profile.bank)));
            const auto up=lanevolume::Sub(lanevolume::Mul(frame.up[i],std::cos(s.profile.bank)),lanevolume::Mul(frame.right[i],std::sin(s.profile.bank)));
            const auto p=independent==l.space.lanes.end()?lanevolume::Add(curve.points[i],lanevolume::Mul(right,offset)):splinekit::Evaluate(independent->anchors,r.closed,curve.source[i]);
            const auto q=lanevolume::Add(p,lanevolume::Add(lanevolume::Mul(right,s.profile.offset),lanevolume::Mul(up,s.profile.elevation)));generated[lane].push_back({q.x,q.y,q.z});}
            l.basis[lane]=lanevolume::Frames(splinekit::Points(generated[lane]),r.closed);if(!l.basis[lane].error.empty())return fail(l.basis[lane].error);
            for(std::size_t i=0;i<curve.points.size();++i)if(!lanevolume::Valid(SettingsAt(l,double(i),lane).profile))return fail("Loft outline self-intersects; adjust the profile stations");
        }
        l.lane.resize(slots);for(int lane=0;lane<slots;++lane)l.lane[lane]=std::move(generated[lane]);return l;
    }
    if(r.lanePlan.enabled) {
        auto legacy=r;legacy.lanePlan={};legacy.lanes=1;
        const auto spine=Densify(Build(legacy));l.closed=spine.closed;l.authored=spine.authored;l.plan=r.lanePlan;l.sourceCount=int(r.points.size());
        l.lane[0]=spine.lane[0];
        l.distance.push_back(0);for(int i=0;i<int(spine.Span());++i){const auto a=spine.lane[0][i],b=spine.lane[0][(i+1)%spine.Count()];l.distance.push_back(l.distance.back()+std::hypot(b.x-a.x,b.y-a.y,b.z-a.z));}
        // The reference samples are used only for source/distance mapping below.
        // Replace them with the derived lanes after all section queries.
        std::vector<std::vector<Vec>> generated(laneplan::kMaxLanes);
        l.lane.resize(laneplan::kMaxLanes);
        // Keep stable even/odd lane identities across count edits. Include lanes
        // present in any section; the provider gates sections where one ends.
        int forward=r.lanePlan.base.forward,reverse=r.lanePlan.base.reverse;
        for(const auto& rule:r.lanePlan.overrides) {const auto s=laneplan::Resolve(r.lanePlan,rule.section);forward=std::max(forward,s.forward);reverse=std::max(reverse,s.reverse);}
        auto all=r.lanePlan.base;all.forward=forward;all.reverse=reverse;l.lanes=laneplan::Lanes(all);
        const int laneSlots=std::max(2,l.lanes.back()+1);generated.resize(laneSlots);l.lane.resize(laneSlots);
        for(int i=0;i<spine.Count();++i) {
            const auto p=spine.lane[0][i];
            const auto a=spine.lane[0][i?i-1:(l.closed?spine.Count()-1:0)];
            const auto b=spine.lane[0][i+1<spine.Count()?i+1:(l.closed?0:i)];
            const double length=std::hypot(b.x-a.x,b.y-a.y),rx=length>1e-8?(b.y-a.y)/length:1,ry=length>1e-8?-(b.x-a.x)/length:0;
            for(int lane=0;lane<laneSlots;++lane) {
                double offset=0;const auto s=SettingsAt(l,double(i),lane,&offset);
                const double flat=offset*std::cos(s.profile.bank);
                generated[lane].push_back({p.x+rx*flat,p.y+ry*flat,p.z+s.profile.elevation+offset*std::sin(s.profile.bank)});
            }
        }
        for(int lane=0;lane<laneSlots;++lane)l.lane[lane]=std::move(generated[lane]);
        return l;
    }
    if (r.ring) {
        l.closed = true;
        const double toward = r.tiltToward * ringlogic::kPi / 180.0, slope = std::tan(r.tilt * ringlogic::kPi / 180.0);
        for (int i = 0; i < kRingSamples; ++i) {
            const auto p = ringlogic::At(r.shape, 2.0 * ringlogic::kPi * i / kRingSamples);
            // slanted: higher toward `tiltToward`, by the distance from the centre along that direction
            const double z = p.z + slope * ((p.x - r.shape.cx) * std::sin(toward) + (p.y - r.shape.cy) * std::cos(toward));
            l.lane[0].push_back({ p.x, p.y, z });
            l.lane[1].push_back({ p.x, p.y, z + r.shape.gap });
        }
        return l;
    }
    l.closed = r.closed;
    const auto curve=routecurve::Build(r.points,r.rounding,r.closed);l.authored=curve.authored;
    const auto& pts = curve.points;
    const std::size_t n = pts.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Vec& a = pts[i > 0 ? i - 1 : (l.closed ? n - 1 : i)];
        const Vec& b = pts[i + 1 < n ? i + 1 : (l.closed ? 0 : i)];
        double dx = b.x - a.x, dy = b.y - a.y;
        const double run = std::hypot(dx, dy);
        if (run > 1e-6) { dx /= run; dy /= run; }
        const double side = r.lanes > 1 ? r.width * 0.5 : 0.0;  // lane 0 on the right of travel (x right, y forward)
        l.lane[0].push_back({ pts[i].x + dy * side, pts[i].y - dx * side, pts[i].z });
        l.lane[1].push_back({ pts[i].x - dy * side, pts[i].y + dx * side, pts[i].z });
    }
    if (r.lanes > 1) { const std::vector<Vec> spine(pts.begin(), pts.end()); UnfoldLanes(l.lane, std::vector<int>{ 0, 1 }, spine, l.closed, l.squeezed); }  // LB-1
    return l;
}

inline double Direction(const Line& l,int lane);
inline void BakeLanes(Line& l, double stepM = 2.0, const trafficdata::Route* route = nullptr) {
    if (l.error || l.Count() < 2 || !(stepM > 0.1)) return;
    auto table = std::make_shared<std::vector<Line::BakedLane>>(l.lane.size());
    const int n = l.Count();
    const int segs = l.closed ? n : n - 1;
    const int routeTiles = (route && !route->ring && route->points.size() >= 2)
        ? int(route->points.size()) - (route->closed ? 0 : 1) : 0;
    auto routeCentreAt = [&](double u) -> Vec {
        Vec m{}; int cnt = 0;
        const int seg = std::clamp(int(std::floor(u)), 0, std::max(0, segs - 1));
        const double f = std::clamp(u - double(seg), 0.0, 1.0);
        const int nxt = l.closed ? (seg + 1) % n : std::min(n - 1, seg + 1);
        for (std::size_t li = 0; li < l.lane.size(); ++li) {
            const auto& lp = l.lane[li];
            if (int(lp.size()) < n) continue;
            const auto& a = lp[std::size_t(seg)];
            const auto& b = lp[std::size_t(nxt)];
            m.x += a.x + (b.x - a.x) * f;
            m.y += a.y + (b.y - a.y) * f;
            m.z += a.z + (b.z - a.z) * f;
            ++cnt;
        }
        if (cnt > 0) { m.x /= cnt; m.y /= cnt; m.z /= cnt; }
        return m;
    };
    for (std::size_t k = 0; k < l.lane.size(); ++k) {
        const auto& pts = l.lane[k];
        if (int(pts.size()) < 2) continue;
        auto& bl = (*table)[k];
        bl.stepM = stepM;
        bl.prefixU.resize(std::size_t(segs) + 1, 0.0);
        for (int i = 0; i < segs; ++i) {
            const auto& a = pts[std::size_t(i)];
            const auto& b = pts[std::size_t((i + 1) % n)];
            bl.prefixU[std::size_t(i) + 1] = bl.prefixU[std::size_t(i)] + std::hypot(b.x - a.x, b.y - a.y, b.z - a.z);
        }
        bl.totalM = bl.prefixU.back();
        if (!(bl.totalM > 0.5)) continue;
        const int maxSamples = 4096;
        const int count = std::clamp(int(std::floor(bl.totalM / stepM)) + (l.closed ? 0 : 1), 2, maxSamples);
        const double actualStep = l.closed ? bl.totalM / double(count) : bl.totalM / double(count - 1);
        bl.stepM = actualStep;
        bl.samples.resize(std::size_t(count));
        int seg = 0;
        for (int si = 0; si < count; ++si) {
            const double targetS = std::min(bl.totalM, double(si) * actualStep);
            while (seg + 1 < segs && bl.prefixU[std::size_t(seg) + 1] < targetS) ++seg;
            const double s0 = bl.prefixU[std::size_t(seg)], s1 = bl.prefixU[std::size_t(seg) + 1];
            const double f = s1 > s0 + 1e-9 ? std::clamp((targetS - s0) / (s1 - s0), 0.0, 1.0) : 0.0;
            const auto& a = pts[std::size_t(seg)];
            const auto& b = pts[std::size_t((seg + 1) % n)];
            auto& out = bl.samples[std::size_t(si)];
            out.u = double(seg) + f;
            out.s = targetS;
            out.pos = { a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f };
        }
        const double dir = Direction(l, int(k));
        // Compute curvature, grade, corridor envelope [minLat, maxLat], and spawnSafe per baked station.
        for (int si = 0; si < count; ++si) {
            const int prev = l.closed ? (si + count - 1) % count : std::max(0, si - 1);
            const int next = l.closed ? (si + 1) % count : std::min(count - 1, si + 1);
            const auto& pa = bl.samples[std::size_t(prev)].pos;
            const auto& pb = bl.samples[std::size_t(si)].pos;
            const auto& pc = bl.samples[std::size_t(next)].pos;
            const double x1 = pb.x - pa.x, y1 = pb.y - pa.y, l1 = std::hypot(x1, y1);
            const double x2 = pc.x - pb.x, y2 = pc.y - pb.y, l2 = std::hypot(x2, y2);
            double curv = 0.0;
            if (l1 > 1e-4 && l2 > 1e-4) {
                curv = std::fabs(std::atan2(x1 * y2 - y1 * x2, x1 * x2 + y1 * y2));
            }
            const double dxy = std::hypot(pc.x - pa.x, pc.y - pa.y);
            const double grade = dxy > 1e-4 ? std::fabs(pc.z - pa.z) / dxy : 0.0;
            bool squeezedHere = false;
            const int uInt = int(std::floor(bl.samples[std::size_t(si)].u));
            for (const auto& sq : l.squeezed) {
                if (sq.lane == int(k) && uInt >= sq.from && uInt <= sq.to) { squeezedHere = true; break; }
            }
            // Corridor envelope: authored yellow box (exact width, never widened past guardrails) minus hazard boxes.
            double tx = 0.0, ty = 1.0;
            if (dxy > 1e-4) {
                const double sgn = dir >= 0.0 ? 1.0 : -1.0;
                tx = ((pc.x - pa.x) / dxy) * sgn;
                ty = ((pc.y - pa.y) / dxy) * sgn;
            }
            const double rx = ty, ry = -tx;  // + = right of travel
            const Vec centre = routeCentreAt(bl.samples[std::size_t(si)].u);
            const double centreLat = (centre.x - pb.x) * rx + (centre.y - pb.y) * ry;
            double halfBox = 6.0;
            if (route) {
                const int tile = routeTiles > 0 ? std::clamp(int(std::floor(l.ToSource(bl.samples[std::size_t(si)].u))), 0, routeTiles - 1) : -1;
                if (tile >= 0 && tile < int(route->road.boxes.size()) && route->road.boxes[std::size_t(tile)].width > 0.0) {
                    halfBox = route->road.boxes[std::size_t(tile)].width * 0.5;
                } else if (route->road.width > 0.0) {
                    halfBox = route->road.width * 0.5;
                } else {
                    halfBox = std::max(1.0, route->corridor * std::max(1, route->lanes)) * 0.5;
                }
            }
            const double halfRight = std::max(0.5, halfBox - (dir >= 0.0 ? l.edgeRight : l.edgeLeft));
            const double halfLeft  = std::max(0.5, halfBox - (dir >= 0.0 ? l.edgeLeft  : l.edgeRight));
            // Respect authored yellow box walls while keeping at least half a lane (+-1.5 m) around the lane centre
            // so legacy routes whose stored road_boxes are narrower than the edited road width (FW-1) never self-block.
            double minLat = std::min(-1.5, centreLat - halfLeft);
            double maxLat = std::max( 1.5, centreLat + halfRight);
            bool hazardBlocked = false;
            if (route && !route->hazards.boxes.empty()) {
                if (routehazard::TouchesAny(route->hazards, pb.x, pb.y, pb.z - 0.2, pb.z + 2.5, 1.35 + routehazard::kPlannerMargin)) {
                    hazardBlocked = true;
                }
                for (double lat = 0.5; lat <= maxLat + 1e-6; lat += 0.5) {
                    if (routehazard::TouchesAny(route->hazards, pb.x + rx * lat, pb.y + ry * lat, pb.z - 0.2, pb.z + 2.5, 0.15)) {
                        maxLat = std::min(maxLat, lat - 0.25);
                        break;
                    }
                }
                for (double lat = -0.5; lat >= minLat - 1e-6; lat -= 0.5) {
                    if (routehazard::TouchesAny(route->hazards, pb.x + rx * lat, pb.y + ry * lat, pb.z - 0.2, pb.z + 2.5, 0.15)) {
                        minLat = std::max(minLat, lat + 0.25);
                        break;
                    }
                }
            }
            bl.samples[std::size_t(si)].curvature = curv;
            bl.samples[std::size_t(si)].grade = grade;
            bl.samples[std::size_t(si)].minLat = minLat;
            bl.samples[std::size_t(si)].maxLat = maxLat;
            bl.samples[std::size_t(si)].hazardBlocked = hazardBlocked;
            bl.samples[std::size_t(si)].spawnSafe = !squeezedHere && !hazardBlocked && curv <= 0.35 && grade <= 0.28;
        }
    }
    l.baked = std::move(table);
}

// O(1) uniform-arclength lane slice from pre-baked table. Returns count written (0 if unbaked).
inline int SliceBaked(const Line& l, int lane, double u, int fromStep, int count, Vec* outPts, double* outU = nullptr) {
    if (!l.baked || lane < 0 || lane >= int(l.baked->size()) || count <= 0 || !outPts) return 0;
    const auto& bl = (*l.baked)[std::size_t(lane)];
    const int n = int(bl.samples.size());
    if (n < 2 || bl.prefixU.empty() || !(bl.stepM > 0.0)) return 0;
    const double span = l.Span();
    if (!(span > 0.0)) return 0;
    if (l.closed) { u = std::fmod(u, span); if (u < 0) u += span; }
    else u = std::clamp(u, 0.0, span);
    const auto seg = std::min(std::size_t(u), bl.prefixU.size() - 1);
    const double s0 = seg + 1 < bl.prefixU.size()
        ? bl.prefixU[seg] + (bl.prefixU[seg + 1] - bl.prefixU[seg]) * (u - double(seg))
        : bl.prefixU.back();
    const double dir = Direction(l, lane);
    const int baseIdx = int(std::llround(s0 / bl.stepM));
    for (int i = 0; i < count; ++i) {
        const int step = fromStep + i;
        int idx = baseIdx + (dir >= 0.0 ? step : -step);
        if (l.closed) { idx %= n; if (idx < 0) idx += n; }
        else idx = std::clamp(idx, 0, n - 1);
        const auto& sample = bl.samples[std::size_t(idx)];
        outPts[i] = sample.pos;
        if (outU) outU[i] = sample.u;
    }
    return count;
}

inline bool BakedSpawnSafe(const Line& l, int lane, double u) {
    if (!l.baked || lane < 0 || lane >= int(l.baked->size())) return true;
    const auto& bl = (*l.baked)[std::size_t(lane)];
    const int n = int(bl.samples.size());
    if (n < 2 || bl.prefixU.empty() || !(bl.stepM > 0.0)) return true;
    const double span = l.Span();
    if (l.closed && span > 0.0) { u = std::fmod(u, span); if (u < 0) u += span; }
    else u = std::clamp(u, 0.0, span);
    const auto seg = std::min(std::size_t(u), bl.prefixU.size() - 1);
    const double s0 = seg + 1 < bl.prefixU.size()
        ? bl.prefixU[seg] + (bl.prefixU[seg + 1] - bl.prefixU[seg]) * (u - double(seg))
        : bl.prefixU.back();
    int idx = int(std::llround(s0 / bl.stepM));
    if (l.closed) { idx %= n; if (idx < 0) idx += n; }
    else idx = std::clamp(idx, 0, n - 1);
    return bl.samples[std::size_t(idx)].spawnSafe;
}

inline bool BakedCorridor(const Line& l, int lane, double u, double& minLat, double& maxLat) {
    if (!l.baked || lane < 0 || lane >= int(l.baked->size())) return false;
    const auto& bl = (*l.baked)[std::size_t(lane)];
    const int n = int(bl.samples.size());
    if (n < 2 || bl.prefixU.empty() || !(bl.stepM > 0.0)) return false;
    const double span = l.Span();
    if (l.closed && span > 0.0) { u = std::fmod(u, span); if (u < 0) u += span; }
    else u = std::clamp(u, 0.0, span);
    const auto seg = std::min(std::size_t(u), bl.prefixU.size() - 1);
    const double s0 = seg + 1 < bl.prefixU.size()
        ? bl.prefixU[seg] + (bl.prefixU[seg + 1] - bl.prefixU[seg]) * (u - double(seg))
        : bl.prefixU.back();
    int idx = int(std::llround(s0 / bl.stepM));
    if (l.closed) { idx %= n; if (idx < 0) idx += n; }
    else idx = std::clamp(idx, 0, n - 1);
    minLat = bl.samples[std::size_t(idx)].minLat;
    maxLat = bl.samples[std::size_t(idx)].maxLat;
    return true;
}

// Compile transforms into the same sampled lane geometry consumed by the existing controllers.
inline Line Build(const trafficdata::Route& r) {
    auto l=BuildBase(r);if(r.road.enabled||l.error||r.lanePlan.transforms.records.empty()||l.Count()<2){BakeLanes(l, 2.0, &r);return l;}
    if(l.lanes.empty()){l.lanes.push_back(0);if(r.lanes>1)l.lanes.push_back(1);}
    std::vector<double> arc{0};for(int i=0;i<int(l.Span());++i)arc.push_back(arc.back()+std::hypot(l.lane[0][(i+1)%l.Count()].x-l.lane[0][i].x,l.lane[0][(i+1)%l.Count()].y-l.lane[0][i].y,l.lane[0][(i+1)%l.Count()].z-l.lane[0][i].z));
    auto metres=[&](double source){const double u=std::clamp(l.ToLine(source),0.0,l.Span());const auto i=std::min(std::size_t(u),arc.size()-1);return i+1<arc.size()?arc[i]+(arc[i+1]-arc[i])*(u-i):arc.back();};
    l.plan=r.lanePlan;l.sourceCount=int(r.points.size());
    for(int lane:l.lanes){
        for(const auto& t:r.lanePlan.transforms.records)if(t.lane==lane&&t.section<0)for(auto& point:l.lane[lane]){const auto q=lanetransform::Point(t,{point.x,point.y,point.z});point={q.x,q.y,q.z};}
        for(int i=0;i<l.Count();++i){auto& point=l.lane[lane][i];lanetransform::Vec displacement{};double weights=0;
            for(const auto& t:r.lanePlan.transforms.records)if(t.lane==lane&&t.section>=0){const double w=lanetransform::Weight(arc[i],metres(t.section),metres(t.section+1),arc.back(),t.blend,l.closed);const auto q=lanetransform::Point(t,{point.x,point.y,point.z});displacement=lanevolume::Add(displacement,lanevolume::Mul(lanevolume::Sub(q,{point.x,point.y,point.z}),w));weights+=w;}
            const double scale=1/std::max(1.0,weights);point={point.x+displacement.x*scale,point.y+displacement.y*scale,point.z+displacement.z*scale};
        }
        l.basis[lane]=lanevolume::Frames(splinekit::Points(l.lane[lane]),l.closed);if(!l.basis[lane].error.empty()){l.error="invalid transformed lane frame";return l;}
    }
    BakeLanes(l, 2.0, &r);
    return l;
}
inline double Wrap(const Line& l, double u)
{
    const double s = l.Span();
    if (s <= 0) return 0;
    if (l.closed) { u = std::fmod(u, s); return u < 0 ? u + s : u; }
    return std::clamp(u, 0.0, s);
}
inline Vec At(const Line& l, int lane, double u)
{
    const auto& p = l.lane[std::clamp(lane,0,int(l.lane.size())-1)];
    if (p.empty()) return {};
    u = Wrap(l, u);
    const int i = int(std::floor(u)), n = int(p.size());
    const int j = l.closed ? (i + 1) % n : std::min(i + 1, n - 1);
    const double f = u - i;
    const Vec& a = p[std::min(i, n - 1)];
    const Vec& b = p[j];
    return { a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f };
}
// The u of the lane point nearest (x, y).
inline double Nearest(const Line& l, int lane, double x, double y)
{
    const auto& p = l.lane[std::clamp(lane,0,int(l.lane.size())-1)];
    double best = 0, bestD = std::numeric_limits<double>::infinity();
    const int n = int(p.size()), segs = l.closed ? n : n - 1;
    for (int i = 0; i < segs; ++i) {
        const Vec& a = p[i];
        const Vec& b = p[(i + 1) % n];
        const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
        const double f = len2 > 1e-9 ? std::clamp(((x - a.x) * dx + (y - a.y) * dy) / len2, 0.0, 1.0) : 0.0;
        const double d = std::hypot(a.x + dx * f - x, a.y + dy * f - y);
        if (d < bestD) { bestD = d; best = i + f; }
    }
    return best;
}
inline double SegmentLength(const Line& l, double u)
{
    const Vec a = At(l, 0, std::floor(Wrap(l, u))), b = At(l, 0, std::floor(Wrap(l, u)) + 1.0);
    return std::max(1.0, std::hypot(b.x - a.x, b.y - a.y));
}
inline double Direction(int lane) { return (lane & 1) ? -1.0 : 1.0; }
inline double Direction(const Line& l,int lane) { return lane>=0&&lane<int(l.direction.size())?l.direction[lane]:Direction(lane); }
inline bool Available(const Line& l,int lane,double u) {
    return !l.error&&(!l.road.empty()||!l.plan.enabled||laneplan::Active(laneplan::Resolve(l.plan,l.sourceCount?int(routepolicy::Segment(l.sourceCount,l.closed,l.ToSource(u),lane)):-1),lane));
}
inline std::vector<int> Lanes(const Line& l,int legacy=2) {return l.error?std::vector<int>{}:l.lanes.empty()?(legacy>1?std::vector<int>{0,1}:std::vector<int>{0}):l.lanes;}
inline bool HasLane(const Line& l,int legacy,int lane) {const auto lanes=Lanes(l,legacy);return l.Count()>=2&&std::find(lanes.begin(),lanes.end(),lane)!=lanes.end();}
inline int PickLane(const Line& l,int legacy,unsigned pick) {const auto lanes=Lanes(l,legacy);return lanes.empty()?0:lanes[pick%lanes.size()];}
inline int KeepLane(const Line& l,int legacy,int lane) {const auto available=Lanes(l,legacy);return available.empty()?0:std::find(available.begin(),available.end(),lane)!=available.end()?lane:available.front();}
// How far (in u) the lane travels from `from` to `to`; infinity when an open path's lane never gets there.
inline double Ahead(const Line& l, int lane, double from, double to)
{
    double d = (to - from) * Direction(l,lane);
    if (l.closed) { d = std::fmod(d, l.Span()); if (d < 0) d += l.Span(); return d; }
    return d >= 0 ? d : std::numeric_limits<double>::infinity();
}
inline bool ConnectedAlong(const Line& l,int lane,double from,double to) {
    if(!l.plan.enabled)return true;
    const double distance=Ahead(l,lane,from,to);if(!std::isfinite(distance))return false;
    const int steps=std::max(1,int(std::ceil(distance*4)));
    for(int i=0;i<=steps;++i)if(!Available(l,lane,Wrap(l,from+Direction(l,lane)*distance*i/steps)))return false;
    return true;
}
// The lane reaching `to` from `from` sooner (one-lane routes: lane 0).
inline int BestLane(const Line& l, int lanes, double from, double to)
{
    auto available=Lanes(l,lanes);if(available.empty())return 0;int best=available.front();
    for(auto lane:available)if(Ahead(l,lane,from,to)<Ahead(l,best,from,to))best=lane;return best;
}
inline double Length(const Line& l)
{
    double s = 0;
    const auto& p = l.lane[0];
    const int n = int(p.size()), segs = l.closed ? n : n - 1;
    for (int i = 0; i < segs; ++i) s += std::hypot(p[(i + 1) % n].x - p[i].x, p[(i + 1) % n].y - p[i].y);
    return s;
}

// Move by metres along the polyline, not a fraction of whichever segment the
// vehicle currently occupies. Do not cut diagonally across sharp corners.
inline double LeadU(const Line& l,int lane,double u,double metres,bool reverse=false,bool stopAtCorners=true){
    const double dir=Direction(l,lane)*(reverse?-1:1);u=Wrap(l,u);
    for(int step=0;step<l.Count()+1&&metres>0.001;++step){
        double edge=dir>0?std::floor(u+1e-6)+1:std::ceil(u-1e-6)-1;
        if(!l.closed)edge=std::clamp(edge,0.0,l.Span());
        const Vec a=At(l,lane,u),b=At(l,lane,edge);
        const double length=std::hypot(b.x-a.x,b.y-a.y,b.z-a.z);
        if(length>=metres)return Wrap(l,u+(edge-u)*metres/length);
        if(length<0.001&&edge==u)return u;
        const Vec c=At(l,lane,edge+dir*0.01);
        const double ax=b.x-a.x,ay=b.y-a.y,bx=c.x-b.x,by=c.y-b.y;
        const double norms=std::hypot(ax,ay)*std::hypot(bx,by);
        // Inside 4 m, lead just beyond the vertex so it can be traversed;
        // otherwise target the corner, rather than a chord through a building.
        if(stopAtCorners&&norms>1e-6&&(ax*bx+ay*by)/norms<0.866&&length>4)return Wrap(l,edge);
        metres-=length;u=Wrap(l,edge);
    }
    return u;
}
inline double DistanceAlong(const Line& l,int lane,double from,double to){
    double remaining=Ahead(l,lane,from,to);if(!std::isfinite(remaining))return remaining;
    double result=0;double u=Wrap(l,from);const double dir=Direction(l,lane);
    for(int i=0;i<l.Count()+1&&remaining>1e-6;++i){
        const double edge=dir>0?std::floor(u+1e-6)+1:std::ceil(u-1e-6)-1;
        const double part=std::min(remaining,std::fabs(edge-u));
        const auto a=At(l,lane,u),b=At(l,lane,u+dir*part);
        result+=std::hypot(b.x-a.x,b.y-a.y,b.z-a.z);remaining-=part;u=Wrap(l,u+dir*part);
    }
    return result;
}

enum class Leg { Climb, Join, Follow, Leave, Done };
inline const char* Name(Leg l)
{
    switch (l) {
    case Leg::Climb: return "climbing to the lane";
    case Leg::Join: return "joining the lane";
    case Leg::Follow: return "following the route";
    case Leg::Leave: return "leaving the route";
    default: return "done";
    }
}
struct Trip {
    Leg leg = Leg::Climb;
    int lane = 0;
    bool lap = true;               // lap and home, else to `dest`
    double targetU = 0;            // where Follow ends
    double lastU = 0, travelled = 0;
    bool uKnown = false;
    Vec home, dest;
    double laneZ = 0;              // the lane's height at the join
};
struct Goal { double x = 0, y = 0, z = 0, stop = 6.0; bool passThrough = false; };

// A lap from home: join the lane nearest home, once round (a path: to its end and back is not a lap; it goes to the end).
inline Trip StartLap(const Line& l, int lane, Vec home)
{
    Trip t; t.lane = std::clamp(lane,0,int(l.lane.size())-1); t.lap = true; t.home = home;
    t.targetU = Nearest(l, t.lane, home.x, home.y);
    if (!l.closed) t.targetU = Direction(l,t.lane)>0 ? l.Span() : 0.0;  // a path's lap: to the end of its lane
    return t;
}
// To a destination: leave at `exitU` (the exit nearest it), on the lane that gets there sooner.
inline Trip StartTo(const Line& l, int lanes, Vec home, Vec dest, double exitU)
{
    Trip t; t.lap = false; t.home = home; t.dest = dest; t.targetU = exitU;
    t.lane = BestLane(l, lanes, Nearest(l, 0, home.x, home.y), exitU);
    return t;
}

// Height-aware projection with a bounded distance along the current lane. This
// prevents switching arms at a crossing or between a bridge and the road below.
inline double NearestProgress(const Line& l,int lane,double x,double y,double z,bool known,double previous) {
    const auto& points=l.lane[std::clamp(lane,0,int(l.lane.size())-1)];double best=previous,bestD=1e30;
    const int segments=int(l.Span());if(points.size()<2||segments<=0)return previous;
    const auto old=At(l,lane,previous);const double window=std::max(12.0,2*std::hypot(x-old.x,y-old.y,z-old.z)+4);
    // One arc-length pass replaces a full DistanceAlong walk for every candidate.
    // Keep 3D distances and closed-loop wrap so bridge/crossing continuity is identical.
    std::vector<double> prefix;if(known){prefix.resize(std::size_t(segments)+1);for(int i=0;i<segments;++i){const auto& a=points[std::size_t(i)];const auto& b=points[std::size_t((i+1)%points.size())];prefix[std::size_t(i)+1]=prefix[std::size_t(i)]+std::hypot(b.x-a.x,b.y-a.y,b.z-a.z);}}
    double previousMetres=0;if(known){const double u=Wrap(l,previous);const auto i=std::min(std::size_t(u),prefix.size()-1);previousMetres=prefix[i]+(i+1<prefix.size()?(prefix[i+1]-prefix[i])*(u-double(i)):0);}
    for(int i=0;i<segments;++i){const auto& a=points[std::size_t(i)];const auto& b=points[std::size_t((i+1)%points.size())];
        const double dx=b.x-a.x,dy=b.y-a.y,dz=b.z-a.z,den=dx*dx+dy*dy+dz*dz;
        const double t=den>1e-9?std::clamp(((x-a.x)*dx+(y-a.y)*dy+(z-a.z)*dz)/den,0.0,1.0):0;
        const double u=i+t;
        if(known){const double metres=prefix[std::size_t(i)]+(prefix[std::size_t(i)+1]-prefix[std::size_t(i)])*t;double distance=std::fabs(metres-previousMetres);if(l.closed)distance=std::min(distance,std::max(0.0,prefix.back()-distance));if(distance>window)continue;}
        const double d=std::hypot(x-a.x-t*dx,y-a.y-t*dy,z-a.z-t*dz);
        if(d<bestD){bestD=d;best=u;}
    }
    return best;
}
inline Goal Step(const Line& l, Trip& t, double x, double y, double z, double reach=kReach, double lead=kLead)
{
    Goal g;
    double joinU = reach<kReach ? NearestProgress(l,t.lane,x,y,z,t.uKnown,t.lastU) : Nearest(l, t.lane, x, y);
    if(t.leg==Leg::Follow&&t.uKnown){
        const auto old=At(l,t.lane,t.lastU);
        const double direct=std::hypot(old.x-x,old.y-y,old.z-z),limit=std::max(20.0,2*direct+5);
        const double along=std::min(DistanceAlong(l,t.lane,t.lastU,joinU),DistanceAlong(l,t.lane,joinU,t.lastU));
        if(along>limit)joinU=t.lastU; // don't jump to another arm of a crossing route
    }
    const Vec join = At(l, t.lane, joinU);
    if (t.leg == Leg::Climb) {
        if (z >= join.z - kClimbSlack) { t.leg = Leg::Join; }
        else {
            // Up, leaning up to kClimbLean toward the lane: a goal straight overhead is 0 m away sideways, which the autopilot took
            // for "arrived" in a settlement and stayed on the ground (2026-10-28 18:11, a cold-start vehicle idle 131 s at its spot).
            const double dx = join.x - t.home.x, dy = join.y - t.home.y, d = std::hypot(dx, dy);
            const double k = d > 1.0 ? std::min(kClimbLean, d) / d : 0.0;
            g.x = t.home.x + dx * k; g.y = t.home.y + dy * k; g.z = join.z; g.stop = 4.0;
            return g;
        }
    }
    if (t.leg == Leg::Join) {
        if (std::hypot(join.x - x, join.y - y) < reach && std::fabs(z - join.z) < reach) {
            t.leg = Leg::Follow; t.lastU = joinU; t.uKnown = true; t.travelled = 0;
        }
        else { g.x = join.x; g.y = join.y; g.z = join.z; g.passThrough = true; return g; }
    }
    if (t.leg == Leg::Follow) {
        const double u = joinU;
        bool crossedTarget=false;
        if (t.uKnown) {
            double step = (u - t.lastU) * Direction(l,t.lane);
            if (l.closed) step = std::remainder(step, l.Span());
            if (step > 0 && DistanceAlong(l,t.lane,t.lastU,u)<150.0){
                const double target=Ahead(l,t.lane,t.lastU,t.targetU);
                crossedTarget=std::isfinite(target)&&target<=step+1e-6;
                t.travelled+=step;
            }
        }
        t.lastU = u; t.uKnown = true;
        const double left = Ahead(l, t.lane, u, t.targetU);
        const bool lapDone = !t.lap || !l.closed || t.travelled >= l.Span() * 0.9;
        const bool there = lapDone && (crossedTarget || left * SegmentLength(l, u) < reach || (!l.closed && left == 0.0));
        if (there) t.leg = Leg::Leave;
        else {
            const double leadU = LeadU(l,t.lane,u,lead);
            const Vec p = At(l, t.lane, leadU);
            g.x = p.x; g.y = p.y; g.z = p.z; g.passThrough = true;
            t.laneZ = p.z;
            return g;
        }
    }
    const Vec to = t.lap ? t.home : t.dest;
    if (t.leg == Leg::Leave) {
        if (std::hypot(to.x - x, to.y - y) < reach) t.leg = Leg::Done;
        else { g.x = to.x; g.y = to.y; g.z = t.laneZ ? t.laneZ : join.z; g.stop = 6.0; return g; }
    }
    g.x = to.x; g.y = to.y; g.z = z; g.stop = 6.0;
    return g;
}

}
