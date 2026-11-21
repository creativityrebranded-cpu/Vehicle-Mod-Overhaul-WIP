#pragma once
// Three-layer F7 editing (2026-11-03, notes/route-fields-20261103/F7_THREE_LAYER_SPEC.md): pure helpers shared by
// the editor and its headless layout test. The stroke font has no middle dot, en dash or arrow: " - " and "->" stand in.
#include "roadplan.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace f7layers {
enum Group { Field=0, Flow=1, Position=2, Rules=3 };
inline const char* GroupName(int g) {constexpr const char* n[]{"Field","Flow","Position","Rules"};return n[std::clamp(g,0,3)];}
inline const char* ShortKind(int kind) {constexpr const char* n[]{"Default","Flat","Box","Round","Tube","Trap","Open"};return kind>=0&&kind<=6?n[kind]:"?";}
inline const char* LinkName(int connector) {return connector==1?"Bend":connector==3?"Taper":"Sharp";}
// Pixels per letter of editor text: strokefont::kAdvance (Future face, 6.0) x the editor's kUnit (1.5). One value for every
// fit and cut (2026-11-11; it was the literal 8.25 of the Classic face in 30 places). routeedit.cpp asserts it matches.
inline constexpr double kLetterPx = 9.0;
// Cut to `width` px at `advance` px a letter; a cut line ends in "...".
inline std::string Fit(const std::string& s,double width,double advance) {
    const int fit=int(width/std::max(1e-9,advance));
    if(int(s.size())<=fit)return s;
    if(fit<=3)return s.substr(0,std::size_t(std::max(0,fit)));
    return s.substr(0,std::size_t(fit-3))+"...";
}
// Cross-section outline for the dock preview in (s, h): s across -1..1, h up from the floor, roof at heightRatio.
inline std::vector<std::pair<double,double>> Outline(const roadplan::Primitive& k,double heightRatio) {
    constexpr double kPi=3.141592653589793;
    std::vector<std::pair<double,double>> o;const double H=std::max(.05,heightRatio);
    switch(k.kind){
    case roadplan::Flat:o={{-1,0},{1,0},{1,.04},{-1,.04}};break;
    case roadplan::Tube:for(int i=0;i<=16;++i){const double a=kPi*i/16;o.push_back({std::cos(a),H*std::sin(a)});}break;
    case roadplan::Trapezoid:{const double t=std::clamp(k.param,.1,1.0);o={{-1,0},{1,0},{t,H},{-t,H}};break;}
    case roadplan::Rounded:{const double r=std::clamp(k.param/8.0,0.05,std::min(1.0,H));o={{-1,0},{1,0}};
        for(int i=0;i<=6;++i){const double a=kPi/2*i/6;o.push_back({1-r+r*std::cos(a),H-r+r*std::sin(a)});}
        for(int i=0;i<=6;++i){const double a=kPi/2+kPi/2*i/6;o.push_back({-1+r+r*std::cos(a),H-r+r*std::sin(a)});}break;}
    default:o={{-1,0},{1,0},{1,H},{-1,H}};break;
    }
    return o;
}
// A cross-section point at `depth` along the road, turned by `turn` about the vertical, seen slightly from above.
inline std::pair<double,double> Project(double s,double h,double depth,double turn,double cx,double cy,double scale) {
    const double x=s*std::cos(turn)+depth*std::sin(turn),z=-s*std::sin(turn)+depth*std::cos(turn);
    return {cx+x*scale,cy-h*scale+z*scale*.35};
}
}
