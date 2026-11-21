// One tile's field box (tilebox.h): the shared placement for the provider and the F7 tile check.
#include "tilebox.h"
#include <cstdio>
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
static bool Near(double a, double b, double e = .05) { return std::fabs(a - b) <= e; }

int main() {
    trafficdata::Route r; r.ring = false; r.lanes = 2; r.corridor = 5; r.travel = routepolicy::Travel::Both;  // a stored route defaults to Hover only (all Avoid); this one drives
    r.points = { { 0, 0, 10 }, { 0, 40, 10 }, { 0, 80, 18 } };
    auto line = trafficroute::Build(r);
    FvoFieldBoxV1 f;
    Check(!line.error, "a plain route builds");
    Check(tilebox::TileBox(r, line, 0, f) && fieldboxapi::IsDefault(f), "old route gets marked default");
    Check(Near(f.halfWidth*2, 25) && f.faces == 1, "default covers lane spacing plus corridor; bottom Touch");
    Check(r.road.boxes.empty(), "reading never rewrites route");
    fieldbox::Box converted; Check(fieldboxapi::ToBox(f, converted), "V2 geometry accepted");
    auto legacy=f; legacy.version=1; legacy.isDefault=1;
    Check(fieldboxapi::ToBox(legacy, converted) && !fieldboxapi::IsDefault(legacy), "V1 geometry accepted; old padding never provenance");
    auto old=r; old.travel=routepolicy::Travel::Hover; old.road.height=9;
    Check(tilebox::TileBox(old,line,0,f) && f.faces==0 && Near(f.halfHeight*2,9), "hover default all Avoid and road height");
    old.sections.resize(2); old.sections[0].travel=routepolicy::Travel::Drive;
    Check(tilebox::TileBox(old,line,0,f) && f.faces==1, "tile travel overrides route travel");
    const auto identity=old.name; const auto count=old.points.size();
    const int expected=tilebox::TileCount(old);
    Check(tilebox::MaterializeMissing(old)==expected && tilebox::MaterializeMissing(old)==0, "update is idempotent");
    Check(old.name==identity && old.points.size()==count && old.road.boxes[0].faces==1, "update keeps identity geometry and per tile travel");
    Check(tilebox::TileBox(old,line,0,f) && !fieldboxapi::IsDefault(f), "materialized box is authored");
    {   // FW-1: an authored box narrower than the lanes (the edited lc_ring_road: 11 m boxes on a 15.1 m road) is widened to the lanes; the route keeps its 11.
        auto narrow = old; for (auto& b : narrow.road.boxes) b.width = 11;
        Check(tilebox::TileBox(narrow,line,0,f) && Near(f.halfWidth*2, tilebox::LaneSpanWidth(narrow)) && narrow.road.boxes[0].width==11, "FW-1: a field is never narrower than its lanes");
        for (auto& b : narrow.road.boxes) b.width = 40;
        Check(tilebox::TileBox(narrow,line,0,f) && Near(f.halfWidth*2, 40), "FW-1: a wider authored box keeps its width"); }
    old=r; old.road.primitives.resize(2); old.road.primitives[1].kind=roadplan::Tube;
    Check(!tilebox::TileBox(old,line,1,f) && tilebox::MaterializeMissing(old)==-1 && old.road.boxes.empty(), "D11 excluded and failed update atomic");
    old=r; old.road.boxesUnsupported="future";
    Check(tilebox::MaterializeMissing(old)==-1 && old.road.boxesUnsupported=="future", "future format preserved");
    roadplan::FieldBox b; b.faces = 1; r.road.boxes = { b, b }; r.width = 4;  // lanes 4 m apart: they span 9 m, inside the widths below (FW-1)
    line = trafficroute::Build(r);
    Check(tilebox::TileBox(r, line, 0, f), "an explicit box is answered without road tiles switched on");
    Check(Near(f.halfWidth * 2, 10) && Near(f.halfHeight * 2, 5), "width 0 is the road's width (corridor x lanes), height 0 the 5 m roof");
    Check(Near(f.cx, 0, .3) && Near(f.cy, 20, 1.0) && Near(f.cz - f.halfHeight, 10, .3), "on the route line, bottom face on it, centred on the tile");
    Check(Near(f.halfLength * 2, 40, 1.0) && Near(f.yaw, 0, .02) && Near(f.pitch, 0, .02), "length and direction from the tile's points");
    Check(tilebox::TileBox(r, line, 1, f) && f.pitch > .15, "a rising tile is pitched up");
    Check(!tilebox::TileBox(r, line, 2, f), "past the last tile: none");
    r.road.width = 14; Check(tilebox::TileBox(r, line, 0, f) && Near(f.halfWidth * 2, 14), "road.width wins when set");
    r.road.boxes[0].width = 6; r.road.boxes[0].yaw = 90; Check(tilebox::TileBox(r, line, 0, f) && Near(f.halfWidth * 2, 9) && Near(f.yaw, 1.5708, .03), "its own yaw offset, degrees to radians; FW-1: its 6 m width widened to the 9 m its lanes span");
    const auto authored=r.road.boxes[0];
    Check(tilebox::MaterializeMissing(r)>=0 && r.road.boxes[0].width==authored.width && r.road.boxes[0].yaw==authored.yaw, "update preserves authored box");
    r.road.boxesUnsupported = "junk"; Check(!tilebox::TileBox(r, line, 0, f), "unreadable road_boxes: treated as not authored");
    std::printf("%s\n", failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
