#pragma once
#include "hudprompts.h"
#include <map>
#include <iterator>

// Presentation only: native game controls never enter this model.
namespace inlinehints {
using A=bindings::Action;
enum class Element { Summon, Follow, Hover, Gear, Auto, Target, Route, Seat, Count };
inline constexpr std::size_t Count=std::size_t(Element::Count), Actions=std::size_t(A::Count);
struct Input { std::string key; bindlogic::Gesture gesture=bindlogic::Gesture::Raw; };
using Inputs=std::array<std::vector<Input>,Actions>;
inline std::vector<Input> Resolve(const bindlogic::Binding& binding,int vk,std::uint32_t pad,bool controller,bindlogic::PadStyle style,std::uint8_t context){
    std::vector<Input> result;
    for(auto alt:binding){
        if(!bindlogic::InCtx(alt,context))continue;
        const auto gesture=alt.gesture;alt.gesture=bindlogic::Gesture::Raw;
        auto key=hudprompts::Input({alt},vk,pad,controller,style,context);
        if(!key.empty())result.push_back({std::move(key),gesture});
    }
    return result;
}
struct Context {
    bool allowed=true,hints=false,scanner=false,driving=false,flying=false,placement=false;
    bool own=false,occupied=false,escorts=false,route=false,retry=false;
    bool location=false,actor=false,hostile=false,exact=false,savedSpot=false;
};
struct Meaning {A action;bindlogic::Gesture gesture;std::string text;};
struct Group {std::string key;std::vector<Meaning> meanings;};
struct View {
    std::array<std::vector<Group>,Count> elements;
    const std::vector<Group>& operator[](Element e)const{return elements[std::size_t(e)];}
};
inline std::string Verb(bindlogic::Gesture g){
    switch(g){case bindlogic::Gesture::Tap:return "Tap";case bindlogic::Gesture::Hold:return "Hold";case bindlogic::Gesture::Double:return "Double tap";default:return "Press";}
}
inline View Build(const Inputs& inputs,const Context& c){
    View view;if(!c.allowed)return view;
    std::map<std::string,Element> owners;
    const auto add=[&](A action,Element preferred,const char* text){
        for(const auto& input:inputs[std::size_t(action)]){
            if(input.key.empty())continue;
            const auto [it,inserted]=owners.emplace(input.key,preferred);(void)inserted;
            auto& groups=view.elements[std::size_t(it->second)];
            auto g=std::find_if(groups.begin(),groups.end(),[&](const Group& group){return group.key==input.key;});
            if(g==groups.end()){groups.push_back({input.key,{}});g=std::prev(groups.end());}
            if(std::none_of(g->meanings.begin(),g->meanings.end(),[&](const Meaning& m){return m.action==action&&m.gesture==input.gesture;}))
                g->meanings.push_back({action,input.gesture,text});
        }
    };
    // Original prompt owners first. Rebound gestures join the owner of the actual input.
    if(!c.driving&&c.own){add(A::Summon,Element::Summon,"Summon");add(A::Cover,Element::Follow,"Follow / cover");}
    if(c.driving&&!c.scanner){add(A::HoverToggle,Element::Hover,c.flying?"Land":"Hover");add(A::Gear,Element::Gear,"Next gear");add(A::AutoToggle,Element::Auto,"Auto");}
    if(c.occupied&&!c.scanner){add(A::BoardPassenger,Element::Seat,"Passenger seat");if(c.hints)add(A::PullDriver,Element::Seat,"Pull out driver");}
    if(!c.hints)return view;
    if(c.scanner){
        if(c.placement){
            add(A::SavePlacement,Element::Target,"Record spot");
            if(c.location&&c.savedSpot)add(A::DeletePlacement,Element::Target,"Delete spot");
            if(c.location&&c.exact)add(A::SetSummonPoint,Element::Target,"Set summon point");
        }else{
            if(c.location&&!c.actor){add(A::Scanner,Element::Target,"Destination");if(c.own||c.driving)add(A::GuardPoint,Element::Target,"Guard here");}
            if(c.route)add(A::ClearRoute,Element::Target,"Clear route");
        }
    }
    if(c.own&&!c.driving){add(A::RecallEscorts,Element::Summon,"Recall escorts");if(c.escorts)add(A::CrewDrive,Element::Follow,"Follower drives");}
    if(c.scanner&&!c.placement){
        if(c.actor&&c.hostile&&(c.own||c.driving))add(A::GuardPoint,Element::Target,"Focus fire");
        if(c.hostile&&(c.own||c.driving||c.escorts))add(A::FocusTarget,Element::Target,"Focus fire");
    }
    if(c.driving&&!c.scanner){if(c.flying)add(A::LiftDown,Element::Gear,"Descend");add(A::OwnProfile,Element::Auto,"Vehicle profile");}
    if(c.route){if(c.retry)add(A::RetryTrip,Element::Route,"Retry trip");add(A::SkipStop,Element::Route,"Skip stop");}
    return view;
}
inline std::string Keys(const std::vector<Group>& groups){std::string result;for(const auto& group:groups){if(!result.empty())result+=" / ";result+=group.key;}return result;}
inline std::string Label(const std::vector<Group>& groups,bool hints,const std::string& fallback={}){
    if(groups.empty())return fallback;
    if(!hints){const auto& m=groups.front().meanings.front();return m.text+(m.gesture==bindlogic::Gesture::Raw?"":" ("+Verb(m.gesture)+")");}
    std::string result;
    for(const auto& group:groups)for(const auto& m:group.meanings){
        if(!result.empty())result+='\n';
        if(groups.size()>1)result+=group.key+": ";
        result+=Verb(m.gesture)+": "+m.text;
    }
    return result;
}
inline std::string Annotations(const std::vector<Group>& groups){
    std::string result;
    for(const auto& group:groups){
        if(!result.empty())result+='\n';result+=group.key+": ";
        bool first=true;for(const auto& m:group.meanings){if(!first)result+=" | ";first=false;result+=Verb(m.gesture)+" "+m.text;}
    }
    return result;
}
inline std::string TargetText(const std::string& value){
    if(value.empty())return {};
    if(value.rfind("TARGET",0)==0||value.rfind("Target",0)==0)return value;
    return "Target | "+value;
}
}
