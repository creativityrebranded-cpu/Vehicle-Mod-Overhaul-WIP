#include "triplogic.h"
#include <cstdio>
#include <limits>
#include <string>
#include <vector>
int main() {
 int failures=0;
 auto check=[&](bool ok,const char* text){std::printf("%s %s\n",ok?"PASS":"FAIL",text); if(!ok) ++failures;};
 using namespace triplogic;
 check(Reached(7,6,1,1,true),"settled at destination reaches arrival");
 check(!Reached(7,6,-20,0,true),"below rooftop cannot arrive");
 check(!Reached(7,6,20,0,true),"above destination cannot arrive");
 check(!Reached(7,6,0,12,true),"fast flyby cannot arrive");
 check(!Reached(7,6,0,5,true),"vertical motion cannot arrive");
 check(!Reached(7,6,0,0,false),"missing terrain or physics cannot arrive");
 check(!Reached(20,6,0,0,true),"outside stop radius cannot arrive");
 check(!Reached(0,6,std::numeric_limits<double>::quiet_NaN(),0,true),"invalid height cannot arrive");
 check(Status(false,false,false,0,false,false)==Phase::WaitingTerrain,"missing map data explains waiting");
 check(Status(true,false,true,-20,false,false)==Phase::AdjustingHeight,"wrong rooftop height explains adjusting");
 check(Status(true,false,false,0,true,true)==Phase::FindingWay,"escape explains finding another way");
 check(Status(true,false,false,0,false,true)==Phase::Blocked,"stationary obstruction explains blocked");
 check(Status(true,true,true,0,false,false)==Phase::Arrived,"settled status shows arrived");
 std::vector<std::string> queue={"Roof","Outpost"}; std::string current="Home"; bool have=true;
 check(!Advance(queue,current,have,true,false,[](const auto&){return false;}) && queue.size()==2 && current=="Home",
  "failed marker placement retains named queue and current stop");
 check(Advance(queue,current,have,true,false,[](const auto&){return true;}) && current=="Roof" && queue.back()=="Home",
  "patrol advances and appends completed named stop");
 check(Advance(queue,current,have,true,true,[](const auto&){return true;}) && current=="Outpost" && queue.size()==1 && queue.front()=="Home",
  "manual skip does not requeue skipped patrol stop");
 queue.clear();
 check(!Advance(queue,current,have,false,true,[](const auto&){return true;}) && current=="Outpost","empty queue cannot erase current destination");
 return failures ? 1 : 0;
}
