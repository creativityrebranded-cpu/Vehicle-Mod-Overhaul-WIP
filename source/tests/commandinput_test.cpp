#include "commandinput.h"
#include "inputownership.h"
#include <cstdio>
#include <string>

int main(){
    using namespace bindings;using namespace bindlogic;
    int failures=0,checks=0;
    auto check=[&](bool good,const std::string& text){++checks;if(!good){++failures;std::printf("FAIL %s\n",text.c_str());}};
    commandinput::Bindings map;commandinput::Keys keys;commandinput::Pads pads{};
    for(std::size_t i=0;i<commandinput::Count;++i){
        std::string bad;map[i]=Parse(kDefs[i].spec,&bad);keys[i]=kDefs[i].defaultVk;
        check(bad.empty()&&!map[i].empty(),std::string("valid defaults: ")+kDefs[i].what);
    }
    pads[std::size_t(Action::Boost)]=kLS;pads[std::size_t(Action::VerticalBoost)]=kY;
    pads[std::size_t(Action::DriverExit)]=kB;pads[std::size_t(Action::BoardPassenger)]=kA;pads[std::size_t(Action::PullDriver)]=kA;
    bool clean=true;
    for(std::size_t i=0;i<commandinput::Count;++i)for(std::size_t j=i+1;j<commandinput::Count;++j){
        if(!LayersOverlap(kDefs[i].layer,kDefs[j].layer))continue;
        if((Action(i)==Action::Boost&&Action(j)==Action::ThrottleLock)||(Action(j)==Action::Boost&&Action(i)==Action::ThrottleLock))continue;
        for(bool pad:{false,true})for(const auto& a:map[i])for(const auto& b:map[j])
            if(commandinput::Conflict(a,keys[i],pads[i],b,keys[j],pads[j],pad)){
                clean=false;std::printf("CLASH %s / %s (%s)\n",kDefs[i].what,kDefs[j].what,pad?"pad":"keyboard");
            }
    }
    check(clean,"complete default map has no conflicting commands");
    for(std::size_t i=0;i<commandinput::Count;++i)for(bool controller:{false,true}){
        const auto slots=DeviceSlots(map[i],controller,keys[i],pads[i]);
        check(!slots.empty(),std::string("complete device: ")+kDefs[i].what+(controller?" pad":" keyboard"));
        for(const auto& slot:slots){
            const auto& alt=map[i][slot.index];
            for(unsigned c=1;c<=kCtxEditor;c<<=1){
                if(!InCtx(alt,std::uint8_t(c)))continue;
                // Unscoped HUD toggle is tested in real HUD contexts, not every menu.
                if(!alt.ctx&&c!=kCtxFoot&&c!=kCtxEditor)continue;
                commandinput::Sample input;input.context=std::uint8_t(c);input.driving=(c&kCtxVehicle)!=0;
                input.placement=kDefs[i].layer==Layer::Placement;
                if(!input.Available(i))continue;
                commandinput::Frame frame;frame.Update(map,keys,pads,input,0);
                std::array<bool,commandinput::Count> fired{};
                auto step=[&](bool down,std::uint64_t now){
                    input.keys.fill(false);input.pad=0;
                    if(controller){if(down)input.pad=slot.in.pad|slot.in.mod;}
                    else if(down)input.keys[std::size_t(slot.in.vk)]=true;
                    frame.Update(map,keys,pads,input,now);
                    for(std::size_t n=0;n<fired.size();++n)fired[n]=fired[n]||frame.triggered[n];
                };
                step(true,100);
                if(alt.gesture==Gesture::Hold){step(true,700);step(true,800);step(false,900);}
                else if(alt.gesture==Gesture::Double){step(false,160);step(true,220);step(false,270);step(false,650);}
                else {step(false,200);step(false,550);}
                check(fired[i],std::string("reachable ")+kDefs[i].what+(controller?" pad ":" key ")+std::to_string(c));
                for(std::size_t n=0;n<fired.size();++n){
                    if(n==i||!fired[n])continue;
                    const bool boostPair=Action(i)==Action::ThrottleLock&&Action(n)==Action::Boost;
                    check(boostPair,std::string("independent ")+kDefs[i].what+" unexpectedly fired "+kDefs[n].what);
                }
            }
        }
    }
    // Changing a slot preserves its gesture and context and leaves other device slots.
    const auto original=map[std::size_t(Action::Scanner)];
    const auto changed=commandinput::ScopedSlot(original,original,true,0,{0,kX,0},'T',0);
    check(changed[1].ctx==kCtxScanner&&changed[1].gesture==Gesture::Tap,"rebind retains scanner tap semantics");
    check(Spec(DeviceSlots(changed,false,'T',0).empty()?Binding{}:Binding{changed[0]})=="default-key:tap@scanner","controller rebind retains keyboard slot");
    check(!commandinput::Conflict(Parse("Pad:Y:tap@scanner")[0],0,0,Parse("Pad:Y@flying")[0],0,0,true),"scanner Triangle and climb Triangle do not conflict");
    check(!commandinput::Conflict(Parse("Pad:DLeft:tap@scanner")[0],0,0,Parse("Pad:DLeft:hold@scanner")[0],0,0,true),"retry tap and profile hold do not conflict");
    check(commandinput::Conflict(Parse("Pad:Y:tap@scanner")[0],0,0,Parse("Pad:Y:tap@scanner")[0],0,0,true),"real scanner tap collision is detected");
    const auto restored=commandinput::RestoreDevice(Parse("F6:tap@scanner, Pad:X:tap@scanner"),original,true);
    check(Spec(restored)=="F6:tap@scanner, Pad:Y:tap@scanner","controller reset preserves custom keyboard command");
    const auto cleared=SetSlot(original,true,0,{},'T',0);
    check(Spec(cleared)=="default-key:tap@scanner","controller clear preserves keyboard command");
    check(Modifiers({Parse("Pad:LB+B@scanner")},kCtxGround)==0,"inactive chord cannot suppress driving input");
    // A held button cannot become a different command when a scanner, editor or menu closes.
    commandinput::Frame frame;commandinput::Sample sample;sample.context=kCtxScanner;
    frame.Update(map,keys,pads,sample,0);sample.pad=kLS;frame.Update(map,keys,pads,sample,100);
    sample.context=kCtxGround;sample.driving=true;frame.Update(map,keys,pads,sample,200);
    check(!frame.triggered[std::size_t(Action::HoverToggle)],"closing scanner with L3 down cannot toggle hover");
    sample.allowed=false;frame.Update(map,keys,pads,sample,250);sample.allowed=true;frame.Update(map,keys,pads,sample,300);
    check(!frame.triggered[std::size_t(Action::HoverToggle)],"returning from menu with L3 held needs release");
    sample.pad=0;frame.Update(map,keys,pads,sample,350);sample.pad=kLS;frame.Update(map,keys,pads,sample,400);
    check(frame.triggered[std::size_t(Action::HoverToggle)],"fresh L3 after menu works");
    std::uint8_t owner=0;
    check(!inputownership::Own(owner,true,true,false),"native owns original press");
    check(!inputownership::Own(owner,true,false,true),"context cannot steal native held press");
    check(!inputownership::Own(owner,false,false,true),"native receives release despite new mod context");
    check(inputownership::Own(owner,true,true,true),"mod owns new press");
    check(inputownership::Own(owner,false,false,false),"mod-owned release does not leak into native context");
    const std::uint8_t gameplay[]={0,0x49,0x38},menu[]={0,0x49,0x4E};
    check(inputownership::Gameplay(gameplay,3)&&!inputownership::Gameplay(menu,3),"native UI blocker suppresses mod gameplay");
    commandinput::Frame analog;commandinput::Sample axis;axis.context=kCtxGround;axis.driving=true;
    analog.Update(map,keys,pads,axis,0);axis.axes.ly=.62;axis.pad=kLUp;analog.Update(map,keys,pads,axis,10);
    check(std::fabs(analog.Amount(std::size_t(Action::Accelerate),map,keys,pads,axis)-.5)<1e-9,"analog throttle stays analog in native ground bridge");
    std::printf("Command input: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
