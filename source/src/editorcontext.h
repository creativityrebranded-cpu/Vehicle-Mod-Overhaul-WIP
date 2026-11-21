#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace editorcontext {
enum class Surface { Browse, Edit, Advanced, Popup };
enum class Popup { None, Dimensions, Movement, Vehicles, Transform, Scope };
struct State {
    Surface surface=Surface::Browse;
    Popup popup=Popup::None;
    bool pinned=false,sections=false;
    std::string selection;
    void Select(const std::string& id){selection=id;surface=Surface::Edit;popup=Popup::None;}
    void Browse(){surface=Surface::Browse;popup=Popup::None;}
    void Advanced(){surface=Surface::Advanced;popup=Popup::None;}
    void Open(Popup next){popup=popup==next?Popup::None:next;surface=popup==Popup::None?Surface::Edit:Surface::Popup;}
    void Back(){if(surface==Surface::Browse){surface=Surface::Edit;}else if(surface==Surface::Edit){Browse();}else {surface=Surface::Edit;popup=Popup::None;}}
    bool Panel()const{return surface==Surface::Browse||surface==Surface::Advanced||pinned;}
};
struct Rect {double x=0,y=0,w=0,h=0;bool Contains(double px,double py)const{return w>0&&h>0&&px>=x&&px<=x+w&&py>=y&&py<=y+h;}};
inline Rect Clamp(Rect r,double width,double height,double margin=12){r.w=std::min(r.w,std::max(1.0,width-2*margin));r.h=std::min(r.h,std::max(1.0,height-2*margin));r.x=std::clamp(r.x,margin,std::max(margin,width-margin-r.w));r.y=std::clamp(r.y,margin,std::max(margin,height-margin-r.h));return r;}
inline bool Intersects(Rect a,Rect b){return a.x<b.x+b.w&&b.x<a.x+a.w&&a.y<b.y+b.h&&b.y<a.y+a.h;}
inline bool Number(const std::string& text,double& value){try{std::size_t used=0;value=std::stod(text,&used);return used==text.size()&&std::isfinite(value);}catch(...){return false;}}
}
