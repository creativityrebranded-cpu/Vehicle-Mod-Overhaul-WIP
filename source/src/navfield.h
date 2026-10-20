#pragma once
#include <vector>
namespace navfield {
struct Field {
 int ox=0, oy=0;
 double gx=0, gy=0, z=0, width=0;
 bool objects=true;
 unsigned long long built=0;
 std::vector<double> cost;
 double Value(double x, double y) const;
};
const Field& Get(double x, double y, double z, double gx, double gy, double width, bool objects);
void Reset();
}
