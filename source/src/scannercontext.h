#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
namespace scannercontext {
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
