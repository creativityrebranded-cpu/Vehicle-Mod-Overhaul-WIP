// Landing wires (F7 spec 7b/7d): a new landing gets one In wire from the nearest lane upstream and one Out wire back
// downstream, routed in 3D round known obstacles from the mod's own nav memory (no engine queries). Unknown stretches
// keep their points and are marked red; with no route in reach the landing stays unconnected. Drafts until Save.
std::set<std::string> g_wireDrafts;std::map<std::string,std::vector<bool>> g_wireUnknown;  // key: landing id + "#" + wire index
autoroute::Result RouteWire(trafficdata::Vec from,trafficdata::Vec to,const trafficdata::Route* road){
    using namespace autoroute;Grid g;const double margin=40;
    const double x0=std::min(from.x,to.x)-margin,y0=std::min(from.y,to.y)-margin,z0=std::min(from.z,to.z)-10;
    const int nx=std::clamp(int(std::ceil((std::fabs(from.x-to.x)+2*margin)/kXY)),4,96),ny=std::clamp(int(std::ceil((std::fabs(from.y-to.y)+2*margin)/kXY)),4,96);
    const int nz=std::clamp(int(std::ceil((std::fabs(from.z-to.z)+30)/kZ)),6,20);
    g.Resize(x0,y0,z0,nx,ny,nz);
    for(int j=0;j<ny;++j)for(int i=0;i<nx;++i)for(int k=0;k<nz;++k){const auto m=navmap::At(g.X(i),g.Y(j),g.Z(k));auto& v=g.At(i,j,k);
        if((m.haveGround&&g.Z(k)<=m.ground)||(m.blocked&&std::fabs(m.blockedZ-g.Z(k))<=kZ)||(m.objBlocked&&!m.objOverruled))v.state=State::Solid;
        else if(m.free||(m.haveGround&&g.Z(k)>m.ground))v.state=State::Free;}
    Params p;p.drive=!road||roadplan::PrimitiveAt(road->road,0).kind!=roadplan::Tube;p.halfWidth=(road?road->corridor:7)*.5+.5;p.height=road&&road->road.height>0?road->road.height:3;p.budget=150000;
    auto snap=[&](trafficdata::Vec q){Point at{q.x,q.y,q.z};for(int dk=-1;dk<=4;++dk){const int k=g.K(q.z)+dk;if(g.Inside(g.I(q.x),g.J(q.y),k)&&detail::Passable(g,g.I(q.x),g.J(q.y),k,p)){at.z=g.oz+k*kZ+.1;break;}}return at;};
    auto r=Search(g,snap(from),snap(to),p);if(r.ok&&p.drive)for(auto& q:r.points)q.z+=kGroundLift;return r;
}
void AutoWires(const std::string& id){
    const int index=LandingIndex(id);if(index<0)return;auto& spot=g_ed.file.landings[std::size_t(index)];if(!spot.wires.empty()||!placeaccess::Empty(spot.access))return;  // only new, unwired landings
    double best=250;std::string route;int lane=-1;double u=0;trafficroute::Line line;
    for(const auto& r:g_ed.file.routes){if(!Usable(r))continue;trafficroute::Line l;bool ring=false;if(!LineOf(r.id,l,ring)||l.error)continue;
        for(int k:trafficroute::Lanes(l,LanesOf(r.id))){const double at=trafficroute::Nearest(l,k,spot.at.x,spot.at.y);const auto q=trafficroute::At(l,k,at);const double d=std::hypot(q.x-spot.at.x,q.y-spot.at.y);if(d<best){best=d;route=r.id;lane=k;u=at;line=l;}}}
    if(route.empty()){Status("Landing added with no route nearby (250 m): unconnected.");logger::Write("OBS system=traffic.place event=wire_refused place=%s reason=no_route_nearby",id.c_str());return;}
    const trafficdata::Route* road=nullptr;trafficdata::Route td;for(const auto& r:g_ed.file.routes)if(r.id==route){td=ToTd(r,"");road=&td;}
    const auto upstream=trafficroute::At(line,lane,trafficroute::LeadU(line,lane,u,30,true)),downstream=trafficroute::At(line,lane,trafficroute::LeadU(line,lane,u,30,false));
    const trafficdata::Vec landing{spot.at.x,spot.at.y,spot.at.z};
    for(int socket:{0,1}){const auto a=socket?landing:upstream,b=socket?downstream:landing;placeaccess::Wire w;w.socket=socket;w.route=route;w.lane=lane;
        const auto r=RouteWire(a,b,road);std::vector<bool> unknown;
        if(r.ok)for(std::size_t n=socket?1:0;n+(socket?0:1)<r.points.size();++n){w.path.push_back({r.points[n].x,r.points[n].y,r.points[n].z});unknown.push_back(r.points[n].unknown);}
        if(w.path.empty()){w.path.push_back(socket?placeaccess::Point{b.x,b.y,b.z}:placeaccess::Point{a.x,a.y,a.z});unknown.push_back(true);}
        if(w.path.size()>128){w.path.resize(128);unknown.resize(128);}
        logger::Write("OBS system=f7.autoroute event=%s place=%s socket=%s route=%s points=%zu expanded=%zu unknown=%zu field=%zu reason=%s",r.ok?"done":"fallback",id.c_str(),socket?"out":"in",route.c_str(),w.path.size(),r.expanded,r.unknownCells,r.fieldCells,r.why);
        const auto key=id+"#"+std::to_string(spot.wires.size());g_wireUnknown[key]=unknown;g_wireUnknownDraw[key]=unknown;g_wireDrafts.insert(key);spot.wires.push_back(w);}
    placeaccess::Mirror(spot.access,spot.wires);
    logger::Write("OBS system=traffic.place event=wire_built place=%s route=%s lane=%d wires=2",id.c_str(),route.c_str(),lane);
    Status("Landing wired to "+route+": In and Out drafts built round known obstacles (red = unknown). Edit them; Save keeps them.");
}
// Routes whose lanes are too narrow for any traffic model (2026-11-04: Roadrunner at 1 m held all 66 positions empty).
// Names the first such route for the reset card and the status line; empty when every lane can take a vehicle.
std::string NarrowLaneNote(){
    constexpr double kNarrowest=2.2;  // the smallest traffic hull is about 2.2 m wide
    int count=0;std::string first;double width=0;
    for(const auto& r:g_ed.file.routes){if(!Usable(r)||r.ring)continue;const int lanes=std::max(1,r.road.dividers.empty()?std::clamp(r.lanes,1,2):int(r.road.dividers.size())+1);
        const double w=r.lanePlan.enabled?r.lanePlan.base.profile.width:r.road.enabled?(r.road.width>0?r.road.width:r.corridor*std::clamp(r.lanes,1,2))/lanes:r.corridor;
        if(w<kNarrowest){if(!count){first=r.label.empty()?r.id:r.label;width=w;}++count;}}
    if(!count)return {};
    char b[160];std::snprintf(b,sizeof b,"%s lanes are %.1f m: too narrow for any vehicle%s. Widen in Position > Width.",first.c_str(),width,count>1?(" (+"+std::to_string(count-1)+" more)").c_str():"");
    return b;
}

// F7 layout (Arrange): offsets of the toolbar, dock, side panel and messages, beside FVO's hoverui.ini.
std::wstring F7LayoutPath(){PWSTR docs=nullptr;std::wstring path;if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents,0,nullptr,&docs))){path=std::wstring(docs)+L"\\My Games\\Starfield\\SFSE\\Logs\\FlyingVehicleOverhaul\\f7layout.ini";}if(docs)CoTaskMemFree(docs);return path;}
void SaveF7Layout(){const auto path=F7LayoutPath();if(path.empty())return;std::ofstream out(path,std::ios::trunc);out<<"; F7 Traffic Manager layout (Arrange): area dx dy in screen units\n";
    const char* names[]{"toolbar","dock","panel","messages"};for(int a=0;a<4;++a)out<<names[a]<<" = "<<g_f7Offset[std::size_t(a)].dx<<" "<<g_f7Offset[std::size_t(a)].dy<<"\n";
    for(const auto& [key,off]:g_f7NodeOffsets())out<<"node "<<key<<" = "<<off.first<<" "<<off.second<<"\n";}
void LoadF7Layout(){static bool done=false;if(done)return;done=true;const auto path=F7LayoutPath();if(path.empty())return;std::ifstream in(path);std::string line;
    const char* names[]{"toolbar","dock","panel","messages"};while(std::getline(in,line)){
        if(line.rfind("node ",0)==0){const auto eq=line.find(" = ");if(eq!=std::string::npos){std::istringstream v(line.substr(eq+3));double x=0,y=0;if(v>>x>>y&&std::isfinite(x)&&std::isfinite(y))g_f7NodeOffsets()[line.substr(5,eq-5)]={std::clamp(x,-4000.0,4000.0),std::clamp(y,-4000.0,4000.0)};}continue;}
        for(int a=0;a<4;++a){const std::string key=std::string(names[a])+" = ";if(line.rfind(key,0)==0){std::istringstream v(line.substr(key.size()));double x=0,y=0;if(v>>x>>y&&std::isfinite(x)&&std::isfinite(y))g_f7Offset[std::size_t(a)]={std::clamp(x,-4000.0,4000.0),std::clamp(y,-4000.0,4000.0)};}}}
    logger::Write("OBS system=f7.arrange event=loaded path=f7layout.ini");}
