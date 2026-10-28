#include "inlinehints.h"
#include <set>
#include <cstdio>
using namespace inlinehints;
Inputs Defaults(bool pad,std::uint8_t context,bindlogic::PadStyle style=bindlogic::PadStyle::Xbox){
    Inputs inputs;for(std::size_t i=0;i<Actions;++i)inputs[i]=Resolve(bindlogic::Parse(bindings::kDefs[i].spec),bindings::kDefs[i].defaultVk,0,pad,style,context);return inputs;
}
bool Has(const View& view,Element element,A action){for(const auto& g:view[element])for(const auto& m:g.meanings)if(m.action==action)return true;return false;}
bool Anywhere(const View& view,A action){for(std::size_t i=0;i<Count;++i)if(Has(view,Element(i),action))return true;return false;}
int main(){
    int checks=0,failures=0;const auto check=[&](bool ok,const char* message){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",message);}};
    Context c;c.hints=true;c.scanner=true;c.own=true;c.escorts=true;c.location=true;c.exact=true;c.route=true;
    const auto pad=Defaults(true,bindlogic::kCtxScanner);
    auto v=Build(pad,c);
    check(v[Element::Target].size()==1,"destination gestures share one physical input");
    check(Has(v,Element::Target,A::Scanner)&&Has(v,Element::Target,A::GuardPoint)&&Has(v,Element::Target,A::ClearRoute),"destination/guard/clear share Target");
    check(Has(v,Element::Summon,A::RecallEscorts),"recall extends original Summon");
    check(Has(v,Element::Follow,A::CrewDrive),"crew drive extends original Follow");
    c.actor=true;c.hostile=true;c.location=false;v=Build(pad,c);
    check(Has(v,Element::Follow,A::FocusTarget),"shared L3 focus belongs to Follow");
    check(!Anywhere(v,A::Scanner),"actor target excludes destination");
    check(Anywhere(v,A::GuardPoint),"hostile actor: the T hold is focus fire (rams removed 2026-10-25)");
    c.hostile=false;v=Build(pad,c);check(!Anywhere(v,A::FocusTarget)&&!Anywhere(v,A::GuardPoint),"neutral target excludes focus and the hold");
    auto kb=Defaults(false,bindlogic::kCtxScanner);c.hostile=true;v=Build(kb,c);
    check(Has(v,Element::Target,A::FocusTarget),"keyboard H focus attaches to Target");
    auto remap=pad;remap[std::size_t(A::FocusTarget)]={{"LB+X",bindlogic::Gesture::Double}};v=Build(remap,c);
    check(Has(v,Element::Target,A::FocusTarget)&&!Has(v,Element::Follow,A::FocusTarget),"remapped focus moves to its actual input owner");
    c.actor=c.hostile=false;c.placement=true;c.location=c.exact=true;c.savedSpot=false;v=Build(pad,c);
    check(!Anywhere(v,A::Scanner)&&!Anywhere(v,A::GuardPoint)&&!Anywhere(v,A::ClearRoute),"placement replaces destination gestures");
    check(Anywhere(v,A::SavePlacement)&&Anywhere(v,A::SetSummonPoint),"valid surface supports placement and summon point");
    check(!Anywhere(v,A::DeletePlacement),"no delete hint without a saved spot");
    c.savedSpot=true;v=Build(pad,c);check(Anywhere(v,A::DeletePlacement),"saved spot gains delete hint");
    c.exact=false;v=Build(pad,c);check(!Anywhere(v,A::SetSummonPoint),"estimated surface cannot set summon point");
    c.hints=false;v=Build(pad,c);check(!Anywhere(v,A::SavePlacement)&&Has(v,Element::Summon,A::Summon),"help off preserves original prompts and hides supplemental hints");
    c.allowed=false;v=Build(pad,c);check(v[Element::Summon].empty(),"modal suppression hides hints");
    c={};c.hints=true;c.driving=true;c.flying=true;v=Build(Defaults(true,bindlogic::kCtxHover),c);
    check(Has(v,Element::Gear,A::LiftDown)&&v[Element::Gear].size()==2,"flight descend gestures attach to Gear with their own inputs");
    check(Has(v,Element::Auto,A::OwnProfile),"profile shares Auto input");
    check(!Anywhere(v,A::DriverExit)&&!Anywhere(v,A::Boost)&&!Anywhere(v,A::Accelerate),"native actions never gain duplicate hints");
    c.flying=false;v=Build(Defaults(true,bindlogic::kCtxGround),c);check(!Anywhere(v,A::LiftDown),"ground has no flight descent annotation");
    check(TargetText("NO TARGET")=="Target | NO TARGET","empty target has a readable information name");
    check(TargetText("TARGET 120 m")=="TARGET 120 m","existing Target name is not duplicated");
    check(TargetText("").empty(),"absent target stays hidden");
    check(Annotations({}).empty()&&Keys({}).empty()&&Label({},true).empty(),"unbound inputs do not generate artificial information bindings");
    const auto sony=Defaults(true,bindlogic::kCtxScanner,bindlogic::PadStyle::PlayStation);
    check(sony[std::size_t(A::Summon)][0].key=="Circle","PlayStation labels resolve actual device style");
    auto chord=Resolve(bindlogic::Parse("Pad:LB+X:hold@scanner, N:tap@foot"),0,0,true,bindlogic::PadStyle::Xbox,bindlogic::kCtxScanner);
    check(chord.size()==1&&chord[0].key=="LB+X"&&chord[0].gesture==bindlogic::Gesture::Hold,"contextual chord resolves without gesture in physical identity");
    // Context/device matrix: each physical input occurs once and no native control leaks into the model.
    for(bool controller:{false,true})for(auto context:{bindlogic::kCtxFoot,bindlogic::kCtxGround,bindlogic::kCtxHover,bindlogic::kCtxScanner,bindlogic::kCtxPassenger})
    for(bool hints:{false,true})for(bool target:{false,true})for(bool placement:{false,true}){
        Context test;test.hints=hints;test.scanner=context==bindlogic::kCtxScanner;test.driving=(context&bindlogic::kCtxVehicle)!=0;test.flying=context==bindlogic::kCtxHover;
        test.placement=placement;test.own=test.escorts=test.route=test.retry=true;test.location=test.actor=test.hostile=test.exact=test.savedSpot=target;
        const auto view=Build(Defaults(controller,std::uint8_t(context)),test);std::set<std::string> keys;
        bool unique=true,native=false;
        for(const auto& groups:view.elements)for(const auto& group:groups){unique=keys.insert(group.key).second&&unique;for(const auto& m:group.meanings)native=native||m.action==A::DriverExit||m.action==A::Boost||m.action==A::Accelerate;}
        check(unique,"matrix unique physical input owner");check(!native,"matrix excludes native controls");
    }
    std::printf("Inline hints: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
