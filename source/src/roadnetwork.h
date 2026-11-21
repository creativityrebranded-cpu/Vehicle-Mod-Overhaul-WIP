#pragma once
#include "trafficroute.h"
#include "trafficlinks.h"
#include <map>
#include <queue>
#include <tuple>
#include <functional>

namespace roadnetwork {
using Vec=trafficdata::Vec;
namespace rules {
constexpr double approachReach=60, waitAging=5, reservationGrace=3;
constexpr double stopMargin=3, holdDistance=1, stopBand=2, stoppedSpeed=.3;
constexpr double approachFresh=1, approachRefresh=.1, crossingHeight=6;
constexpr double exitLead=12, exitRadius=8, exitHeight=4, exitBlockedSpeed=1;
}
struct Approach { std::string route; double u=0; Vec at; };
struct Junction { Vec at; double radius=9; std::vector<Approach> approaches; };
struct Request {
    unsigned vehicle=0; int junction=-1, priority=0;
    double distance=0, stoppedFor=0, waited=0;
    bool inside=false, exitClear=true, green=true;
    routepolicy::Control control=routepolicy::Control::None;
    double stopSeconds=1;
    bool frontOfQueue=true;
};
inline bool EntryReady(const Request& q) {
    return q.frontOfQueue&&q.green&&q.exitClear&&
        (q.control!=routepolicy::Control::Stop||q.stoppedFor>=q.stopSeconds);
}
inline bool MustYield(const Request& q,unsigned granted) {
    return !q.inside&&q.distance>=0&&q.vehicle!=granted;
}
struct ApproachTiming {
    double since=-1,stoppedSince=-1;
    void Observe(double now,double distance,double speed) {
        if(since<0)since=now;
        if(distance>=0&&distance<=rules::stopBand&&speed<rules::stoppedSpeed){
            if(stoppedSince<0)stoppedSince=now;
        }else stoppedSince=-1;
    }
};
struct Reservation { unsigned owner=0; double lastSeen=0; bool entered=false; };
class Arbiter {
    std::map<int,Reservation> reservations;
public:
    void Reset(){reservations.clear();}
    // Whole conflict-zone reservation is intentionally conservative. It is
    // released by observed exit, never merely by a predicted arrival time.
    unsigned Grant(int junction,double now,const std::vector<Request>& requests) {
        auto& r=reservations[junction];
        const Request* owner=nullptr;
        const Request* occupant=nullptr;
        for(const auto& q:requests){
            if(!q.vehicle||q.junction!=junction)continue;
            if(q.vehicle==r.owner)owner=&q;
            if(q.inside&&(!occupant||q.vehicle<occupant->vehicle))occupant=&q;
        }
        if(owner&&owner->inside){r.lastSeen=now;r.entered=true;return r.owner;}
        if(occupant){r={occupant->vehicle,now,true};return r.owner;}
        if(owner){
            if(!r.entered&&owner->distance>=0&&EntryReady(*owner)){r.lastSeen=now;return r.owner;}
            r={}; // observed outside after entry, past the approach, or lost permission
        }else if(r.owner&&now-r.lastSeen<=rules::reservationGrace)return r.owner;
        else r={};
        const Request* best=nullptr;
        for(const auto& q:requests){
            if(!q.vehicle||q.junction!=junction||q.distance>rules::approachReach||q.distance<0||!EntryReady(q))continue;
            const auto rank=[](const Request& a){return std::make_tuple(
                a.priority+int(std::min(1000.0,a.waited)/rules::waitAging),a.waited,-a.distance,-std::int64_t(a.vehicle));};
            if(!best||rank(q)>rank(*best))best=&q;
        }
        if(best){r={best->vehicle,now,best->inside};return best->vehicle;}return 0;
    }
};
inline bool Green(const routepolicy::Section& s,double seconds){
    if(s.control!=routepolicy::Control::Signal)return true;
    double phase=std::fmod(seconds+s.signalOffset,s.signalPeriod);if(phase<0)phase+=s.signalPeriod;
    return phase<s.signalGreen;
}
inline bool Crossing(Vec a,Vec b,Vec c,Vec d,Vec& at,double& ta,double& tb){
    const double rx=b.x-a.x,ry=b.y-a.y,sx=d.x-c.x,sy=d.y-c.y,den=rx*sy-ry*sx;
    if(std::fabs(den)<1e-8)return false;
    ta=((c.x-a.x)*sy-(c.y-a.y)*sx)/den;tb=((c.x-a.x)*ry-(c.y-a.y)*rx)/den;
    if(ta<0||ta>1||tb<0||tb>1)return false;
    const double za=a.z+(b.z-a.z)*ta,zb=c.z+(d.z-c.z)*tb;
    if(std::fabs(za-zb)>3)return false; // bridge/overpass, not a junction
    at={a.x+rx*ta,a.y+ry*ta,(za+zb)*0.5};return true;
}
struct Connection { int from=0,to=0; double at=0,toAt=0;int fromLane=-1,toLane=-1; };
struct Itinerary { std::vector<std::string> routes; std::vector<double> exits,entries; std::vector<int> lanes; double cost=1e30; };
class Network {
public:
    std::vector<trafficdata::Route> routes;
    std::vector<trafficroute::Line> lines;
    std::vector<Connection> connections;
    std::vector<Junction> junctions;
    void AddJunction(Vec at,Approach a,Approach b){
        auto it=std::find_if(junctions.begin(),junctions.end(),[&](const Junction& j){return std::hypot(at.x-j.at.x,at.y-j.at.y,at.z-j.at.z)<10;});
        if(it==junctions.end()){junctions.push_back({at,9,{}});it=junctions.end()-1;}
        for(const auto& p:{a,b})if(std::none_of(it->approaches.begin(),it->approaches.end(),[&](const Approach& x){return x.route==p.route&&std::fabs(x.u-p.u)<0.1;}))it->approaches.push_back(p);
    }
    void Build(std::vector<trafficdata::Route> input,const std::vector<trafficdata::Link>& links){
        routes=std::move(input);lines.clear();connections.clear();junctions.clear();
        for(const auto& r:routes)lines.push_back(trafficroute::Build(r));
        for(const auto& k:links){
            int a=-1,b=-1;for(int i=0;i<int(routes.size());++i){if(routes[i].name==k.from)a=i;if(routes[i].name==k.to)b=i;}
            if(a<0||b<0)continue;
            const double ua=trafficlinks::U(routes[a],k.at),ub=k.toEnd?lines[b].Span():trafficlinks::U(routes[b],k.toAt);
            const auto pa=trafficroute::At(lines[a],0,ua),pb=trafficroute::At(lines[b],0,ub);
            if(std::hypot(pa.x-pb.x,pa.y-pb.y,pa.z-pb.z)>std::max(60.0,k.radius*2))continue;
            AddJunction(pa,{routes[a].name,ua,pa},{routes[b].name,ub,pb});
            if(k.type=="crossing")continue;
            if(k.type!="merge")connections.push_back({a,b,ua,ub,k.fromLane,k.toLane});
            if(k.type!="exit")connections.push_back({b,a,ub,ua,k.toLane,k.fromLane});
        }
        // Spatial bins bound intersection discovery to nearby lane segments.
        struct Edge {int r,i;Vec a,b;};std::map<std::pair<int,int>,std::vector<Edge>> bins;
        for(int r=0;r<int(lines.size());++r){const auto& l=lines[r];for(int i=0;i<int(l.Span());++i){
            const auto a=trafficroute::At(l,0,i),b=trafficroute::At(l,0,i+1);
            const int x0=int(std::floor(std::min(a.x,b.x)/64)),x1=int(std::floor(std::max(a.x,b.x)/64));
            const int y0=int(std::floor(std::min(a.y,b.y)/64)),y1=int(std::floor(std::max(a.y,b.y)/64));
            // Very long authored segments are subdivided by the authoring curve;
            // cap pathological file work, explicit links still function.
            if(std::int64_t(x1-x0+1)*(y1-y0+1)>4096)continue;
            for(int x=x0;x<=x1;++x)for(int y=y0;y<=y1;++y)bins[{x,y}].push_back({r,i,a,b});
        }}
        std::set<std::tuple<int,int,int,int>> done;
        for(const auto& [key,edges]:bins){(void)key;for(std::size_t i=0;i<edges.size();++i)for(std::size_t j=i+1;j<edges.size();++j){
            auto a=edges[i],b=edges[j];if(a.r==b.r)continue;if(a.r>b.r)std::swap(a,b);
            if(!done.insert({a.r,a.i,b.r,b.i}).second)continue;
            Vec p;double ta=0,tb=0;if(Crossing(a.a,a.b,b.a,b.b,p,ta,tb))AddJunction(p,{routes[a.r].name,a.i+ta,p},{routes[b.r].name,b.i+tb,p});
        }}
    }
    // Lane-direction-aware shortest connected itinerary. Each arrival connection
    // is a separate state, so a route with multiple junctions keeps their order.
    Itinerary Plan(Vec start,Vec dest,double reach,const std::vector<std::string>& required={},const std::string& destinationRoute={},int destinationLane=-1,const std::function<bool(const trafficdata::Route&)>& admissible={})const{
        struct Node{int r,lane;double u,cost;int previous,via,stage;};std::vector<Node> nodes;
        std::priority_queue<std::pair<double,int>,std::vector<std::pair<double,int>>,std::greater<std::pair<double,int>>> open;
        for(int r=0;r<int(routes.size());++r)for(int lane:trafficroute::Lanes(lines[r],routes[r].lanes)){
            if((admissible&&!admissible(routes[r]))||(!required.empty()&&routes[r].name!=required.front()))continue;
            const double u=trafficroute::Nearest(lines[r],lane,start.x,start.y);const auto p=trafficroute::At(lines[r],lane,u);
            const double join=std::hypot(start.x-p.x,start.y-p.y,start.z-p.z);if(join>reach||!trafficroute::Available(lines[r],lane,u))continue;
            nodes.push_back({r,lane,u,join,-1,-1,0});open.push({join,int(nodes.size()-1)});
        }
        Itinerary result;int finish=-1;double exitU=0;
        std::map<std::tuple<int,int,long long,int>,double> best;
        while(!open.empty()){
            const auto [cost,index]=open.top();open.pop();const Node n=nodes[index];if(cost>result.cost)continue;
            const auto key=std::make_tuple(n.r,n.lane,std::llround(n.u*10000),required.empty()?0:n.stage);if(best.count(key)&&best[key]<=cost)continue;best[key]=cost;
            const auto& l=lines[n.r];const double u=trafficroute::Nearest(l,n.lane,dest.x,dest.y);const auto p=trafficroute::At(l,n.lane,u);
            const double along=trafficroute::DistanceAlong(l,n.lane,n.u,u),leave=std::hypot(dest.x-p.x,dest.y-p.y,dest.z-p.z);
            if((destinationRoute.empty()||routes[n.r].name==destinationRoute)&&(destinationLane<0||n.lane==destinationLane)&&(required.empty()||n.stage+1==int(required.size()))&&std::isfinite(along)&&trafficroute::ConnectedAlong(l,n.lane,n.u,u)&&cost+along+4*leave<result.cost){result.cost=cost+along+4*leave;finish=index;exitU=u;}
            for(int c=0;c<int(connections.size());++c){const auto& k=connections[c];if(k.from!=n.r||(admissible&&!admissible(routes[k.to])))continue;
                if(k.fromLane>=0&&k.fromLane!=n.lane)continue;
                if(!required.empty()&&(n.stage+1>=int(required.size())||routes[k.to].name!=required[std::size_t(n.stage+1)]))continue;
                const double d=trafficroute::DistanceAlong(l,n.lane,n.u,k.at);if(!std::isfinite(d)||!trafficroute::ConnectedAlong(l,n.lane,n.u,k.at))continue;
                for(int lane:trafficroute::Lanes(lines[k.to],routes[k.to].lanes)){const auto a=trafficroute::At(l,n.lane,k.at),b=trafficroute::At(lines[k.to],lane,k.toAt);
                    if(k.toLane>=0&&k.toLane!=lane)continue;
                    if(!trafficroute::Available(lines[k.to],lane,k.toAt))continue;
                    const double next=cost+d+std::hypot(a.x-b.x,a.y-b.y,a.z-b.z)+5;if(next>=result.cost)continue;
                    nodes.push_back({k.to,lane,k.toAt,next,index,c,n.stage+1});open.push({next,int(nodes.size()-1)});
                }
            }
        }
        if(finish<0)return result;
        std::vector<int> chain;for(int i=finish;i>=0;i=nodes[i].previous)chain.push_back(i);std::reverse(chain.begin(),chain.end());
        for(std::size_t i=0;i<chain.size();++i){const auto& n=nodes[chain[i]];result.routes.push_back(routes[n.r].name);result.lanes.push_back(n.lane);result.entries.push_back(n.u);result.exits.push_back(i+1<chain.size()?connections[nodes[chain[i+1]].via].at:exitU);}
        return result;
    }
};
}
