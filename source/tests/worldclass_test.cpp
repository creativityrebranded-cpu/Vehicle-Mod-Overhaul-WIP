// worldclass.h: class and context of a ray hit (world truth, Build A). Cases from the 11:38 run.
#include "worldclass.h"
#include <cstdio>
using namespace worldclass;
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
int main() {
    Hit ground; ground.layer = 17; ground.owner = 0; ground.haveNormal = true; ground.up = 0; ground.z = 101.23; ground.haveFloor = true; ground.floor = 100.5;
    Check(Classify(ground) == Class::Terrain, "layer 17 with no owner is terrain");
    Check(Explain(ground, Classify(ground), .75) == Context::Seam, "a terrain face 0.73 m over the measured floor is a seam");
    Check(WouldStop(Class::Terrain, Context::Seam) == Verdict::NoStop, "explained terrain never stops the car");
    Hit block; block.layer = 1; block.owner = 0x09006083; block.base = 0x001FA9EE; block.size = 20; block.roadBase = true;
    block.haveNormal = true; block.up = .95; block.z = 100.52; block.haveFloor = true; block.floor = 100.4;
    Check(Classify(block) == Class::Road, "a learned road base is road");
    Check(Explain(block, Classify(block), .75) == Context::Floor, "an up-facing hit on the measured floor is the floor");
    Hit rock; rock.layer = 1; rock.owner = 0xFF01CEAD; rock.size = 4; rock.haveNormal = true; rock.up = .1; rock.z = 101.4; rock.haveFloor = true; rock.floor = 100;
    Check(Classify(rock) == Class::Loose, "a runtime (FF) owner is a loose object");
    Check(WouldStop(Classify(rock), Explain(rock, Classify(rock), .75)) == Verdict::MayStop, "an unexplained rock may stop the car");
    Hit building; building.layer = 1; building.owner = 0x09010947; building.size = 90; building.haveNormal = true; building.up = 0; building.z = 101.3; building.haveFloor = true; building.floor = 100;
    Check(Classify(building) == Class::Structure, "a 90 m static owner is a structure");
    Check(WouldStop(Classify(building), Explain(building, Classify(building), .75)) == Verdict::BodyCheck, "an unexplained structure asks the body check");
    building.knownSolid = true;
    Check(Explain(building, Classify(building), .75) == Context::KnownSolid, "a structure the sweep knows is explained");
    Hit post; post.layer = 1; post.owner = 0x09001234; post.size = 1.5; Check(Classify(post) == Class::Loose, "a small static (1.5 m) is a loose object");
    Hit tree; tree.layer = 9; Check(Classify(tree) == Class::Plant, "layer 9 is a plant");
    Hit wall; wall.layer = 27; Check(Classify(wall) == Class::Barrier, "layer 27 is a barrier");
    Hit odd; odd.layer = -1; odd.owner = 0; Check(Classify(odd) == Class::Unknown, "no layer and no owner is unknown");
    Hit high = ground; high.z = 102; Check(Explain(high, Classify(high), .75) == Context::Unexplained, "terrain 1.5 m over the floor is not explained");
    RoadSet<> rs; rs.NoteFloor(0x001FA9EE); rs.NoteFloor(0x001FA9EE); Check(!rs.Has(0x001FA9EE), "road set: two floor hits are not enough");
    rs.NoteFloor(0x001FA9EE); Check(rs.Has(0x001FA9EE), "road set: three floor hits make it road");
    {   Hit pebble; pebble.layer = 4; pebble.owner = 0x09001111; pebble.size = 2.0; pebble.z = 100.2; pebble.haveFloor = true; pebble.floor = 100;
        Check(DriveOver(pebble, Classify(pebble)), "drive over: anything loose 0.2 m over the floor");
        Hit can; can.layer = 4; can.owner = 0x09001112; can.size = .4; can.z = 100.3; can.haveFloor = true; can.floor = 100;
        Check(DriveOver(can, Classify(can)), "drive over: a 0.4 m object 0.3 m high (under half the one tyre, 0.35 m)");
        Hit crate; crate.layer = 4; crate.owner = 0x09001113; crate.size = .9; crate.z = 100.6; crate.haveFloor = true; crate.floor = 100;
        Check(!DriveOver(crate, Classify(crate)), "not over: a 0.9 m crate 0.6 m high");
        Hit nofloor = pebble; nofloor.haveFloor = false; Check(!DriveOver(nofloor, Classify(nofloor)), "not over: no measured floor");
        Hit wall; wall.layer = 1; wall.owner = 0x09010947; wall.size = 90; wall.z = 100.2; wall.haveFloor = true; wall.floor = 100;
        Check(!DriveOver(wall, Classify(wall)), "not over: a structure is never driven over by this rule"); }
    { Hit h; h.haveFloor=true; h.floor=0; h.z=.175;
      Check(UnderQuarterTire(h, Class::Loose), "quarter tire: boundary adds no slowdown");
      h.z=.176; Check(!UnderQuarterTire(h, Class::Loose), "above quarter: not discarded");
      h.z=.35; h.size=4; Check(DriveOver(h, Class::Loose), "half tire (0.35 m, the one car's climb): wide low object can climb");
      h.z=.36; Check(!DriveOver(h, Class::Loose), "over half the one tyre: not climbed");
      h.z=.501; Check(!DriveOver(h, Class::Loose), "above half: not drive-over");
      h.z=.2; Check(!UnderQuarterTire(h, Class::Vehicle) && !DriveOver(h, Class::Vehicle), "vehicle excluded");
      Check(!UnderQuarterTire(h, Class::Person) && !DriveOver(h, Class::Person), "person excluded");
      h.haveFloor=false; Check(!UnderQuarterTire(h, Class::Loose) && !DriveOver(h, Class::Loose), "unmeasured floor excluded"); }
    {   // RT-1 (2026-11-18, 11:57 ramp): deck strips (base learned as road) vs wall strips (another base) vs an owner not answered yet.
        Hit deck; deck.layer = 1; deck.owner = 0x06002BA9; deck.base = 0x1FA9EE; deck.size = 20; deck.roadBase = true; deck.haveNormal = true; deck.up = .39;
        deck.haveFloor = true; deck.floor = -140.3; deck.z = -139.27;   // 1.03 m over the measured floor, as at the stall
        Check(Classify(deck) == Class::Road && WouldStop(Classify(deck), Explain(deck, Classify(deck), .75)) == Verdict::NoStop, "RT-1: a deck point on a road base never stops the car");
        Hit wall = deck; wall.owner = 0x06007FFD; wall.base = 0x1FA9EF; wall.roadBase = false; wall.up = 0;
        Check(Classify(wall) == Class::Structure && WouldStop(Classify(wall), Explain(wall, Classify(wall), .75)) != Verdict::NoStop, "RT-1: a wall strip (another base) still may stop it");
        Hit pending; pending.layer = -1; pending.owner = 0; pending.haveFloor = true; pending.floor = 0; pending.z = 1;
        Check(WouldStop(Classify(pending), Explain(pending, Classify(pending), .75)) == Verdict::MayStop, "RT-1: an owner not answered yet (layer -1) may stop it");
        RoadSet<> rs; for (int i = 0; i < rules::roadLearn; ++i) rs.NoteFloor(0x1FA9EE);
        Check(rs.Has(0x1FA9EE) && !rs.Has(0x1FA9EF), "RT-1: riding on a base learns it as road; the wall's base is not");
    }
    std::printf("worldclass_test: %s\n", failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
