#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <cstdlib>
#include <cctype>
namespace scannercontext {
inline double RangeMeters(std::string_view shown) {
    std::string number;
    std::size_t i=0;
    while(i<shown.size() && std::isspace(static_cast<unsigned char>(shown[i]))) ++i;
    for(;i<shown.size();++i) {
        const char c=shown[i];
        if(c==',') continue; // English UI thousands separator
        if((c>='0'&&c<='9')||c=='.') number+=c;
        else break;
    }
    while(i<shown.size() && std::isspace(static_cast<unsigned char>(shown[i]))) ++i;
    const auto unit=shown.substr(i);
    double scale=1;
    if(unit=="km" || unit=="KM") scale=1000;
    else if(!unit.empty() && unit!="m" && unit!="M") return 0;
    char* end=nullptr;
    const double value=std::strtod(number.c_str(),&end)*scale;
    return end && *end==0 && std::isfinite(value) && value>0 && value<=100000 ? value : 0;
}
inline double Range3D(double x,double y,double z) { return std::hypot(std::hypot(x,y),z); }
// A selected, named POI wins over a nearer collision hit. A nearby-area label is never supplied here.
inline double PoiScore(std::string_view name, std::string_view selected, double along, double lateral,
    double hitDistance, double range, double selectedRange)
{
    const double reject = std::numeric_limits<double>::infinity();
    if (along < 20) return reject;
    const double tolerance = std::max(80.0, 0.03 * along);
    if (lateral > tolerance) return reject;
    if (!selected.empty()) {
        if (name != selected) return reject;
        // Distinguish repeated location names using the distance the game's selected marker shows.
        if (selectedRange > 0 && std::fabs(range - selectedRange) > std::max(80.0, selectedRange * 0.05)) return reject;
    } else if (hitDistance >= 0 && hitDistance < along - tolerance) return reject;
    return lateral / tolerance + (!selected.empty() && selectedRange > 0 ? std::fabs(range-selectedRange)/std::max(80.0,selectedRange*0.05) : 0);
}
}
