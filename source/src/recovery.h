#pragma once
#include <cmath>
#include <cstdint>
namespace recovery {
struct Budget {
 bool initialized=false, failed=false;
 unsigned attempts=0;
 std::uint64_t since=0, watch=0;
 double gx=0,gy=0,gz=0,x=0,y=0,z=0;
 void Reset(){*this=Budget{};}
 bool Update(std::uint64_t now,double px,double py,double pz,double tx,double ty,double tz,bool reached,bool waiting) {
  if(!initialized || std::hypot(tx-gx,ty-gy)>8 || std::fabs(tz-gz)>4) {
   Reset(); initialized=true; since=watch=now; x=px;y=py;z=pz;gx=tx;gy=ty;gz=tz;
  }
  if(failed) return false;
  if(reached) {attempts=0;watch=since=now;return false;}
  // Missing world data cannot consume attempts. It also cannot carry an old timer into a ready frame.
  if(waiting) {watch=since=now;x=px;y=py;z=pz;return false;}
  if(std::hypot(std::hypot(px-x,py-y),pz-z)>4) {watch=now;x=px;y=py;z=pz;}
  if(now-watch>=20000) {watch=now;if(++attempts>=3) failed=true;return !failed;}
  if(now-since>=600000) failed=true;
  return false;
 }
};
}
