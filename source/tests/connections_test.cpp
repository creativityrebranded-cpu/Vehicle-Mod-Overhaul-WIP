#include "runtimeforms.h"
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
    check(pilotlayout::kSlots==fvolimits::kVehicles&&pilotlayout::kBoardPackagesRest.At(0).local==0xB00&&pilotlayout::kBoardPackagesRest.At(std::size_t(pilotlayout::kSlots-65)).local==std::uint32_t(0xB00+pilotlayout::kSlots-65)&&!pilotlayout::kBoardPackagesRest.At(std::size_t(pilotlayout::kSlots-64))&&pilotlayout::kBoardPackages.At(5).local==0x806&&pilotlayout::kBoardPackages.At(11).local==0x80C&&!pilotlayout::kBoardPackages.At(12)&&pilotlayout::kBoardPackagesMore.At(0).local==0x900&&pilotlayout::kBoardPackagesMore.At(51).local==0x933&&!pilotlayout::kBoardPackagesMore.At(52),"native quest capacity is explicit without guessing load-order bits (the shared vehicle limit, 2026-10-29)");
    vehiclelinks::Services services{Scan,Owned,Driver,Used};
    check(bool(services)&&!vehiclelinks::Services{}&&!vehiclelinks::Services{Scan,Owned,Driver},"a complete connection is required before snapshot publication");
    framework::ScanCursor cursor;std::uintptr_t observed=0;
    check(services.scan(Note,&observed,cursor)&&observed==42&&services.owned()==7&&services.driver(42)==8&&services.inUse(42),"the same consumer contract accepts independent discovery, flight and seat providers");
    std::printf("connections: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
