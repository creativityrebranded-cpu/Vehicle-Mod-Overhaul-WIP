#include "editorcontext.h"
#include "editoractions.h"
#include "trafficroute.h"
#include <set>
#include <cstdio>
struct Button{int id;double x,y,w,h;std::string text;bool active,enabled;std::string key;int style=0;std::string value;unsigned swatch=0;};
struct Box{double x,y,w,h;unsigned fill;double alpha;unsigned line=0;double lineAlpha=0;};
struct Text{std::string text;double x,y,w;unsigned color;double unit;};
struct UiPolygon{std::vector<std::pair<double,double>> points;unsigned color=0;double alpha=.25;};
struct {double width=1920,height=1080,panelBottom=500,menuX=600,menuY=100,menuH=0,barX=494,barY=50,barW=700,barH=44;std::vector<Button> buttons;std::vector<Box> boxes;std::vector<UiPolygon> polygons;}g_ui;
struct {int point=-1,spot=-1,zone=-1,link=-1,sheet=0,laneScope=0,laneFocus=0,menu=0;std::set<std::string> permanentFiles;}g_ed;
editorcontext::State g_context;std::string g_contextObserved,g_contextText;int g_contextNumber=-1,g_contextMenuPage=0,g_contextCategory=-1;bool g_contextPanel=true,g_contextDirectory=false;std::vector<std::pair<int,std::string>>g_contextLinks;std::vector<editorcontext::Rect>g_contextRects;std::vector<Text>g_panelText;
constexpr double kPanelW=412,kInner=384,kUnit=1.5;constexpr unsigned kText=0xffffff,kDim=0x888888,kTeal=0x00ffcc;constexpr unsigned kAmber=0xF2B04E,kClearanceRed=0xFF5555;constexpr double kLaneNoneFit=2.5,kLaneAllFit=3.5;
double PanelX(){return g_ui.width-kPanelW-12;}const char* KeyOf(int){return "";}bool laneSelected=true;
bool ContextLane(){return laneSelected;}std::string ContextKey(){return "selection";}lanetransform::Vec ContextCentre(const trafficdata::Route&){return {400,250,0};}bool ContextPlaceActive(){return g_ed.spot>=0||g_ed.zone>=0;}bool Local(const routemap::Camera&,routemap::Vec3 p,double& x,double& y){x=p.x;y=p.y;return true;}
bool g_roadPanel=false,g_roadAll=false;int g_roadDivider=0,g_roadLane=0;
int RoadLanes(const trafficdata::Route& r){return r.road.dividers.empty()?std::clamp(r.lanes,1,2):int(r.road.dividers.size())+1;}
struct F7Offset{double dx=0,dy=0;};std::array<F7Offset,4> g_f7Offset{};std::array<editorcontext::Rect,4> g_f7AreaRect{};
int g_f7Group=0,g_f7Nearest=-1,g_f7NextPrim=0,g_f7LinkPrompt=-1,g_f7Highlight=-1,g_f7FieldPage=0;bool g_f7Divider=false;double g_f7Clock=0;
int F7Point(const trafficdata::Route& r){const int n=int(r.points.size());if(g_ed.point>=0&&g_ed.point<n)return g_ed.point;return g_f7Nearest>=0&&g_f7Nearest<n?g_f7Nearest:-1;}
void F7WirePanel(){}
#include "f7layers.h"
#include "contextlayout.inl"
int main(){int total=0,failed=0;auto check=[&](bool good,const char* name){++total;if(!good){++failed;std::printf("FAIL %s\n",name);}};trafficdata::Route route;route.name="fixture";route.ring=false;route.closed=false;route.points={{0,0,0},{100,0,0},{100,100,0}};route.lanePlan.enabled=true;routemap::Camera camera;
    auto reset=[&](editorcontext::Surface surface,int sheet=0){g_context={};g_context.surface=surface;g_contextObserved="selection";g_contextNumber=-1;g_contextCategory=-1;g_contextDirectory=false;g_ed={};g_ed.sheet=sheet;g_ui.buttons.clear();g_ui.boxes.clear();g_panelText.clear();g_ui.panelBottom=500;g_ui.buttons.push_back({kSelect,100,50,70,44,"Select",true,true,"Q",1});g_ui.buttons.push_back({kTest,PanelX()+14,400,100,30,"Test",false,true,""});};
    auto count=[&](int id){return std::count_if(g_ui.buttons.begin(),g_ui.buttons.end(),[&](const auto& b){return b.id==id;});};
    auto overlaps=[&](){for(std::size_t i=0;i<g_ui.buttons.size();++i)for(std::size_t j=i+1;j<g_ui.buttons.size();++j){const auto& a=g_ui.buttons[i];const auto& b=g_ui.buttons[j];if(a.style==1||b.style==1)continue;if(a.x<b.x+b.w-.5&&b.x<a.x+a.w-.5&&a.y<b.y+b.h-.5&&b.y<a.y+a.h-.5)return true;}return false;};
    for(auto dimensions:std::vector<std::pair<double,double>>{{1280,720},{1366,768},{1920,1080},{2560,1440},{3440,1440}}){g_ui.width=dimensions.first;g_ui.height=dimensions.second;
        // Layer 2: the dock and its group tabs share the footer's centre line and sit above it at every resolution.
        for(int group=0;group<4;++group){reset(editorcontext::Surface::Edit);g_f7Group=group;g_roadPanel=false;ContextLayout(&route,camera);check(!g_contextPanel,"selection tucks legacy panel");
            const auto tab=std::find_if(g_ui.buttons.begin(),g_ui.buttons.end(),[](const auto& b){return b.id==kF7GroupBase;});
            check(tab!=g_ui.buttons.end()&&std::abs(tab->x-(g_ui.width*.5-318))<.01&&tab->y==g_ui.height-152,"dock tabs share footer centre at a fixed offset");
            for(int gi=0;gi<4;++gi)check(count(kF7GroupBase+gi)==1,"every group tab present once");
            for(int id:std::vector<int>{kContextMove,kContextTurn,kContextZ,kContextMore,kSave,kCancel,kTest})check(count(id)==1,"single visible common control");
            const int values=int(std::count_if(g_ui.buttons.begin(),g_ui.buttons.end(),[&](const auto& b){return b.y==g_ui.height-62;}));check(values>=3&&values<=4,"dock shows at most four values");
            const auto test=std::find_if(g_ui.buttons.begin(),g_ui.buttons.end(),[](const auto& b){return b.id==kTest;});
            for(int id:{kTest,kCancel,kSave}){const auto b=std::find_if(g_ui.buttons.begin(),g_ui.buttons.end(),[&](const auto& x){return x.id==id;});check(b!=g_ui.buttons.end()&&b->x>=g_ui.width*.5-330&&b->x+b->w<=g_ui.width*.5+330&&b->y>=g_ui.height-162,"Test / Cancel / Save sit inside the dock");}
            for(const auto& b:g_ui.buttons)check(b.x>=0&&b.y>=0&&b.x+b.w<=g_ui.width&&b.y+b.h<=g_ui.height-12,"actual native button remains on usable screen");
            check(!overlaps(),"no two controls overlap");check(std::count_if(g_ui.buttons.begin(),g_ui.buttons.end(),[](const auto& b){return b.x>=PanelX()&&b.y<78&&b.style!=1;})==0,"hidden panel does not leave invisible hit buttons");
            for(const auto& t:g_panelText)check(t.text.size()*f7layers::kLetterPx<=t.w+f7layers::kLetterPx,"dock and tag text fits its space");}
        reset(editorcontext::Surface::Edit);g_f7Group=2;ContextLayout(&route,camera);check(count(kContextWidth)==1&&count(kContextExact)==1,"position values keep width and exact entry");
        reset(editorcontext::Surface::Edit);g_f7Group=3;ContextLayout(&route,camera);check(count(kContextTravel)==1&&count(kContextVehicles)==1&&count(kContextMovement)==1,"rules values keep travel, vehicles and movement");
        reset(editorcontext::Surface::Edit);g_f7Group=0;ContextLayout(&route,camera);for(int k=1;k<=5;++k)check(count(kF7PrimBase+k)==1,"field group offers the five shapes");
        reset(editorcontext::Surface::Edit);g_f7Group=0;g_f7LinkPrompt=1;ContextLayout(&route,camera);for(int k=1;k<=3;++k)check(count(kF7LinkBase+k)==1,"a new road point offers the three join shapes");check(count(kF7PrimBase+1)==0,"join choice replaces the shape row");g_f7LinkPrompt=-1;
        // Layer 1: the tag. Nearest stays on screen; pinned can leave it, and then no tag is drawn.
        laneSelected=false;reset(editorcontext::Surface::Edit);g_f7Nearest=1;g_ed.point=-1;ContextLayout(&route,camera);
        check(count(kF7TagPrev)==0&&std::any_of(g_panelText.begin(),g_panelText.end(),[](const auto& t){return t.text.find("Tile 2-3")!=std::string::npos;}),"tag names the tile under the nearest point and has no buttons");
        check(std::any_of(g_panelText.begin(),g_panelText.end(),[](const auto& t){return t.text=="nearest";}),"nearest badge shown");
        route.points[1]={-5000,-5000,0};reset(editorcontext::Surface::Edit);g_f7Nearest=1;ContextLayout(&route,camera);
        const auto tag=std::find_if(g_panelText.begin(),g_panelText.end(),[](const auto& t){return t.text=="nearest";});check(tag!=g_panelText.end()&&tag->x>=12&&tag->y>=12&&tag->x<g_ui.width-12&&tag->y<g_ui.height-162,"nearest tag is clamped on screen above the dock");
        reset(editorcontext::Surface::Edit);g_ed.point=1;ContextLayout(&route,camera);check(std::none_of(g_panelText.begin(),g_panelText.end(),[](const auto& t){return t.text=="pinned";}),"pinned item off screen has no tag");route.points[1]={100,0,0};
        route.points[1]={600,250,0};reset(editorcontext::Surface::Edit);g_ed.point=1;g_f7Group=2;ContextLayout(&route,camera);route.points[1]={100,0,0};check(std::any_of(g_panelText.begin(),g_panelText.end(),[](const auto& t){return t.text=="pinned";})&&std::any_of(g_panelText.begin(),g_panelText.end(),[](const auto& t){return t.text.find("Point 2")!=std::string::npos;}),"pinned point tag names the point");
        laneSelected=true;g_f7Nearest=-1;g_f7Group=0;}
    reset(editorcontext::Surface::Popup,1);g_context.popup=editorcontext::Popup::Vehicles;g_ui.buttons.push_back({kTypeBase,PanelX()+14,100,300,22,"Vehicle model",false,true,""});g_panelText.push_back({"Inherited vehicle restrictions",PanelX()+14,78,320,kText,kUnit});ContextLayout(&route,camera);check(!g_contextPanel,"popup replaces browser");const auto model=std::find_if(g_ui.buttons.begin(),g_ui.buttons.end(),[](const auto& b){return b.id==kTypeBase;});check(model!=g_ui.buttons.end()&&model->x<PanelX(),"existing vehicle sheet relocates beside selected object");check(model!=g_ui.buttons.end()&&std::any_of(g_contextRects.begin(),g_contextRects.end(),[&](auto r){return r.Contains(model->x+1,model->y+1);}),"popup background and model hit agree");check(std::none_of(g_ui.buttons.begin(),g_ui.buttons.end(),[](const auto& b){return b.id==kContextMove;}),"popup avoids competing handle layer");
    reset(editorcontext::Surface::Advanced);g_contextDirectory=true;ContextLayout(&route,camera);check(std::count_if(g_ui.buttons.begin(),g_ui.buttons.end(),[](const auto& b){return b.id>=kContextCategories&&b.id<kContextCategories+10;})==10,"all ten advanced categories present");
    for(int category=0;category<10;++category){reset(editorcontext::Surface::Advanced);g_contextDirectory=true;g_contextCategory=category;ContextLayout(&route,camera);check(!g_contextLinks.empty(),"advanced category exposes real action links");check(std::count_if(g_ui.buttons.begin(),g_ui.buttons.end(),[](const auto& b){return b.id==kSave;})==1,"all advanced editors share one Save flow");}
    reset(editorcontext::Surface::Edit);laneSelected=false;g_ed.point=1;route.rounding={0,120,0};ContextLayout(&route,camera);for(int id:{kContextSharp,kContextRound,kContextCorner,kContextCornerDrag})check(std::any_of(g_ui.buttons.begin(),g_ui.buttons.end(),[&](auto b){return b.id==id&&b.enabled;}),"selected corner exposes precise bend controls");check(std::any_of(g_panelText.begin(),g_panelText.end(),[](const auto& t){return t.text.find("actual 45.00")!=std::string::npos;}),"corner displays attainable rather than only requested radius");
    reset(editorcontext::Surface::Edit);g_contextNumber=0;g_contextText="-12.5";ContextLayout(&route,camera);check(std::any_of(g_panelText.begin(),g_panelText.end(),[](const auto& t){return t.text.find("-12.5")!=std::string::npos;}),"exact entry is visible in native layout");
    // Layer 3: the Field group in the side panel, paged so it ends above the dock at every resolution.
    for(auto dimensions:std::vector<std::pair<double,double>>{{1280,720},{1920,1080}}){g_ui.width=dimensions.first;g_ui.height=dimensions.second;
        std::set<int> seen;for(int page=0;page<6;++page){reset(editorcontext::Surface::Edit);g_contextNumber=-1;g_roadPanel=true;g_f7FieldPage=page;ContextLayout(&route,camera);
            for(const auto& b:g_ui.buttons)if(b.enabled&&b.x>=PanelX())seen.insert(b.id);
            // 2026-11-11: the panel is 32 px wider, so its left edge now passes over the dock's Test / Cancel / Save at 1280 px; a panel
            // control is one that starts inside the panel's own rectangle, and the panel itself must end above the dock.
            const auto fieldPanel=g_f7AreaRect[2];check(fieldPanel.y+fieldPanel.h<=g_ui.height-170,"field panel ends above the dock");
            for(const auto& b:g_ui.buttons)if(b.x>=PanelX()&&b.y<fieldPanel.y+fieldPanel.h)check(b.y+b.h<=g_ui.height-170,"field panel ends above the dock");check(!overlaps(),"field panel controls do not overlap");}
        for(int id=9501;id<=9509;++id)check(seen.count(id)||id==9503||id==9504||id==9505||id==9510,"road panel exposes tile scope, dimensions, connectors and toggle");
        for(int id:{kF7FieldShape,kF7FieldWidth,kF7Taper})check(seen.count(id)==1,"field panel offers shape, width and taper");}
    g_ui.width=1920;g_ui.height=1080;reset(editorcontext::Surface::Edit);g_roadPanel=true;ContextLayout(&route,camera);check(count(9500)==1,"field panel toggle present");
    {auto narrow=route;narrow.road.enabled=true;narrow.road.width=6;narrow.lanes=2;reset(editorcontext::Surface::Edit);g_roadPanel=true;ContextLayout(&narrow,camera);
     auto has=[&](const char* t,unsigned c){return std::any_of(g_panelText.begin(),g_panelText.end(),[&](const auto& x){return x.text.find(t)!=std::string::npos&&x.color==c;});};
     check(has("2 lanes: 3.0 m each - small vehicles only",kAmber),"N lanes too tight shows a yellow warning");
     check(has("With 3 lanes: 2.0 m each - no vehicle fits",kClearanceRed),"N+1 lanes too narrow shows a red warning");
     narrow.road.width=14;reset(editorcontext::Surface::Edit);g_roadPanel=true;ContextLayout(&narrow,camera);check(has("2 lanes: 7.0 m each - all vehicles fit",kDim),"roomy lanes show no warning colour");}
    check(std::count_if(g_ui.buttons.begin(),g_ui.buttons.end(),[](const auto& b){return b.id==kSave;})==1,"road editing shares the existing Save transaction");
    std::printf("contextual_layout: %d checks, %d failures\n",total,failed);return failed?1:0;
}
