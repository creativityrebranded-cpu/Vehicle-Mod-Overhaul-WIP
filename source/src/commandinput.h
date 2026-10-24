#pragma once
#include "bindingdefs.h"
#include <array>

namespace commandinput {
inline constexpr std::size_t Count=std::size_t(bindings::Action::Count);
using Bindings=std::array<bindlogic::Binding,Count>;
using Keys=std::array<int,Count>;
using Pads=std::array<std::uint32_t,Count>;
struct Sample {
    std::array<bool,256> keys{};
    std::uint32_t pad=0;
    bindlogic::Axes axes;
    std::uint8_t context=bindlogic::kCtxFoot;
    bool allowed=true,placement=false,driving=false;
    bool Layer(bindings::Layer layer)const {
        return layer==bindings::Layer::Placement?placement:layer==bindings::Layer::Target?!placement:true;
    }
    bool Available(std::size_t index)const {
        const auto action=bindings::Action(index);
        if(driving&&(action==bindings::Action::Summon||action==bindings::Action::Cover||action==bindings::Action::CrewDrive||
            action==bindings::Action::RideAlong||bindings::kDefs[index].layer==bindings::Layer::Placement))return false;
        return Layer(bindings::kDefs[index].layer);
    }
};
struct Alternative {
    bindlogic::GestureState gesture;
    bool raw=false,on=false;
};
struct Frame {
    std::array<std::vector<Alternative>,Count> states;
    std::array<std::vector<bool>,Count> on;
    std::array<bool,Count> triggered{},started{},enabled{};
    std::uint8_t context=0;
    bool allowed=false;
    std::uint32_t modifiers=0;
    void Reset(){*this=Frame{};}
    void Update(const Bindings& bindings,const Keys& vk,const Pads& pads,const Sample& input,std::uint64_t now){
        const bool transition=context!=input.context||allowed!=input.allowed;
        context=input.context;allowed=input.allowed;modifiers=0;triggered.fill(false);started.fill(false);
        std::array<bool,256> doubleKeys{};std::uint32_t doublePads=0;
        for(std::size_t i=0;i<Count;++i)if(input.Available(i))for(const auto& alt:bindings[i])if(bindlogic::InCtx(alt,context)){
            modifiers|=alt.mod;
            if(alt.gesture!=bindlogic::Gesture::Double)continue;
            const int key=alt.useDefault?(alt.defaultKeyboard?vk[i]:0):alt.vk;
            const auto pad=alt.useDefault?(alt.defaultController?pads[i]:0):alt.pad;
            if(key>0&&key<256)doubleKeys[std::size_t(key)]=true;
            doublePads|=pad;
        }
        for(std::size_t i=0;i<Count;++i){
            const bool active=allowed&&input.Available(i);
            const bool changed=transition||active!=enabled[i];enabled[i]=active;
            states[i].resize(bindings[i].size());on[i].assign(bindings[i].size(),false);
            for(std::size_t j=0;j<bindings[i].size();++j){
                const auto& alt=bindings[i][j];auto& state=states[i][j];
                const int key=alt.useDefault?(alt.defaultKeyboard?vk[i]:0):alt.vk;
                const auto pad=alt.useDefault?(alt.defaultController?pads[i]:0):alt.pad;
                const bool keyOn=key>0&&key<256&&input.keys[std::size_t(key)];
                auto padAlt=alt;padAlt.pad=pad;
                const bool padOn=bindlogic::PadOn(padAlt,input.pad,modifiers);
                const bool raw=keyOn||padOn;
                const bool usable=active&&bindlogic::InCtx(alt,context);
                if(changed||!usable)state.gesture.Cancel(raw);
                bool value=false;
                if(usable){
                    const bool doubleBound=(key>0&&key<256&&doubleKeys[std::size_t(key)])||(pad&&(pad&doublePads));
                    started[i]=started[i]||(raw&&!state.raw&&!state.gesture.blocked);
                    if(alt.gesture==bindlogic::Gesture::Raw){value=!state.gesture.blocked&&raw;if(!raw)state.gesture.blocked=false;}
                    else value=state.gesture.Update(raw,alt.gesture,now,doubleBound);
                }
                triggered[i]=triggered[i]||(value&&!state.on);
                state.raw=raw;state.on=value;on[i][j]=value;
            }
        }
    }
    double Amount(std::size_t i,const Bindings& bindings,const Keys& vk,const Pads& pads,const Sample& input)const {
        if(!allowed||!enabled[i])return 0;
        double value=0;
        for(std::size_t j=0;j<bindings[i].size();++j){
            if(j>=states[i].size()||states[i][j].gesture.blocked)continue;
            const auto& alt=bindings[i][j];
            if(!bindlogic::InCtx(alt,input.context))continue;
            if(alt.gesture!=bindlogic::Gesture::Raw){if(on[i][j])value=1;continue;}
            const int key=alt.useDefault?(alt.defaultKeyboard?vk[i]:0):alt.vk;
            const auto pad=alt.useDefault?(alt.defaultController?pads[i]:0):alt.pad;
            if(key>0&&key<256&&input.keys[std::size_t(key)])return 1;
            auto padAlt=alt;padAlt.pad=pad;
            if(!pad||(!alt.mod&&(input.pad&modifiers&~pad)))continue;
            if(!alt.mod&&bindlogic::AnalogAmount(pad,input.axes)>0)value=std::max(value,bindlogic::AnalogAmount(pad,input.axes));
            else if(bindlogic::PadOn(padAlt,input.pad,modifiers))value=1;
        }
        return value;
    }
};
inline bool Conflict(const bindlogic::Alt& a,int ak,std::uint32_t ap,const bindlogic::Alt& b,int bk,std::uint32_t bp,bool pad){
    const auto ac=a.ctx?a.ctx:bindlogic::kCtxAll,bc=b.ctx?b.ctx:bindlogic::kCtxAll;
    if(!(ac&bc))return false;
    const int av=a.useDefault?(a.defaultKeyboard?ak:0):a.vk;
    const int bv=b.useDefault?(b.defaultKeyboard?bk:0):b.vk;
    const auto ab=a.useDefault?(a.defaultController?ap:0):a.pad;
    const auto bb=b.useDefault?(b.defaultController?bp:0):b.pad;
    const bool same=pad?(ab&&ab==bb&&a.mod==b.mod):(av&&av==bv);
    return same&&(a.gesture==b.gesture||a.gesture==bindlogic::Gesture::Raw||b.gesture==bindlogic::Gesture::Raw);
}
inline bindlogic::Binding ScopedSlot(const bindlogic::Binding& current,const bindlogic::Binding& defaults,bool pad,int slot,bindlogic::Input input,int vk,std::uint32_t pb){
    const auto slots=bindlogic::DeviceSlots(current,pad,vk,pb);
    if(slot>=0&&std::size_t(slot)<slots.size())return bindlogic::SetSlot(current,pad,slot,input,vk,pb);
    auto result=current;if(input.Empty())return result;
    bindlogic::Alt alt{input.vk,input.pad,input.mod,false};
    const auto candidates=bindlogic::DeviceSlots(defaults,pad,vk,pb);
    if(!candidates.empty()){
        const auto index=slot>=0&&std::size_t(slot)<candidates.size()?std::size_t(slot):0;
        alt.ctx=defaults[candidates[index].index].ctx;alt.gesture=defaults[candidates[index].index].gesture;
    }
    result.push_back(alt);return result;
}
inline bindlogic::Binding RestoreDevice(const bindlogic::Binding& current,const bindlogic::Binding& defaults,bool pad){
    bindlogic::Binding result;
    for(auto alt:current){if(alt.useDefault){(pad?alt.defaultController:alt.defaultKeyboard)=false;if(alt.defaultController||alt.defaultKeyboard)result.push_back(alt);}else if(pad?alt.vk!=0:alt.pad!=0)result.push_back(alt);}
    for(auto alt:defaults){if(alt.useDefault){(pad?alt.defaultKeyboard:alt.defaultController)=false;if(alt.defaultController||alt.defaultKeyboard)result.push_back(alt);}else if(pad?alt.pad!=0:alt.vk!=0)result.push_back(alt);}
    return result;
}
inline std::size_t FindConflict(std::size_t index,const bindlogic::Binding& proposed,const Bindings& current,const Keys& keys,const Pads& pads,bool pad){
    for(std::size_t other=0;other<Count;++other){
        if(other==index||!bindings::LayersOverlap(bindings::kDefs[index].layer,bindings::kDefs[other].layer))continue;
        const auto a=bindings::Action(index),b=bindings::Action(other);
        if((a==bindings::Action::Boost&&b==bindings::Action::ThrottleLock)||(b==bindings::Action::Boost&&a==bindings::Action::ThrottleLock))continue;
        for(const auto& left:proposed)for(const auto& right:current[other])
            if(Conflict(left,keys[index],pads[index],right,keys[other],pads[other],pad))return other;
    }
    return Count;
}
}
