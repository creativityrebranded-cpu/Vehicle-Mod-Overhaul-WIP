#pragma once
#include "bindingdefs.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

namespace hudprompts {
inline constexpr std::size_t Actions=std::size_t(bindings::Action::Count), Scanner=Actions, Fire=Actions+1, Aim=Actions+2, Look=Actions+3, Count=Actions+4;
inline constexpr int SelectionBase=500, Group=700;
struct Placement {double x=0,y=0,scale=1;bool placed=false;};
inline void Move(Placement& placement,const Placement& last,double dx,double dy){
    if(!dx&&!dy)return;
    if(!placement.placed){placement.x=last.x;placement.y=last.y;placement.placed=true;}
    placement.x+=dx;placement.y+=dy;
}
using Layout=std::array<std::array<Placement,Count>,2>;
inline bool Included(std::size_t index){return (index>=Scanner&&index<Count)||(index<Actions&&bindings::kDefs[index].layer!=bindings::Layer::Editor);}
inline bool IsSelection(int selection){return selection>=SelectionBase&&std::size_t(selection-SelectionBase)<Count&&Included(std::size_t(selection-SelectionBase));}
inline std::string Key(std::size_t index){
    if(index==Scanner)return "scanner_orders";
    if(index==Fire)return "native_fire";
    if(index==Aim)return "native_aim";
    if(index==Look)return "native_look";
    if(index>=Actions)return "unknown";
    std::string result;for(const wchar_t* p=bindings::kDefs[index].key;*p;++p)result+=char(*p);return result;
}
inline std::string Name(std::size_t index){
    if(index==Scanner)return "Scanner / vehicle orders";
    if(index==Fire)return "Fire weapon";
    if(index==Aim)return "Aim weapon";
    if(index==Look)return "Camera / look";
    if(index>=Actions)return "HUD command";
    using A=bindings::Action;
    switch(A(index)){
    case A::Scanner:return "Destination / queue stop";
    case A::HoverToggle:return "Hover / land";
    case A::RecallEscorts:return "Recall escorts";
    case A::CrewDrive:return "Follower drives";
    case A::OwnProfile:return "Vehicle profile";
    case A::NpcProfile:return "Escort profiles";
    case A::AutoToggle:return "Autopilot";
    case A::ClearRoute:return "Clear route";
    case A::GuardPoint:return "Guard location";
    case A::FocusTarget:return "Focus fire";
    case A::ClearFocus:return "Clear focus";
    case A::DriverExit:return "Exit driver seat";
    case A::BoardPassenger:return "Passenger seat";
    case A::PullDriver:return "Pull out driver";
    case A::RideAlong:return "Ride along";
    case A::HudEdit:return "Edit HUD";
    case A::ShipPreview:return "Hull preview";
    case A::SavePlacement:return "Save parking spot";
    case A::DeletePlacement:return "Delete parking spot";
    case A::SetSummonPoint:return "Set summon point";
    default:return bindings::kDefs[index].what;
    }
}
inline std::string Gesture(bindlogic::Gesture gesture){
    switch(gesture){case bindlogic::Gesture::Tap:return " [tap]";case bindlogic::Gesture::Hold:return " [hold]";case bindlogic::Gesture::Double:return " [double tap]";default:return "";}
}
inline std::string Input(const bindlogic::Binding& binding,int defaultKey,std::uint32_t defaultPad,bool pad,bindlogic::PadStyle style,std::uint8_t context){
    std::string result;
    for(const auto& alt:binding){
        if(!bindlogic::InCtx(alt,context))continue;
        const int vk=alt.useDefault?(alt.defaultKeyboard?defaultKey:0):alt.vk;
        const auto bit=alt.useDefault?(alt.defaultController?defaultPad:0):alt.pad;
        std::string text;
        if(!pad&&vk)text=bindlogic::KeyLabel(vk);
        if(pad&&bit){
            switch(bit){
            case bindlogic::kLUp:text="Left stick up";break;case bindlogic::kLDown:text="Left stick down";break;
            case bindlogic::kLLeft:text="Left stick left";break;case bindlogic::kLRight:text="Left stick right";break;
            case bindlogic::kRUp:text="Right stick up";break;case bindlogic::kRDown:text="Right stick down";break;
            case bindlogic::kRLeft:text="Right stick left";break;case bindlogic::kRRight:text="Right stick right";break;
            default:text=bindlogic::PadLabel(bit,style);break;
            }
            if(alt.mod)text=bindlogic::PadLabel(alt.mod,style)+"+"+text;
        }
        if(text.empty())continue;
        text+=Gesture(alt.gesture);
        if(!result.empty())result+=" / ";result+=text;
    }
    return result;
}
inline std::string Line(std::size_t index,const std::string& input,bool pad){
    return Name(index)+"  |  "+(input.empty()?(pad?"UNBOUND (controller)":"UNBOUND (keyboard)"):input);
}
inline std::uint8_t Contexts(const bindlogic::Binding& binding){
    std::uint8_t result=0;for(const auto& alt:binding)result|=alt.ctx?alt.ctx:bindlogic::kCtxAll;return result;
}
inline std::string NativeName(const std::string& raw,std::size_t index){
    if(raw.find("GetUp")!=std::string::npos||raw.find("Exit")!=std::string::npos)return "Exit vehicle (native)";
    if(raw.find("Boost")!=std::string::npos)return "Boost display (native "+std::to_string(index+1)+")";
    if(raw.find("Health")!=std::string::npos)return "Vehicle health (native "+std::to_string(index+1)+")";
    if(raw.find("Speed")!=std::string::npos)return "Speed display (native "+std::to_string(index+1)+")";
    if(raw.empty()||raw.rfind("instance",0)==0||raw.rfind("child",0)==0)return "Vehicle widget element "+std::to_string(index+1);
    auto text=raw;for(const char* suffix:{"_mc","_tf"}){const auto n=std::string(suffix).size();if(text.size()>n&&text.compare(text.size()-n,n,suffix)==0)text.resize(text.size()-n);}
    std::string name;for(char c:text){if(c=='_')c=' ';if(c>='A'&&c<='Z'&&!name.empty()&&name.back()!=' ')name+=' ';name+=c;}
    return name.empty()?"Vehicle widget element "+std::to_string(index+1):"Native: "+name;
}
inline std::string Save(const Layout& layout){
    std::ostringstream out;
    for(std::size_t mode=0;mode<2;++mode)for(std::size_t i=0;i<Count;++i)if(Included(i)){
        const auto& p=layout[mode][i];out<<"prompt_"<<(mode?"pad_":"kb_")<<Key(i)<<" = "<<p.x<<" "<<p.y<<" "<<p.scale<<" "<<(p.placed?1:0)<<"\n";
    }
    return out.str();
}
inline bool Load(const std::string& line,Layout& layout){
    for(std::size_t mode=0;mode<2;++mode)for(std::size_t i=0;i<Count;++i)if(Included(i)){
        const auto prefix=std::string("prompt_")+(mode?"pad_":"kb_")+Key(i)+" = ";
        if(line.rfind(prefix,0)!=0)continue;
        Placement p;int placed=1;const auto fields=std::sscanf(line.c_str()+prefix.size(),"%lf %lf %lf %d",&p.x,&p.y,&p.scale,&placed);
        if(fields<3||(placed!=0&&placed!=1)||!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.scale))return false;
        p.placed=placed!=0;
        p.x=std::clamp(p.x,-8000.0,8000.0);p.y=std::clamp(p.y,-8000.0,8000.0);p.scale=std::clamp(p.scale,.5,2.5);layout[mode][i]=p;return true;
    }
    return false;
}
}
