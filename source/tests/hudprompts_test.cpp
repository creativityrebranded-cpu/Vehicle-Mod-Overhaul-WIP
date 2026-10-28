#include "hudprompts.h"
#include <set>
#include <cstdio>
int main(){
    using namespace hudprompts;using namespace bindlogic;
    int checks=0,failures=0;auto check=[&](bool valid,const char* name){++checks;if(!valid){++failures;std::printf("FAIL %s\n",name);}};
    std::set<std::string> keys;
    for(std::size_t i=0;i<Count;++i)if(Included(i)){
        check(!hudprompts::Name(i).empty()&&hudprompts::Name(i)!="?","every prompt has a display name");
        check(keys.insert(Key(i)).second,"stable layout IDs are unique");
        check(IsSelection(SelectionBase+int(i)),"every prompt is selectable in the editor");
        check(Line(i,"",true).find("UNBOUND (controller)")!=std::string::npos,"unbound controller prompt keeps its name and placeholder");
        check(Line(i,"",false).find("UNBOUND (keyboard)")!=std::string::npos,"unbound keyboard prompt keeps its name and placeholder");
    }
    check(!Included(std::size_t(bindings::Action::HudLeft)),"editor movement is help rather than a gameplay prompt");
    check(hudprompts::Input(Parse("default-key:tap@scanner, Pad:B:double@scanner"),'N',0,false,PadStyle::Xbox,kCtxScanner)=="N [tap]","keyboard default includes its gesture");
    check(hudprompts::Input(Parse("default-key:tap@scanner, Pad:B:double@scanner"),'N',0,true,PadStyle::PlayStation,kCtxScanner)=="Circle [double tap]","controller double tap has explicit PlayStation name");
    check(hudprompts::Input(Parse("Pad:A:hold@passenger"),0,0,true,PadStyle::Xbox,kCtxPassenger)=="A [hold]","Xbox hold names are explicit");
    check(hudprompts::Input(Parse("Pad:A:hold@passenger"),0,0,true,PadStyle::PlayStation,kCtxGround).empty(),"unavailable context has no misleading binding");
    check(hudprompts::Input(Parse("Pad:LStickUp@ground"),0,0,true,PadStyle::PlayStation,kCtxGround)=="Left stick up","axes get human readable names");
    Layout saved{};saved[1][1]={182,-43,1.65,true};saved[0][1]={-11,8,.75,true};
    const auto text=Save(saved);Layout restored{};std::istringstream stream(text);std::string line;
    while(std::getline(stream,line))check(Load(line,restored),"saved prompt layout line loads");
    check(restored[1][1].x==182&&restored[1][1].y==-43&&restored[1][1].scale==1.65,"controller position and size survive reload");
    check(restored[0][1].x==-11&&restored[0][1].scale==.75,"keyboard layout is independent");
    check(restored[1][1].placed&&restored[0][1].placed,"explicit placements stay fixed across context reflow");
    check(!restored[1][0].placed,"untouched prompts retain automatic placement");
    Placement moving;Move(moving,{240,180,1,false},10,-5);
    check(moving.placed&&moving.x==250&&moving.y==175,"first move anchors the displayed position");
    Move(moving,{999,999,1,false},1,2);
    check(moving.x==251&&moving.y==177,"later moves retain the explicit anchor");
    const auto before=restored[1][1];check(!Load("prompt_pad_summon = nan 0 1",restored),"non-finite layout input is rejected");
    check(restored[1][1].x==before.x,"bad layout line preserves previous layout");
    check(NativeName("",0)=="Vehicle widget element 1","unnamed native clips have a nonempty fallback");
    check(NativeName("instance729",3)=="Vehicle widget element 4","anonymous native clip identifiers become readable names");
    check(NativeName("GetUpButton_mc",2)=="Exit vehicle (native)","native exit clip has an action name");
    check(NativeName("BoostMeter_mc",0)!=NativeName("BoostBar_mc",1),"related native clips remain distinguishable in the editor");
    check(!IsSelection(-1)&&!IsSelection(999),"invalid selections cannot index the prompt array");
    check(Contexts(Parse("N:tap@foot|scanner"))==(kCtxFoot|kCtxScanner),"prompt applicability retains relevant contexts even when the device has no binding");
    check(Contexts(Parse("none"))==0,"a fully unbound command does not invent a context");
    check(hudprompts::Input(Parse(bindings::kDefs[std::size_t(bindings::Action::HudEdit)].spec),0x76,0,true,PadStyle::PlayStation,kCtxEditor)=="Circle","editor help names the exit control without entry-only bindings");
    std::printf("HUD prompts: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
