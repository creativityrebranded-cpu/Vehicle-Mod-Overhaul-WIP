#pragma once
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace roadplan {
// connector: the join at the end of the tile. 1 rounded bend, 2 sharp join, 3 taper (sharp join whose
// first quarter of the next tile must fit both cross-sections).
struct Tile { double side=0, swing=0, tilt=0; int connector=2; };
// Cross-section of the tile that starts at a point (F7 1-5). 0 default: flat when the road has no
// height, box when it has. param: rounded box corner radius (m); trapezoid top width / bottom width.
// Open (6): an open-air stretch such as a vertical shaft: no floor, roof or walls; vehicles hover through it.
enum Kind { Default=0, Flat=1, Box=2, Rounded=3, Tube=4, Trapezoid=5, Open=6 };
struct Primitive { int kind=Default; double param=0; };
inline const char* KindName(int kind) {constexpr const char* n[]{"default","flat tile","box","rounded box","tube","trapezoid","open air"};return kind>=0&&kind<=6?n[kind]:"unknown";}
inline double DefaultParam(int kind) {return kind==Rounded?1.5:kind==Trapezoid?.6:0;}
// A tile's field box (road_boxes, 2026-11-06, format agreed by the editor and driving sessions; notes/VEHICLE_DECISION_DESIGN_20261106.md
// section 3). width, height: metres, 0 = the road's own. yaw, pitch: degree offsets from what the tile's points give, 0 = follow the line;
// roll: degrees. faces: bit 0 bottom, 1 top, 2 left, 3 right, 4 front, 5 back (fieldbox::Face order), 1 = Touch, 0 = Avoid. solids: 1 =
// solids inside are allowed. Length is never stored: it comes from the tile's points.
struct FieldBox { double width=0, height=0, yaw=0, pitch=0, roll=0; int faces=1, solids=0; };
inline constexpr int kFaceBottom=1, kAllAvoid=0;
inline bool ValidBox(const FieldBox& b) {
    auto angle=[](double v){return std::isfinite(v)&&v>=-180&&v<=180;};
    return std::isfinite(b.width)&&(b.width==0||(b.width>=.5&&b.width<=400))&&std::isfinite(b.height)&&(b.height==0||(b.height>=.5&&b.height<=200))&&
           angle(b.yaw)&&angle(b.pitch)&&angle(b.roll)&&b.faces>=0&&b.faces<=63&&(b.solids==0||b.solids==1);
}
// Faces a tile has before anyone sets them: Hover only (travel 2, routepolicy::Travel::Hover) is all Avoid, anything else bottom Touch.
inline FieldBox DefaultBox(bool hoverOnly) { FieldBox b; b.faces=hoverOnly?kAllAvoid:kFaceBottom; return b; }
struct Plan {
    bool enabled=false;
    double height=0;
    std::vector<Tile> tiles;
    std::vector<std::vector<Tile>> dividers; // Explicit divider rows; empty reads legacy single-divider roads.
    double width=0;                          // Whole road width in metres; 0 = lane corridor x lanes (older roads).
    std::vector<int> directions;             // Per lane, right edge first: 1 along the markers, -1 against; empty = default.
    std::vector<Primitive> primitives;       // Per tile; empty or Default reads as before (flat / box by height).
    double median=0;                         // Width of every red divider in metres (a median between lanes); 0 = a line.
    double edgeLeft=0,edgeRight=0;           // No-go band along the road's left / right edge in metres (left/right of the markers); 0 = none (older roads).
    std::string unsupported;
    // Field boxes, one per tile, indexed like `tiles` (tile i starts at point i). Not part of road_tiles: written as its own key
    // road_boxes (WriteBoxes / ReadBoxes), so road_tiles and every older reader stay as they were. Empty: no key, the runtime derives.
    std::vector<FieldBox> boxes;
    std::string boxesUnsupported;            // road_boxes text this build cannot read: kept and written back unchanged
};
inline bool Valid(const Plan& p) {
    if(!p.unsupported.empty() || !std::isfinite(p.height) || p.height<0 || p.height>200)return false;
    for(const auto& t:p.tiles)
        if(!std::isfinite(t.side)||!std::isfinite(t.swing)||!std::isfinite(t.tilt)||
           std::abs(t.side)+std::abs(t.swing)>1.000001||std::abs(t.tilt)>75||
           (t.connector<1||t.connector>3))return false;
    if(p.primitives.size()>10000)return false;
    for(const auto& k:p.primitives){if(k.kind<Default||k.kind>Open||!std::isfinite(k.param))return false;
        if(k.kind==Rounded&&(k.param<0||k.param>50))return false;if(k.kind==Trapezoid&&(k.param<.1||k.param>1))return false;}
    if(p.dividers.size()>15||p.directions.size()>16)return false;
    if(!std::isfinite(p.width)||(p.width!=0&&(p.width<1||p.width>400)))return false;
    if(!std::isfinite(p.median)||p.median<0||p.median>50)return false;
    if(!std::isfinite(p.edgeLeft)||!std::isfinite(p.edgeRight)||p.edgeLeft<0||p.edgeRight<0||p.edgeLeft>50||p.edgeRight>50)return false;
    for(int d:p.directions)if(d!=1&&d!=-1)return false;
    for(const auto& row:p.dividers)for(const auto& t:row)if(!std::isfinite(t.side)||!std::isfinite(t.swing)||!std::isfinite(t.tilt)||std::abs(t.side)+std::abs(t.swing)>1.000001||std::abs(t.tilt)>75)return false;
    return true;
}
// A non-default cross-section or a taper join needs the v4 record; anything else keeps the older one.
inline bool NeedsV4(const Plan& p) {
    for(const auto& k:p.primitives)if(k.kind!=Default)return true;
    for(const auto& t:p.tiles)if(t.connector==3)return true;
    return p.median>0||p.edgeLeft>0||p.edgeRight>0;
}
inline std::string Write(const Plan& p) {
    if(!p.unsupported.empty())return p.unsupported;
    if(!p.enabled&&p.tiles.empty()&&p.height==0&&p.dividers.empty()&&p.directions.empty()&&p.width==0)return {};
    std::ostringstream out;
    const bool v4=NeedsV4(p),v3=v4||!p.dividers.empty()||!p.directions.empty()||p.width!=0;
    out<<std::setprecision(17)<<(v4?"4 ":v3?"3 ":"2 ")<<int(p.enabled)<<" "<<p.height<<" "<<p.tiles.size();
    for(const auto& t:p.tiles)out<<" "<<t.side<<" "<<t.swing<<" "<<t.tilt<<" "<<t.connector;
    if(v3){out<<" "<<p.dividers.size();for(const auto& row:p.dividers){out<<" "<<row.size();for(const auto& t:row)out<<" "<<t.side<<" "<<t.swing<<" "<<t.tilt<<" "<<t.connector;}
        out<<" "<<p.directions.size();for(int d:p.directions)out<<" "<<d;
        if(v4){out<<" "<<p.width<<" "<<p.primitives.size();for(const auto& k:p.primitives)out<<" "<<k.kind<<" "<<k.param;if(p.median>0||p.edgeLeft>0||p.edgeRight>0)out<<" "<<p.median;if(p.edgeLeft>0||p.edgeRight>0)out<<" "<<p.edgeLeft<<" "<<p.edgeRight;}  // median only when set: older v4 readers keep reading
        else if(p.width!=0)out<<" "<<p.width;}
    return out.str();
}
inline Plan Read(const std::string& value) {
    Plan p;
    if(value.empty())return p;
    std::istringstream in(value);
    int version=0;std::size_t count=0;
    int enabled=1;
    if(!(in>>version)||(version<1||version>4)||(version>=2&&(!(in>>enabled)||enabled<0||enabled>1))||!(in>>p.height>>count)||count>10000){p.unsupported=value;return p;}
    p.enabled=enabled!=0;p.tiles.resize(count);
    for(auto& t:p.tiles)if(!(in>>t.side>>t.swing>>t.tilt>>t.connector)){p.unsupported=value;return p;}
    if(version>=3){std::size_t rows=0;if(!(in>>rows)||rows>15){p.unsupported=value;return p;}p.dividers.resize(rows);for(auto& row:p.dividers){std::size_t n;if(!(in>>n)||n>10000){p.unsupported=value;return p;}row.resize(n);for(auto& t:row)if(!(in>>t.side>>t.swing>>t.tilt>>t.connector)){p.unsupported=value;return p;}}
        std::size_t lanes=0;if(!(in>>lanes)||lanes>16){p.unsupported=value;return p;}p.directions.resize(lanes);for(auto& d:p.directions)if(!(in>>d)){p.unsupported=value;return p;}
        if(version==4){std::size_t kinds=0;if(!(in>>p.width>>kinds)||kinds>10000){p.unsupported=value;return p;}p.primitives.resize(kinds);
            for(auto& k:p.primitives)if(!(in>>k.kind>>k.param)){p.unsupported=value;return p;}
            if(!(in>>p.median)){p.median=0;in.clear();}
            else if(!(in>>p.edgeLeft>>p.edgeRight)){p.edgeLeft=p.edgeRight=0;in.clear();}}
        else if(!(in>>p.width))p.width=0;}
    std::string extra;
    if((in>>extra)||!Valid(p))p.unsupported=value;
    return p;
}
inline Tile At(const Plan& p,std::size_t index) {return index<p.tiles.size()?p.tiles[index]:Tile{};}
// road_boxes = 1 <tiles> then per tile: width height yaw pitch roll faces solids. `tiles` is the route's tile count (points for a loop,
// points - 1 for an open path); a box not stored writes as `fallback` for that tile.
inline std::string WriteBoxes(const Plan& p,std::size_t tiles,const std::vector<FieldBox>& fallback={}) {
    if(!p.boxesUnsupported.empty())return p.boxesUnsupported;
    if(p.boxes.empty())return {};
    std::ostringstream out;out<<std::setprecision(17)<<"1 "<<tiles;
    for(std::size_t i=0;i<tiles;++i){const FieldBox b=i<p.boxes.size()?p.boxes[i]:i<fallback.size()?fallback[i]:FieldBox{};
        out<<" "<<b.width<<" "<<b.height<<" "<<b.yaw<<" "<<b.pitch<<" "<<b.roll<<" "<<b.faces<<" "<<b.solids;}
    return out.str();
}
// false: the text is not a version this build reads, or a value is out of range. It is then kept whole in boxesUnsupported.
inline bool ReadBoxes(const std::string& value,Plan& p) {
    p.boxes.clear();p.boxesUnsupported.clear();
    if(value.empty())return true;
    std::istringstream in(value);int version=0;std::size_t count=0;
    auto fail=[&]{p.boxes.clear();p.boxesUnsupported=value;return false;};
    if(!(in>>version)||version!=1||!(in>>count)||count>10000)return fail();
    p.boxes.resize(count);
    for(auto& b:p.boxes)if(!(in>>b.width>>b.height>>b.yaw>>b.pitch>>b.roll>>b.faces>>b.solids)||!ValidBox(b))return fail();
    std::string extra;if(in>>extra)return fail();
    return true;
}
inline FieldBox BoxAt(const Plan& p,std::size_t index,bool hoverOnly) {return index<p.boxes.size()?p.boxes[index]:DefaultBox(hoverOnly);}
// Reversed markers: the box stays where it is, so its own left / right and front / back swap, and pitch and roll change sign (as tilt does).
inline FieldBox Reversed(FieldBox b) {
    const int f=b.faces;auto bit=[&](int i){return (f>>i)&1;};
    b.faces=bit(0)|bit(1)<<1|bit(3)<<2|bit(2)<<3|bit(5)<<4|bit(4)<<5;b.pitch=-b.pitch;b.roll=-b.roll;return b;
}
// The cross-section a tile actually has: Default resolves by road height.
inline Primitive PrimitiveAt(const Plan& p,std::size_t index) {
    auto k=index<p.primitives.size()?p.primitives[index]:Primitive{};
    if(k.kind==Default)k.kind=p.height>0?Box:Flat;
    if(k.kind!=Flat&&k.kind!=Open&&p.height<=0)k.kind=Flat;  // no roof: every shape is its floor
    if(k.param==0)k.param=DefaultParam(k.kind);
    return k;
}
inline Tile DividerAt(const Plan& p,std::size_t divider,std::size_t tile) {
    if(p.dividers.empty())return At(p,tile);
    if(divider>=p.dividers.size())return {};
    const auto& row=p.dividers[divider];
    return tile<row.size()?row[tile]:Tile{};
}
inline int Direction(const Plan& p,int lane,int lanes) {
    if(lane>=0&&lane<int(p.directions.size()))return p.directions[lane];
    return lane<(lanes+1)/2?1:-1;
}
inline void SetDividerCount(Plan& p,int count,std::size_t points) {
    count=std::clamp(count,1,15);
    const auto old=p;
    p.directions.clear();  // new lanes start from the default split
    p.dividers.resize(count);
    for(int d=0;d<count;++d){
        p.dividers[d].resize(points);
        for(std::size_t i=0;i<points;++i){
            p.dividers[d][i]=DividerAt(old,0,i);
            p.dividers[d][i].side=-1+2.0*(d+1)/(count+1);
            p.dividers[d][i].swing=0;
            p.dividers[d][i].tilt=0;
        }
    }
}
inline void Insert(Plan& p,std::size_t index) {
    // A new point splits a tile: both halves keep its box. Boxes follow points whether or not road tiles are on.
    if(!p.boxes.empty()&&p.boxesUnsupported.empty()){p.boxes.resize(std::max(p.boxes.size(),index));const FieldBox b=index&&index-1<p.boxes.size()?p.boxes[index-1]:index<p.boxes.size()?p.boxes[index]:FieldBox{};p.boxes.insert(p.boxes.begin()+std::ptrdiff_t(index),b);}
    if(!p.enabled)return;
    p.tiles.resize(std::max(p.tiles.size(),index));
    const auto tile=index?At(p,index-1):Tile{};
    p.tiles.insert(p.tiles.begin()+index,tile);
    for(auto& row:p.dividers){row.resize(std::max(row.size(),index));const auto value=index?row[index-1]:Tile{};row.insert(row.begin()+index,value);}
    // A new point carries the previous point's cross-section over.
    if(!p.primitives.empty()||index==0){p.primitives.resize(std::max(p.primitives.size(),index));const auto kind=index&&index-1<p.primitives.size()?p.primitives[index-1]:Primitive{};p.primitives.insert(p.primitives.begin()+index,kind);}
}
inline void Remove(Plan& p,std::size_t index) {
    if(index<p.boxes.size()&&p.boxesUnsupported.empty())p.boxes.erase(p.boxes.begin()+std::ptrdiff_t(index));
    if(index<p.tiles.size())p.tiles.erase(p.tiles.begin()+index);
    for(auto& row:p.dividers)if(index<row.size())row.erase(row.begin()+index);
    if(index<p.primitives.size())p.primitives.erase(p.primitives.begin()+index);
}
inline Plan Remap(const Plan& p,const std::vector<int>& indices) {
    auto out=p;out.tiles.clear();
    for(int i:indices)out.tiles.push_back(i>=0?At(p,std::size_t(i)):Tile{});
    for(std::size_t d=0;d<p.dividers.size();++d){out.dividers[d].clear();for(int i:indices)out.dividers[d].push_back(i>=0&&std::size_t(i)<p.dividers[d].size()?p.dividers[d][i]:Tile{});}
    if(!p.primitives.empty()){out.primitives.clear();for(int i:indices)out.primitives.push_back(i>=0&&std::size_t(i)<p.primitives.size()?p.primitives[i]:Primitive{});}
    if(!p.boxes.empty()&&p.boxesUnsupported.empty()){out.boxes.clear();for(int i:indices)out.boxes.push_back(i>=0&&std::size_t(i)<p.boxes.size()?p.boxes[std::size_t(i)]:FieldBox{});}
    return out;
}
inline void Reverse(Plan& p,std::size_t points,bool closed) {
    // A box belongs to its tile, which reverses with it (tiles run between points, so tile i takes tile points-2-i, mod points).
    if(points>=2&&!p.boxes.empty()&&p.boxesUnsupported.empty()){const auto old=p.boxes;p.boxes.assign(points,FieldBox{});
        for(std::size_t i=0;i<points;++i){const auto source=(2*points-2-i)%points;if(source<old.size())p.boxes[i]=Reversed(old[source]);}}
    if(!p.enabled||points<2)return;
    auto old=p;p.tiles.resize(points);
    for(std::size_t i=0;i<points;++i){
        const auto source=(2*points-2-i)%points;
        auto t=At(old,source);t.side=-t.side;t.tilt=-t.tilt;
        // A connector belongs to the end of its tile, so it follows the corner.
        t.connector=At(old,(source+points-1)%points).connector;
        p.tiles[i]=t;
    }
    if(!closed)p.tiles.back()={};
    for(std::size_t d=0;d<old.dividers.size();++d){Plan one;one.enabled=true;one.tiles=old.dividers[old.dividers.size()-1-d];Reverse(one,points,closed);p.dividers[d]=one.tiles;}
    std::swap(p.edgeLeft,p.edgeRight);  // reversed markers: the road's own left and right edges swap
    // Reversed markers swap the lanes side for side and flip travel against them: physical travel is unchanged.
    for(std::size_t k=0;k<old.directions.size();++k)p.directions[k]=-old.directions[old.directions.size()-1-k];
    // A cross-section belongs to its tile, which reverses with it.
    if(!old.primitives.empty()){p.primitives.assign(points,{});for(std::size_t i=0;i<points;++i){const auto source=(2*points-2-i)%points;if(source<old.primitives.size())p.primitives[i]=old.primitives[source];}}
}
}
