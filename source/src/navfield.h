#pragma once
#include <vector>
#include <limits>
namespace navfield {
struct Field {
 int ox=0,oy=0;
 double gx=0,gy=0,z=0,width=0,gz=std::numeric_limits<double>::quiet_NaN(),bottom=0;
 bool objects=true;
 unsigned long long built=0;
 std::vector<float> cost;
 double Value(double x,double y) const;
 double Value(double x,double y,double height) const;
};
const Field& Get(double x,double y,double z,double gx,double gy,double width,bool objects,
 double gz=std::numeric_limits<double>::quiet_NaN());
void Service(double milliseconds=2.0); // main-thread raster budget; search runs on an owned worker (never waited for)
bool Idle(); // no raster copying or worker search outstanding (tests drain with Service until Idle)
void Reset();
}
