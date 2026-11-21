#pragma once
#include <algorithm>
#include <vector>
namespace panelpaging {
struct Interval{double first=0,last=0;};
// Break only between visual rows; boxes, buttons and profile previews stay whole.
inline std::vector<double> Cuts(double first,double last,double capacity,const std::vector<Interval>& blocked){std::vector<double> out{first};if(capacity<1)return out;while(out.back()<last&&out.size()<4096){const double start=out.back();double end=std::min(last,start+capacity);bool changed=true;for(std::size_t pass=0;changed&&pass<=blocked.size();++pass){changed=false;for(auto row:blocked)if(row.first<end-1e-6&&row.last>end+1e-6&&row.first>start+1e-6){end=row.first;changed=true;}}if(end<=start+1e-6)end=std::min(last,start+capacity);out.push_back(end);}return out;}
}
