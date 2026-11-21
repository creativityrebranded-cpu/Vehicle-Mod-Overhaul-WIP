#include "placeaccess.h"
#include <cstdio>
// Landing wires (spec 7b): old single fields read as one In and one Out wire; extra wires are stored apart; edits
// through the old fields win for the first wire of each socket; a trip composes the chosen wires.
using namespace placeaccess;
int main(){int checks=0,fail=0;auto check=[&](bool yes,const char* name){++checks;if(!yes){++fail;std::printf("FAIL %s\n",name);}};
    Plan old;old.entryRoute="ring";old.entrance={{1,2,3}};old.exitRoute="road";old.departure={{4,5,6},{7,8,9}};
    auto w=FromPlan(old);check(w.size()==2&&w[0].socket==0&&w[0].route=="ring"&&w[1].socket==1&&w[1].path.size()==2,"old fields read as one In and one Out wire");
    check(!Extra(w),"one wire per socket needs no extra record");
    w.push_back({1,"bypass",1,{{10,11,12}}});check(Extra(w),"a second Out wire needs the extra record");
    std::vector<Wire> back;check(ReadWires(WriteWires(w),back)&&back.size()==3&&back[2].route=="bypass"&&back[2].lane==1&&back[2].path[0].z==12,"wires round trip");
    check(!ReadWires("2 0",back)&&!ReadWires("1 1 5 \"x\" -1 0",back),"unknown versions and sockets are refused");
    Plan mirrored;Mirror(mirrored,w);check(mirrored.entryRoute=="ring"&&mirrored.exitRoute=="road"&&mirrored.departure.size()==2,"first wire of each socket mirrors into the old fields");
    auto edited=old;edited.exitRoute="moved";auto merged=Merge(edited,w);check(merged.size()==3&&merged[1].route=="moved"&&merged[2].route=="bypass","old-field edits win for the first wire, extra wires kept");
    Plan empty;check(Merge(empty,{}).empty()&&FromPlan(empty).empty(),"an unwired landing has no wires");
    const auto trip=Compose(old,&w[0],&w[2]);check(trip.entryRoute=="ring"&&trip.exitRoute=="bypass"&&trip.departure.size()==1,"a trip composes the chosen In and Out wire");
    MoveWires(w,1,1,1);check(w[0].path[0].x==2&&w[2].path[0].z==13,"world shifts move every wire");
    std::vector<Wire> many(65);check(!ValidWires(many),"wire count is bounded");
    std::printf("placewires: %d checks, %d failures\n",checks,fail);return fail?1:0;
}
