#pragma once
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
namespace journeyplan {
enum class Wait { Arrival, TrafficGap, Reservation, Trigger };
struct Stop { double x=0,y=0,z=0,dwell=5;double speed=0,timeout=0;int mode=0;Wait wait=Wait::Arrival;unsigned trigger=1;int fallback=-1; };
struct Plan { bool enabled=false,loop=false;std::vector<Stop> stops;std::string unsupported; };
inline const char* Name(Wait w){switch(w){case Wait::TrafficGap:return "Traffic gap";case Wait::Reservation:return "Reservation";case Wait::Trigger:return "Trigger";default:return "Arrival";}}
inline bool Extended(const Stop& s){return s.speed!=0||s.timeout!=0||s.mode!=0||s.wait!=Wait::Arrival||s.trigger!=1||s.fallback!=-1;}
inline bool Valid(const Plan& p){if(!p.unsupported.empty()||p.stops.size()>128)return false;for(const auto& s:p.stops)if(!std::isfinite(s.x)||!std::isfinite(s.y)||!std::isfinite(s.z)||!std::isfinite(s.dwell)||s.dwell<0||s.dwell>86400||!std::isfinite(s.speed)||s.speed<0||s.speed>200||!std::isfinite(s.timeout)||s.timeout<0||s.timeout>86400||s.mode<0||s.mode>3||int(s.wait)<0||int(s.wait)>3||s.trigger<1||s.trigger>65535||s.fallback< -2||s.fallback>=int(p.stops.size()))return false;return !p.enabled||!p.stops.empty();}
inline std::string Write(const Plan& p){if(!p.unsupported.empty())return p.unsupported;if(p.stops.empty()&&!p.enabled)return {};int version=1;for(const auto& s:p.stops)if(Extended(s))version=2;std::ostringstream o;o<<std::setprecision(17)<<version<<' '<<p.enabled<<' '<<p.loop<<' '<<p.stops.size();for(const auto& s:p.stops){o<<' '<<s.x<<' '<<s.y<<' '<<s.z<<' '<<s.dwell;if(version>=2)o<<' '<<s.speed<<' '<<s.timeout<<' '<<s.mode<<' '<<int(s.wait)<<' '<<s.trigger<<' '<<s.fallback;}return o.str();}
inline Plan Read(const std::string& text){Plan p;if(text.empty())return p;std::istringstream in(text);int v=0,enabled=0,loop=0;std::size_t n=0;if(in>>v>>enabled>>loop>>n&&(v==1||v==2)&&enabled>=0&&enabled<=1&&loop>=0&&loop<=1&&n<=128){p.enabled=enabled!=0;p.loop=loop!=0;for(std::size_t i=0;i<n;++i){Stop s;int wait=0;if(!(in>>s.x>>s.y>>s.z>>s.dwell)||(v>=2&&!(in>>s.speed>>s.timeout>>s.mode>>wait>>s.trigger>>s.fallback))){p.unsupported=text;return p;}s.wait=Wait(wait);p.stops.push_back(s);}in>>std::ws;if(in.eof()&&Valid(p))return p;}p=Plan{};p.unsupported=text;return p;}
struct Facts {bool arrived=false,gap=false,reserved=false,triggered=false;double elapsed=0;};
struct Decision {bool advance=false,end=false,timedOut=false;int next=-1;const char* reason="waiting for arrival";};
inline Decision Evaluate(const Plan& p,std::size_t stage,const Facts& f){Decision d;if(stage>=p.stops.size()||!Valid(p)){d.reason="invalid script stage";return d;}const auto& s=p.stops[stage];if(!f.arrived)return d;const bool ready=s.wait==Wait::Arrival||(s.wait==Wait::TrafficGap&&f.gap)||(s.wait==Wait::Reservation&&f.reserved)||(s.wait==Wait::Trigger&&f.triggered);if(f.elapsed>=s.dwell&&ready){d.advance=true;d.next=stage+1<p.stops.size()?int(stage+1):p.loop?0:-1;d.end=d.next<0;d.reason="script condition satisfied";return d;}d.reason=f.elapsed<s.dwell?"script dwell":s.wait==Wait::Trigger?"waiting for trigger":s.wait==Wait::Reservation?"waiting for reservation":"waiting for traffic gap";if(s.timeout>0&&f.elapsed>=s.timeout){d.timedOut=true;d.reason="script timeout; holding";if(s.fallback>=0){d.advance=true;d.next=s.fallback;d.reason="script timeout; fallback branch";}else if(s.fallback==-2){d.advance=true;d.end=true;d.reason="script timeout; end at stop";}}return d;}
inline void Remove(Plan& p,std::size_t index){if(index>=p.stops.size())return;p.stops.erase(p.stops.begin()+index);for(auto& s:p.stops){if(s.fallback==int(index))s.fallback=-1;else if(s.fallback>int(index))--s.fallback;}if(p.stops.empty())p.enabled=false;}
inline void Swap(Plan& p,std::size_t a,std::size_t b){if(a>=p.stops.size()||b>=p.stops.size())return;std::swap(p.stops[a],p.stops[b]);for(auto& s:p.stops){if(s.fallback==int(a))s.fallback=int(b);else if(s.fallback==int(b))s.fallback=int(a);}}
}
