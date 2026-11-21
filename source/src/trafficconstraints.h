#pragma once
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
namespace trafficconstraints {
struct Rules {int modes=3,closure=0;double speed=0,minHeight=0,maxHeight=200;int start=-1,end=-1;bool noLaneChange=false;std::string unsupported;};
inline bool Valid(const Rules& r){return r.unsupported.empty()&&r.modes>=0&&r.modes<=3&&r.closure>=0&&r.closure<=2&&std::isfinite(r.speed)&&r.speed>=0&&r.speed<=200&&std::isfinite(r.minHeight)&&std::isfinite(r.maxHeight)&&r.minHeight>=0&&r.maxHeight>=r.minHeight&&r.maxHeight<=200&&r.start>=-1&&r.start<1440&&r.end>=-1&&r.end<1440&&((r.start<0)==(r.end<0));}
inline bool Empty(const Rules& r){return r.modes==3&&r.closure==0&&r.speed==0&&r.minHeight==0&&r.maxHeight==200&&r.start<0&&r.end<0&&!r.noLaneChange&&r.unsupported.empty();}
inline std::string Write(const Rules& r){if(!r.unsupported.empty())return r.unsupported;if(Empty(r))return {};std::ostringstream o;o<<std::setprecision(17)<<1<<' '<<r.modes<<' '<<r.closure<<' '<<r.speed<<' '<<r.minHeight<<' '<<r.maxHeight<<' '<<r.start<<' '<<r.end<<' '<<r.noLaneChange;return o.str();}
inline Rules Read(const std::string& text){Rules r;if(text.empty())return r;std::istringstream in(text);int v=0,noChange=0;if(in>>v>>r.modes>>r.closure>>r.speed>>r.minHeight>>r.maxHeight>>r.start>>r.end>>noChange&&v==1&&noChange>=0&&noChange<=1){r.noLaneChange=noChange!=0;in>>std::ws;if(in.eof()&&Valid(r))return r;}r={};r.unsupported=text;return r;}
inline bool Scheduled(const Rules& r,int minute){if(r.start<0)return true;if(minute<0)return false;if(r.start==r.end)return true;return r.start<r.end?minute>=r.start&&minute<r.end:minute>=r.start||minute<r.end;}
struct Effective {int modes=3,closure=0;double speed=200,minHeight=0,maxHeight=200;bool noLaneChange=false,valid=true,admit=true,extended=false;const char* reason="open";};
inline void Apply(Effective& e,const Rules& r,int minute){e.extended=e.extended||!Empty(r);if(!Valid(r)){e.valid=false;e.admit=false;e.reason="unsupported or invalid restriction";return;}e.modes&=r.modes;e.closure=std::max(e.closure,r.closure);if(r.speed>0)e.speed=std::min(e.speed,r.speed);e.minHeight=std::max(e.minHeight,r.minHeight);e.maxHeight=std::min(e.maxHeight,r.maxHeight);e.noLaneChange=e.noLaneChange||r.noLaneChange;if(!e.modes||e.minHeight>e.maxHeight){e.valid=false;e.reason="no compatible travel mode or height band";}else if(e.closure==2)e.reason="closed; hold";else if(e.closure==1)e.reason="closed to new journeys; draining";if(!Scheduled(r,minute)){e.admit=false;e.reason=minute<0?"schedule clock unavailable":"outside admission window";}e.admit=e.admit&&e.valid&&e.closure==0;}
inline const char* Modes(int mask){return mask==1?"Drive only":mask==2?"Hover only":mask==3?"Drive and hover":"No travel";}
inline const char* Closure(int value){return value==1?"Drain":value==2?"Hold":"Open";}
}
