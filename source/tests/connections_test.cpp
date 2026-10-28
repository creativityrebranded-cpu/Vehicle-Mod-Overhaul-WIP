#include "runtimeforms.h"
#include "cityprofile.h"
#include "pilotlayout.h"
#include "vehiclelinks.h"
#include <cstdio>
#include <string_view>

namespace {
bool Scan(vehiclelinks::Visitor visit,void* context,framework::ScanCursor& cursor){if(visit)visit(42,context);cursor={};return true;}
std::uint32_t Owned(){return 7;}
std::uint32_t Driver(std::uint32_t){return 8;}
bool Used(std::uint32_t){return true;}
void Note(std::uintptr_t id,void* context){*static_cast<std::uintptr_t*>(context)=id;}
}
int main(){
    int checks=0,failed=0;auto check=[&](bool ok,const char* name){++checks;if(!ok){++failed;std::printf("FAIL: %s\n",name);}};
    runtimeforms::Range range{"OtherPlugin.esm",0x900,3};
    check(bool(range.At(0))&&range.At(0).local==0x900&&range.At(2).local==0x902,"resource ranges work independently of the shipped plugin name");
    check(!range.At(3)&&!runtimeforms::Range{"Other.esm",0xFFFFFF,2}.At(0),"range boundaries and overflow cannot create unrelated identities");
    check(!runtimeforms::Key{nullptr,1}&&!runtimeforms::Key{"",1}&&!runtimeforms::Key{"Other.esm",0}&&!runtimeforms::Key{"Other.esm",0x1000000},"malformed stable keys fail empty");
    check(pilotlayout::kSlots==6&&pilotlayout::kBoardPackages.At(5).local==0x806&&!pilotlayout::kBoardPackages.At(6),"native quest capacity is explicit without guessing load-order bits");
    auto other=cityprofile::kNewAtlantis;other.name="Other City";other.plugin="OtherTraffic.esm";other.placementWorldPrefix="OtherPlanet";
    other.ring={"OtherWorld",200,300,50,100};other.frame={10,20,30};
    const std::array profiles{cityprofile::kNewAtlantis,other};
    check(cityprofile::ForPlacement("OtherPlanet42",profiles)==&profiles[1],"placement selection accepts another content definition without a city-specific branch");
    check(!cityprofile::ForPlacement("OtherPlanet42"),"unregistered city content is not silently enabled");
    check(!cityprofile::ForPlacement("OtherPlanet42wrong",profiles)&&!cityprofile::ForPlacement("Unknown42",profiles),"world matching rejects unrelated prefixes and invalid suffixes");
    check(cityprofile::ForRing("OtherWorldSurface",201,302,profiles)==&profiles[1],"ring connection uses registered world and anchor data");
    check(!cityprofile::ForRing("OtherWorldSurface",301,300,profiles),"ring anchor reach remains part of the content contract");
    check(cityprofile::ForRing("JemisonPlanetWorld123",-756.2,1841.6)==&cityprofile::kCities[0],"existing New Atlantis ring behavior is retained");
    check(other.frame.ToLocal(11,24,37)==std::array<double,3>{1,4,7},"coordinate conversion belongs to content data rather than a city controller");
    check(cityprofile::kNewAtlantis.vehicles.At(31).local==0x81F&&cityprofile::kNewAtlantis.pool.At(63).local==0xF3F,"existing traffic and pool identities are unchanged");
    vehiclelinks::Services services{Scan,Owned,Driver,Used};
    check(bool(services)&&!vehiclelinks::Services{}&&!vehiclelinks::Services{Scan,Owned,Driver},"a complete connection is required before snapshot publication");
    framework::ScanCursor cursor;std::uintptr_t observed=0;
    check(services.scan(Note,&observed,cursor)&&observed==42&&services.owned()==7&&services.driver(42)==8&&services.inUse(42),"the same consumer contract accepts independent discovery, flight and seat providers");
    std::printf("connections: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
