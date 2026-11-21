#include <cstdlib>
#include "roadfield.h"
#include "trafficinitial.h"
#include "liftpath.h"
#include <cstdio>
#include <fstream>
#include <sstream>
// Loads a traffic file with the mod's own reader and builds one route the way traffic does: reports record
// validity, build errors, lane widths per station and the open-air tiles.
int main(int argc,char** argv){
    if(argc<3){std::printf("usage: route_check <file.ini> <route>\n");return 2;}
    std::ifstream in(argv[1]);std::stringstream text;text<<in.rdbuf();trafficdata::Data data;trafficdata::Read(text.str(),argv[1],data);
    for(const auto& r:data.routes)if(r.name==argv[2]){
        std::printf("route %s: %zu points, closed %d, lanes %d, travel %d, road %d, record valid %d, width %.1f m, height %.1f m\n",r.name.c_str(),r.points.size(),int(r.closed),r.lanes,int(r.travel),int(r.road.enabled),int(roadplan::Valid(r.road)),r.road.width,r.road.height);
        for(std::size_t i=0;i<r.points.size();++i){const auto k=roadplan::PrimitiveAt(r.road,i);if(k.kind!=roadplan::Box)std::printf("  tile %zu->%zu: %s\n",i+1,(i+1)%r.points.size()+1,roadplan::KindName(k.kind));}
        const auto line=trafficroute::Build(r);
        if(line.error){std::printf("BUILD ERROR: %s\n",line.error);return 1;}
        double narrow=1e9,wide=0;for(const auto& s:line.road)for(std::size_t k=0;k+1<s.boundaries.size();++k){const double w=std::hypot(s.boundaries[k+1].x-s.boundaries[k].x,s.boundaries[k+1].y-s.boundaries[k].y);narrow=std::min(narrow,w);wide=std::max(wide,w);}
        std::printf("built: %d stations, %zu lanes, lane width %.1f to %.1f m (a 2.2 m vehicle needs >= 2.5 m)\n",line.Count(),line.lanes.size(),narrow,wide);
        {   // LF-1 (2026-11-12): a lane line must never fold or kink: 1 m steps along each lane, flag a turn over 45 deg within 1 m, or a lane step
            // running against the route's own direction (the 13:56 run: lane 1 swapped sides at lc_ring_road's hairpin, yaw -92 -> +13 deg in 1 m).
            int folds=0;
            for(int lane:line.lanes){double prevYaw=1e9;double u=0;auto p=trafficroute::At(line,lane,0);
                while(u<line.Span()-1e-6){const double nu=trafficroute::LeadU(line,lane,u,1.0,false,false);if(!(nu>u))break;const auto q=trafficroute::At(line,lane,nu);
                    const double run=std::hypot(q.x-p.x,q.y-p.y);if(run<1e-3){u=nu;continue;}
                    const double yaw=std::atan2(q.x-p.x,q.y-p.y);
                    const auto c0=trafficroute::At(line,0,u),c1=trafficroute::At(line,0,nu);(void)c0;(void)c1;
                    const double turn=prevYaw<1e8?std::fabs(std::remainder(yaw-prevYaw,6.283185307179586))*57.29578:0;
                    if(turn>45&&folds<40){++folds;std::printf("  LANE KINK lane %d at u %.2f (%.1f,%.1f,%.1f): turns %.0f deg within 1 m, step %.2f m\n",lane,u,p.x,p.y,p.z,turn,run);}
                    prevYaw=yaw;p=q;u=nu;}}
            std::printf("lane kinks: %d\n",folds);
            if(const char* e=std::getenv("ROUTE_CHECK_DUMP")){double u0=0,u1=0;if(std::sscanf(e,"%lf,%lf",&u0,&u1)==2)for(int lane:line.lanes)for(double u=u0;u<=u1;u+=.5){const auto q=trafficroute::At(line,lane,u);std::printf("  DUMP lane %d u %.2f (%.2f,%.2f,%.2f)\n",lane,u,q.x,q.y,q.z);}}
        }
        std::printf("lane plan: enabled %d, unsupported %d, vehicle rules %zu, vehicle unsupported %d\n",int(r.lanePlan.enabled),int(!r.lanePlan.unsupported.empty()),r.lanePlan.vehicles.lanes.size(),int(!r.lanePlan.vehicles.unsupported.empty()));
        if(r.lanePlan.enabled)for(int lane=0;lane<2;++lane){int on=0,off=0;for(double u=0;u<line.Count();u+=.5){const auto st=trafficroute::SettingsAt(line,u,lane);(laneplan::Active(st,lane)?on:off)++;}std::printf("  lane %d active at %d of %d samples\n",lane,on,on+off);}
        if(!line.road.empty()){
            const auto slots=trafficinitial::Route(r,50);
            const auto open=std::count_if(slots.begin(),slots.end(),[&](const auto& slot){
                return line.road[std::size_t(slot.u)].primitive.kind==roadplan::Open;
            });
            std::printf("initial spread: %zu spots, %td on open-air tiles\n",slots.size(),open);
        }
        for(int t=0;t<int(r.points.size());++t)if(liftpath::IsLift(r,t))for(int lane=0;lane<2;++lane)for(int m:{t,(t+1)%int(r.points.size())}){
            const double at=line.ToLine(double(m));for(double d:{-30.0,-15.0,-5.0,-1.0,1.0,5.0,15.0,30.0}){const double u=trafficroute::LeadU(line,lane,at,std::fabs(d),d<0,false);const auto v=trafficroute::At(line,lane,u);const auto w=liftpath::At(r,line,lane,u);
                std::printf("lift tile %d marker %d lane %d d %+.0f u %.2f raw %.1f,%.1f,%.1f weight %.2f\n",t+1,m+1,lane,d,u,v.x,v.y,v.z,w.weight);}}
        // Optional spot test: route_check <file> <route> x y heading_deg halfWidth halfLength height originAbove
        if(argc>=10){FvoFieldPoseV1 q;q.x=atof(argv[3]);q.y=atof(argv[4]);q.heading=atof(argv[5])*3.141592653589793/180;q.halfWidth=atof(argv[6])+.15;q.halfLength=atof(argv[7])+.15;q.height=atof(argv[8]);q.originAbove=atof(argv[9]);
            for(int lane:line.lanes){double floor=0;const bool on=roadfield::Floor(line,lane,q.x,q.y,floor);q.z=floor+q.originAbove;
                auto c=q;c.halfWidth=0;c.halfLength=0;const bool centre=on&&roadfield::Fits(line,lane,c);
                auto flat=q;flat.height=.01;const bool body=on&&roadfield::Fits(line,lane,flat);
                if(lane==0&&on){const double cs=std::cos(q.heading),sn=std::sin(q.heading);for(double dx:{-q.halfWidth,q.halfWidth})for(double dy:{-q.halfLength,q.halfLength}){const double px=q.x+dx*cs+dy*sn,py=q.y-dx*sn+dy*cs;double fz=0;const bool f=roadfield::Floor(line,lane,px,py,fz);
                    std::printf("  corner (%+.2f,%+.2f) floor %d z %.2f  ground %d  roof %d\n",dx,dy,int(f),fz,int(roadfield::Point(line,lane,{px,py,q.z-q.originAbove},true)),int(roadfield::Point(line,lane,{px,py,q.z+q.height-q.originAbove},false)));}}
                std::printf("lane %d: on floor %d (z %.2f), centre fits %d, footprint fits %d, with roof %d, surface-aligned birth %d\n",lane,int(on),floor,int(centre),int(body),int(on&&roadfield::Fits(line,lane,q)),int(on&&roadfield::Fits(line,lane,q,true)));}}
        return narrow>=2.5?0:1;
    }
    std::printf("route %s not found\n",argv[2]);return 1;
}
