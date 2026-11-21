#pragma once
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
namespace placeaccess {
struct Point{double x=0,y=0,z=0;};
struct Plan{std::string entryRoute,exitRoute;int entryLane=-1,exitLane=-1;double width=7,height=5;std::vector<Point> entrance,departure;std::string unsupported;};
// Landings as nodes (2026-11-03, F7 spec 7b): a wire joins a route lane to the landing's In socket (arrival, a path
// toward the landing) or its Out socket (departure, a path away from it). A socket takes any number of wires, so a
// landing can belong to one route or many. The first In and first Out wire stay in the single entrance / departure
// fields above, so older builds keep working; only wires beyond those are stored apart (access_wires).
struct Wire{int socket=0;std::string route;int lane=-1;std::vector<Point> path;};
inline std::vector<Wire> FromPlan(const Plan& p){std::vector<Wire> w;
    if(!p.entryRoute.empty()||!p.entrance.empty())w.push_back({0,p.entryRoute,p.entryLane,p.entrance});
    if(!p.exitRoute.empty()||!p.departure.empty())w.push_back({1,p.exitRoute,p.exitLane,p.departure});return w;}
inline int First(const std::vector<Wire>& w,int socket){for(std::size_t i=0;i<w.size();++i)if(w[i].socket==socket)return int(i);return -1;}
// The single fields follow the first wire of each socket (cleared when a socket has none).
inline void Mirror(Plan& p,const std::vector<Wire>& w){
    const int in=First(w,0),out=First(w,1);
    if(in>=0){p.entryRoute=w[std::size_t(in)].route;p.entryLane=w[std::size_t(in)].lane;p.entrance=w[std::size_t(in)].path;}else{p.entryRoute.clear();p.entryLane=-1;p.entrance.clear();}
    if(out>=0){p.exitRoute=w[std::size_t(out)].route;p.exitLane=w[std::size_t(out)].lane;p.departure=w[std::size_t(out)].path;}else{p.exitRoute.clear();p.exitLane=-1;p.departure.clear();}
}
// Edits made through the single fields (the older panel rows) become the first wire of their socket.
inline std::vector<Wire> Merge(const Plan& p,std::vector<Wire> w){
    for(const auto& one:FromPlan(p)){const int at=First(w,one.socket);if(at>=0)w[std::size_t(at)]=one;else w.insert(w.begin(),one);}
    for(int socket:{0,1})if(First(FromPlan(p),socket)<0){const int at=First(w,socket);if(at>=0&&w[std::size_t(at)].route.empty()&&w[std::size_t(at)].path.empty())w.erase(w.begin()+at);}
    return w;
}
inline bool Extra(const std::vector<Wire>& w){int in=0,out=0;for(const auto& x:w)(x.socket?out:in)++;return in>1||out>1;}
inline bool ValidWires(const std::vector<Wire>& w){if(w.size()>64)return false;for(const auto& x:w){if((x.socket!=0&&x.socket!=1)||x.route.size()>200||x.lane< -1||x.lane>15||x.path.size()>128)return false;
    for(const auto& q:x.path)if(!std::isfinite(q.x)||!std::isfinite(q.y)||!std::isfinite(q.z))return false;}return true;}
inline std::string WriteWires(const std::vector<Wire>& w){std::ostringstream out;out<<std::setprecision(17)<<1<<' '<<w.size();
    for(const auto& x:w){out<<' '<<x.socket<<' '<<std::quoted(x.route)<<' '<<x.lane<<' '<<x.path.size();for(const auto& q:x.path)out<<' '<<q.x<<' '<<q.y<<' '<<q.z;}return out.str();}
inline bool ReadWires(const std::string& text,std::vector<Wire>& w){std::istringstream in(text);int v=0;std::size_t n=0;if(!(in>>v>>n)||v!=1||n>64)return false;std::vector<Wire> out(n);
    for(auto& x:out){std::size_t k=0;if(!(in>>x.socket>>std::quoted(x.route)>>x.lane>>k)||k>128)return false;x.path.resize(k);for(auto& q:x.path)if(!(in>>q.x>>q.y>>q.z))return false;}
    in>>std::ws;if(!in.eof()||!ValidWires(out))return false;w=std::move(out);return true;}
// A trip's own plan: the landing's settings with the chosen arrival and departure wires.
inline Plan Compose(const Plan& base,const Wire* in,const Wire* out){Plan p=base;
    if(in){p.entryRoute=in->route;p.entryLane=in->lane;p.entrance=in->path;}
    if(out){p.exitRoute=out->route;p.exitLane=out->lane;p.departure=out->path;}return p;}
inline void MoveWires(std::vector<Wire>& w,double x,double y,double z){for(auto& one:w)for(auto& q:one.path){q.x+=x;q.y+=y;q.z+=z;}}
inline bool Empty(const Plan& p){return p.entryRoute.empty()&&p.exitRoute.empty()&&p.entrance.empty()&&p.departure.empty()&&p.unsupported.empty();}
inline bool Valid(const Plan& p){if(!p.unsupported.empty()||p.entryRoute.size()>200||p.exitRoute.size()>200||p.entryLane< -1||p.entryLane>15||p.exitLane< -1||p.exitLane>15||!std::isfinite(p.width)||!std::isfinite(p.height)||p.width<1||p.width>100||p.height<1||p.height>200||p.entrance.size()>128||p.departure.size()>128)return false;for(const auto* path:{&p.entrance,&p.departure})for(const auto& point:*path)if(!std::isfinite(point.x)||!std::isfinite(point.y)||!std::isfinite(point.z))return false;return true;}
inline std::string Write(const Plan& p){if(!p.unsupported.empty())return p.unsupported;if(Empty(p))return {};std::ostringstream out;out<<std::setprecision(17)<<1<<' '<<std::quoted(p.entryRoute)<<' '<<std::quoted(p.exitRoute)<<' '<<p.entryLane<<' '<<p.exitLane<<' '<<p.width<<' '<<p.height;for(const auto* path:{&p.entrance,&p.departure}){out<<' '<<path->size();for(const auto& point:*path)out<<' '<<point.x<<' '<<point.y<<' '<<point.z;}return out.str();}
inline Plan Read(const std::string& text){Plan p;if(text.empty())return p;std::istringstream in(text);int v=0;if(in>>v>>std::quoted(p.entryRoute)>>std::quoted(p.exitRoute)>>p.entryLane>>p.exitLane>>p.width>>p.height&&v==1){bool ok=true;for(auto* path:{&p.entrance,&p.departure}){std::size_t n=0;if(!(in>>n)||n>128){ok=false;break;}for(std::size_t i=0;i<n;++i){Point point;if(!(in>>point.x>>point.y>>point.z)){ok=false;break;}path->push_back(point);}}in>>std::ws;if(ok&&in.eof()&&Valid(p))return p;}p={};p.unsupported=text;return p;}
inline void Move(Plan& p,double x,double y,double z){for(auto* path:{&p.entrance,&p.departure})for(auto& point:*path){point.x+=x;point.y+=y;point.z+=z;}}
inline std::vector<Point> Path(const Plan& p,bool entering,Point current,Point destination){auto out=entering?p.entrance:p.departure;if(out.empty())return {};out.insert(out.begin(),current);out.push_back(destination);std::vector<Point> clean;for(const auto& p:out)if(clean.empty()||std::hypot(p.x-clean.back().x,p.y-clean.back().y,p.z-clean.back().z)>.001)clean.push_back(p);return clean;}
}
