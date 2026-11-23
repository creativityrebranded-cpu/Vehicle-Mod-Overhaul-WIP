// RR-1 (roomrule.h) and SW-1 / SW-2 (retiringrecord.h), each owner step alone. Offline logic only: never proof of behaviour in game.
#include "traffic/roomrule.h"
#include "traffic/adoption_api.h"
#include "traffic/slotcookie.h"
#include "traffic/retiringrecord.h"
#include <cstdio>
#include <cstring>
static int failed = 0, checks = 0;
static void Check(bool ok, const char* what) { ++checks; std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
static bool Is(const char* a, const char* b) { return a && b && !std::strcmp(a, b); }

int main() {
    {
        using FvoAdoptionOwner::Unclaimed;
        using FvoAdoptionOwner::PlayerSavedAuto;
        using FvoAdoptionOwner::PlayerProtected;
        using FvoAdoptionOwner::OtherProtected;
        using FvoAdoptionOwner::FvoDisposable;
        using trafficadoption::MayClaim;
        Check(MayClaim(Unclaimed,false,true,false) && !MayClaim(Unclaimed,false,false,false),
              "covered adoption takes a free unclaimed chassis only under cover");
        Check(MayClaim(PlayerSavedAuto,true,true,false) && !MayClaim(PlayerSavedAuto,false,true,false) &&
              !MayClaim(PlayerSavedAuto,true,true,true),
              "saved player autopilot admits an empty seat only on the loading screen");
        Check(!MayClaim(PlayerProtected,true,true,false) && !MayClaim(OtherProtected,true,true,false),
              "manual, unknown player state and other protected owners stay protected");
        Check(trafficadoption::MayRetireChassis(FvoDisposable) && !trafficadoption::MayRetireChassis(Unclaimed) &&
              !trafficadoption::MayRetireChassis(PlayerSavedAuto),
              "driving permission does not grant chassis retirement");
    }
    {   struct Slot { double a[3]; } slots[256], foreign{};
        bool same = true;
        for (auto& slot : slots) same &= slotcookie::Find(slots, &slot) == &slot;
        Check(same && !slotcookie::Find(slots, nullptr) && !slotcookie::Find(slots, &foreign) &&
              !slotcookie::Find(slots, slots + 256) &&
              !slotcookie::Find(slots, reinterpret_cast<const char*>(&slots[17]) + 1),
              "Same Work 1: direct cookie lookup accepts exactly the addresses accepted by the old scan");
    }
    // RR-1: the room check names its refusal, in the order it checks.
    roomrule::Facts f; f.count = 4; f.cityLimit = 20; f.ruledMax = 10;
    Check(Is(roomrule::Decide(f), "room") && roomrule::Has(roomrule::Decide(f)), "room (RR-1): an active route with space has room");
    { auto g = f; g.timeKnown = false; g.active = false; Check(Is(roomrule::Decide(g), "schedule_time_unknown"), "room (RR-1): time unknown is named first"); }
    { auto g = f; g.active = false; Check(Is(roomrule::Decide(g), "schedule_inactive"), "room (RR-1): schedule inactive"); }
    { auto g = f; g.cityFlies = false; g.count = 0; Check(Is(roomrule::Decide(g), "city_not_flying"), "room (RR-1): city not flying before count zero"); }
    { auto g = f; g.count = 0; Check(Is(roomrule::Decide(g), "count_zero"), "room (RR-1): count zero"); }
    { auto g = f; g.revisionReady = false; Check(Is(roomrule::Decide(g), "revision_not_ready"), "room (RR-1): revision not ready"); }
    { auto g = f; g.onRoute = 4; Check(Is(roomrule::Decide(g), "route_full") && !roomrule::Has(roomrule::Decide(g)), "room (RR-1): route full at its count"); }
    { auto g = f; g.all = 20; Check(Is(roomrule::Decide(g), "city_limit"), "room (RR-1): city limit"); }
    { auto g = f; g.ruled = 10; Check(Is(roomrule::Decide(g), "ruled_max"), "room (RR-1): ruled max (not pool, not an editor route)"); }
    { auto g = f; g.ruled = 10; g.pool = true; Check(Is(roomrule::Decide(g), "room"), "room (RR-1): the pool is not held by ruled max"); }
    { auto g = f; g.ruled = 10; g.editorRoute = true; Check(Is(roomrule::Decide(g), "room"), "room (RR-1): an editor route is not held by ruled max"); }

    // SW-1: the Census record round-trips and clears.
    std::vector<retiringrecord::Entry> list{ {0xFF100075, 0xFE004001, 0xFF100079, 0x001591EA}, {0xFF1000DC, 0xFE004001, 0, 0} };
    const auto text = retiringrecord::Format(list);
    const auto back = retiringrecord::Parse(text.c_str());
    Check(back.size() == 2 && back[0].vehicle == 0xFF100075 && back[0].dbase == 0x001591EA && back[1].vehicle == 0xFF1000DC && back[1].driver == 0, "record (SW-1): format and parse round-trip");
    Check(retiringrecord::Parse(L"").empty() && retiringrecord::Parse(nullptr).empty(), "record (SW-1): an older file without the key reads as nothing");
    auto c = back;
    Check(retiringrecord::Clear(c, 0xFF100079) && c.size() == 2 && c[0].driver == 0 && c[0].vehicle == 0xFF100075, "record (SW-1): clearing the driver keeps the vehicle");
    Check(retiringrecord::Clear(c, 0xFF100075) && c.size() == 1 && c[0].vehicle == 0xFF1000DC, "record (SW-1): an entry with nothing left is dropped");
    Check(!retiringrecord::Clear(c, 0x12345678) && !retiringrecord::Clear(c, 0), "record (SW-1): an unknown or zero id changes nothing");
    Check(retiringrecord::Format({ {0, 1, 0, 2} }).empty(), "record (SW-1): an empty entry is not written");
    const auto legacyPair=retiringrecord::ParsePair(L"FF100075 FF100079 FE004001 1");
    Check(legacyPair.vehicle==0xFF100075&&legacyPair.driver==0xFF100079&&legacyPair.base==0xFE004001&&legacyPair.staged&&!legacyPair.surplus,
          "record (CS-1): four-field pair rows remain active");
    const auto earlyPair=retiringrecord::ParsePair(L"FF100075 FF100079");
    Check(earlyPair.vehicle==0xFF100075&&earlyPair.driver==0xFF100079&&!earlyPair.base&&!earlyPair.staged&&!earlyPair.surplus,
          "record (CS-1): old two-field rows remain readable");
    const auto marked=retiringrecord::FormatPair({legacyPair.vehicle,legacyPair.driver,legacyPair.base,true,true});
    const auto surplus=retiringrecord::ParsePair(marked.c_str());
    Check(surplus.surplus&&surplus.staged&&surplus.vehicle==legacyPair.vehicle&&surplus.driver==legacyPair.driver&&surplus.base==legacyPair.base,
          "record (CS-1): surplus and staged status round-trip independently");
    unsigned vehicle=0,driver=0,base=0,staged=0;
    Check(std::swscanf(marked.c_str(),L"%x %x %x %x",&vehicle,&driver,&base,&staged)==4&&
          vehicle==surplus.vehicle&&driver==surplus.driver&&base==surplus.base&&staged==1,
          "record (CS-1): a previous build reads the original four fields");

    // SW-2: ids are reused across saves; only an id that still names what was saved is touched.
    Check(retiringrecord::VehicleStillOurs(true, false, 0xFE004001, 0xFE004001), "ownership (SW-2): same vehicle base -> ours");
    Check(!retiringrecord::VehicleStillOurs(true, false, 0xFE005005, 0xFE004001), "ownership (SW-2): another base -> not touched");
    Check(!retiringrecord::VehicleStillOurs(true, true, 0xFE004001, 0xFE004001), "ownership (SW-2): an actor under a vehicle id -> not touched");
    Check(!retiringrecord::VehicleStillOurs(false, false, 0, 0xFE004001) && !retiringrecord::VehicleStillOurs(true, false, 0xFE004001, 0), "ownership (SW-2): gone, or no stored base -> not touched");
    Check(retiringrecord::DriverStillOurs(true, true, 0x001591EA, 0x001591EA) && retiringrecord::DriverStillOurs(true, true, 0x00ABCDEF, 0), "ownership (SW-2): a driver: an actor with the stored base (none stored: any actor)");
    Check(!retiringrecord::DriverStillOurs(true, false, 0x001591EA, 0x001591EA) && !retiringrecord::DriverStillOurs(true, true, 0x00ABCDEF, 0x001591EA), "ownership (SW-2): not an actor, or another base -> not touched");

    std::printf("traffic_rules_test: %d checks, %d failed\n%s\n", checks, failed, failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
