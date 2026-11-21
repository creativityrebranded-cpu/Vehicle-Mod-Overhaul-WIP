// Contextual F7 surfaces and gestures. Uses only the existing editor, input and drawing ports.

std::string ContextKey(){trafficdata::Route scratch;const auto* r=Shown(scratch);std::string key=r?r->name:std::string();key+="/"+std::to_string(g_ed.point)+"/"+std::to_string(g_ed.spot)+"/"+std::to_string(g_ed.zone)+"/"+std::to_string(g_ed.link);for(const auto& p:g_ed.lanePicks)key+="/"+p.route+":"+std::to_string(p.section)+":"+std::to_string(p.identity)+":"+std::to_string(p.lane);return key;}
bool ContextLane(){return !g_ed.lanePicks.empty();}
lanetransform::Vec ContextCentre(const trafficdata::Route& r){
    if(ContextLane()){static routecache::Cache cache;const auto& line=cache.Get(r);lanetransform::Vec sum{};int n=0;for(const auto& pick:g_ed.lanePicks)if(pick.route==r.name&&pick.lane>=0&&pick.lane<int(line.lane.size())){const int section=pick.Resolve(r.lanePlan);for(int i=0;i<line.Count();++i)if(section<0||int(routepolicy::Segment(r.points.size(),r.closed,line.ToSource(i),0))==section){const auto p=line.lane[pick.lane][i];sum=lanevolume::Add(sum,{p.x,p.y,p.z});++n;}}if(n)return lanevolume::Mul(sum,1.0/n);}
    if(g_ed.spot>=0){const auto& list=g_ed.spotKind==1?g_ed.file.landings:g_ed.file.spawns;if(g_ed.spot<int(list.size())){auto p=list[g_ed.spot].at;return {p.x,p.y,p.z};}}
    if(g_ed.zone>=0&&g_ed.zone<int(g_ed.file.zones.size())){const auto& zone=g_ed.file.zones[g_ed.zone];auto p=zone.at;return {p.x,p.y,zone.slow?p.z:zone.top};}
    if(g_ed.point>=0&&g_ed.point<int(r.points.size())){auto p=r.points[g_ed.point];return {p.x,p.y,p.z};}
    const auto p=routeeditor::Centre(r);return {p.x,p.y,p.z};
}
// 2026-11-11 (user: "I tried to make it one way by reducing number of lanes but it didn't let me"): the commit re-checked every
// point of the route, so one stored point that now reads under the ground (lc_ring_road, (-1275, 1962) at 22 m) refused a lane
// count edit that moved no point. Only points this edit adds or moves are checked now; unchanged points were accepted when they
// were placed and keep their own red warnings. Moved points still meet the terrain check (mandate: proven terrain violations).
bool ContextFits(const trafficdata::Route& next){
    trafficdata::Route held;bool have=false;
    if(g_ed.draft&&g_ed.draft->Value().name==next.name){held=g_ed.draft->Value();have=true;}
    else if(const int index=RouteIndex(next.name);index>=0){held=ToTd(g_ed.file.routes[std::size_t(index)],EditorKey());have=true;}
    if(!have)return RouteFits(next,"Selected geometry");
    if(next.ring){if(held.ring&&held.shape.cx==next.shape.cx&&held.shape.cy==next.shape.cy&&held.shape.z==next.shape.z)return true;return RouteFits(next,"Selected geometry");}
    int kept=0,checked=0;
    for(std::size_t i=0;i<next.points.size();++i){const auto& p=next.points[i];
        if(!held.ring&&i<held.points.size()&&held.points[i].x==p.x&&held.points[i].y==p.y&&held.points[i].z==p.z){++kept;continue;}
        ++checked;if(const char* why=PointProblem(p.x,p.y,p.z)){Refused("Selected geometry",p.x,p.y,p.z,why);
            logger::Write("OBS system=f7.commit event=refused route=%s point=%d kept=%d checked=%d reason=moved_point_%s",next.name.c_str(),int(i)+1,kept,checked,why);return false;}}
    static int told=0;if(kept&&told<40){++told;logger::Write("OBS system=f7.commit event=checked route=%s kept=%d checked=%d reason=unchanged_points_not_rechecked",next.name.c_str(),kept,checked);}
    return true;
}
bool ContextCommit(const std::map<std::string,trafficdata::Route>& staged,bool merge=false){
    static ULONGLONG lastAt=0;static std::string lastKey;const auto now=GetTickCount64();const auto key=ContextKey();const bool join=merge&&now-lastAt<400&&lastKey==key&&!g_ed.massUndo.empty()&&g_ed.massUndo.back().depth==DraftDepth();
    for(const auto& [name,r]:staged)if(laneplan::Problem(r.lanePlan)||trafficroute::Build(r).error||(!ContextLane()&&!ContextFits(r))){Status("Selection change refused; all targets kept unchanged.");return false;}
    if(staged.empty())return false;if(!join)PushSnap();bool changed=false;
    for(const auto& [name,r]:staged)if(g_ed.draft&&name==g_ed.draft->Value().name){if(join)g_ed.draft->Amend(r);else g_ed.draft->Edit(r);changed=true;}else if(const int index=RouteIndex(name);index>=0)g_ed.file.routes[index]=FromTd(r);
    if(changed){g_ed.massUndo.back().draftChanged=true;g_ed.massUndo.back().depth=DraftDepth();}lastAt=merge?now:0;lastKey=key;Status("Selected geometry changed. Test previews; Save keeps it.");return true;
}
std::map<std::string,trafficdata::Route> ContextBases(){
    std::map<std::string,trafficdata::Route> result;if(!EnsureDraft())return result;result[g_ed.draft->Value().name]=g_ed.draft->Value();
    for(const auto& pick:g_ed.lanePicks)if(!result.count(pick.route)){const int index=RouteIndex(pick.route);if(index>=0&&Usable(g_ed.file.routes[index]))result[pick.route]=ToTd(g_ed.file.routes[index],EditorKey());else {Status("Edit the selected original route individually before including it in a multi-route edit. No targets changed.");return {};}}return result;
}
std::map<std::string,trafficdata::Route> ContextTransform(std::map<std::string,trafficdata::Route> staged,lanetransform::Vec delta,lanetransform::Matrix matrix){
    for(auto& [name,r]:staged){const auto centre=ContextCentre(g_contextDragBase.count(name)?g_contextDragBase.at(name):r);if(ContextLane()){
        for(const auto& pick:g_ed.lanePicks)if(pick.route==name){const int section=pick.Resolve(r.lanePlan);if(section< -1){r.lanePlan.transforms.unsupported="stale selection";continue;}auto& t=lanetransform::Edit(r.lanePlan.transforms,pick.lane,section,centre);lanetransform::Compose(t,matrix,centre);lanetransform::Translate(t,delta);}
    }else if(g_ed.point>=0&&g_ed.point<int(r.points.size())){auto& p=r.points[g_ed.point];p.x+=delta.x;p.y+=delta.y;p.z+=delta.z;if(r.spacePlan.enabled&&g_ed.point<int(r.spacePlan.handles.size())){auto& h=r.spacePlan.handles[g_ed.point];h.incoming=lanetransform::Apply(matrix,h.incoming);h.outgoing=lanetransform::Apply(matrix,h.outgoing);}}
    else {r=routeeditor::Translated(r,delta.x,delta.y,delta.z);if(matrix!=lanetransform::Identity()){
        const auto pivot=lanevolume::Add(centre,delta);auto point=[&](auto& p){const auto q=lanevolume::Add(pivot,lanetransform::Apply(matrix,lanevolume::Sub({p.x,p.y,p.z},pivot)));p={q.x,q.y,q.z};};auto vector=[&](auto& p){const auto q=lanetransform::Apply(matrix,{p.x,p.y,p.z});p={q.x,q.y,q.z};};
        for(auto& p:r.points)point(p);for(auto& h:r.spacePlan.handles){vector(h.incoming);vector(h.outgoing);}for(auto& path:r.spacePlan.lanes)for(auto& anchor:path.anchors){point(anchor.point);vector(anchor.handle.incoming);vector(anchor.handle.outgoing);}for(auto& stop:r.journey.stops){lanetransform::Vec p{stop.x,stop.y,stop.z};point(p);stop.x=p.x;stop.y=p.y;stop.z=p.z;}
        if(r.ring){const double scale=std::hypot(matrix[0],matrix[3],matrix[6]);r.shape.radius*=scale;r.shape.rotation+=std::atan2(matrix[3],matrix[0]);}
        lanetransform::Parent(r.lanePlan.transforms,matrix,pivot);
    }}
    }return staged;
}
bool ContextLaneStep(lanetransform::Vec delta,double turn,double scale){
    if(!ContextLane())return false;if(!AddonAuthoring(16)){Status("Lane transforms require the updated traffic addon; saved records are retained.");return true;}
    auto matrix=lanetransform::Rotation(2,turn);for(auto& v:matrix)v*=scale;ContextCommit(ContextTransform(ContextBases(),delta,matrix),true);return true;
}
bool ContextPlaceActive(){return g_ed.spot>=0||g_ed.zone>=0;}
bool ContextPlaceEdit(const routefiles::File& base,lanetransform::Vec delta,double turn,bool commit){
    auto next=base;trafficdata::Vec point{};
    if(g_ed.zone>=0&&g_ed.zone<int(next.zones.size())){auto& z=next.zones[g_ed.zone];z.at.x+=delta.x;z.at.y+=delta.y;if(z.slow)z.at.z+=delta.z;else z.top+=delta.z;point={z.at.x,z.at.y,z.slow?z.at.z:z.top};}
    else {auto& list=g_ed.spotKind==1?next.landings:next.spawns;if(g_ed.spot<0||g_ed.spot>=int(list.size()))return false;auto& spot=list[g_ed.spot];const auto origin=spot.at;spot.at={origin.x+delta.x,origin.y+delta.y,origin.z+delta.z};spot.heading+=turn/kDeg;spot.turn+=turn/kDeg;for(auto* path:{&spot.access.entrance,&spot.access.departure})for(auto& p:*path){const auto q=lanetransform::Apply(lanetransform::Rotation(2,turn),{p.x-origin.x,p.y-origin.y,p.z-origin.z});p={spot.at.x+q.x,spot.at.y+q.y,spot.at.z+q.z};}point=spot.at;}
    if(PointProblem(point.x,point.y,point.z)){if(commit)Status("Invalid place position; draft kept unchanged.");return false;}
    if(commit){g_ed.file=base;PushSnap();}g_ed.file=std::move(next);return true;
}
bool ContextCornerEdit(double radius){
    if(!std::isfinite(radius)||radius<0||radius>200||!EnsureDraft())return false;auto r=g_ed.draft->Value();if(g_ed.point<0||g_ed.point>=int(r.points.size())||r.ring)return false;
    if(r.spacePlan.enabled){if(radius!=0){Status("This corner uses spline tangents. Edit its incoming / outgoing handles in Geometry.");return false;}if(g_ed.point>=int(r.spacePlan.handles.size()))return false;r.spacePlan.handles[g_ed.point].incoming={};r.spacePlan.handles[g_ed.point].outgoing={};}
    else {if(radius>0&&routecurve::CornerLimit(r.points,g_ed.point,r.closed)<=0){Status("Select an interior turning point to round.");return false;}r.rounding.resize(r.points.size());r.rounding[g_ed.point]=radius;}
    return ContextCommit({{r.name,r}});
}
// Three-layer F7: the pinned point (clicked), else the one nearest the view centre (live while panning); -1 none.
int F7Point(const trafficdata::Route& r){const int n=int(r.points.size());if(g_ed.point>=0&&g_ed.point<n)return g_ed.point;return g_f7Nearest>=0&&g_f7Nearest<n?g_f7Nearest:-1;}
std::size_t F7Tile(const trafficdata::Route& r){const int n=int(r.points.size());return n>1?std::size_t(std::min(std::max(0,F7Point(r)),r.closed?n-1:n-2)):0;}
// Turning road tiles on converts the route's existing lanes into one road with red dividers.
// The old lane records stay in the route, unused while road tiles are on.
void EnableRoad(trafficdata::Route& r){
    if(r.road.enabled)return;
    r.road.tiles.resize(r.points.size());
    // Only a road never laid out is converted; switching tiles back on keeps hand-made dividers.
    const bool fresh=r.road.width==0&&r.road.dividers.empty()&&r.road.directions.empty()&&
        std::all_of(r.road.tiles.begin(),r.road.tiles.end(),[](const roadplan::Tile& t){return t.side==0&&t.swing==0&&t.tilt==0;});
    if(fresh&&roadmigrate::HasLaneGeometry(r)){
        const auto result=roadmigrate::FromLanes(r);
        if(result.changed){char text[160];std::snprintf(text,sizeof text,"%s: %d lane%s kept in place as one road with %d divider%s.",r.name.c_str(),result.lanes,result.lanes==1?"":"s",result.lanes-1,result.lanes==2?"":"s");Status(text);}
        else Status(r.name+": lanes left as drawn ("+result.why+"); road tiles use the default split.");
        logger::Write("OBS system=route.migrate route=%s event=%s lanes=%d reason=%s",r.name.c_str(),result.changed?"converted":"kept",result.lanes,result.changed?"lanes_to_dividers":result.why.c_str());
    }
    r.travel=routepolicy::Travel::Drive;r.road.enabled=true;g_ed.view.volumes=true;r.road.tiles.resize(r.points.size());
}
int RoadLanes(const trafficdata::Route& r){return r.road.dividers.empty()?std::clamp(r.lanes,1,2):int(r.road.dividers.size())+1;}
// Lane count edit: 1 one way, 2 the legacy single divider, 3+ explicit divider rows (older readers keep two lanes).
bool SetRoadLanes(trafficdata::Route& r,int lanes){
    if(lanes<1||lanes>16)return false;
    const int before=RoadLanes(r);if(lanes==before)return true;
    if(lanes<=2){
        if(!r.road.dividers.empty())for(std::size_t i=0;i<r.road.tiles.size();++i){const auto d=roadplan::DividerAt(r.road,0,i);auto& t=r.road.tiles[i];t.side=d.side;t.swing=d.swing;t.tilt=d.tilt;}
        r.road.dividers.clear();r.road.directions.clear();r.lanes=lanes;return true;
    }
    roadplan::SetDividerCount(r.road,lanes-1,r.points.size());r.lanes=2;return true;
}
// Set the shape of the selected tile (or all tiles); a roofed shape on a flat road adds a 5 m roof.
bool F7SetPrimitive(int kind){
    trafficdata::Route scratch;const auto* shown=Shown(scratch);
    if(!shown||shown->ring){Status("Shapes apply to point routes with road tiles.");return false;}
    if(shown->points.empty()){g_f7NextPrim=kind;Status(std::string("This click: ")+roadplan::KindName(kind));logger::Write("OBS system=f7.primitive event=next_click kind=%s",roadplan::KindName(kind));return true;}
    if(!AddonAuthoring(32)){Status("Road tiles need a traffic addon with road support; existing routes remain available.");return false;}
    auto staged=ContextBases();int from=-1;
    for(auto& [name,r]:staged){if(r.ring||r.points.size()<2)return false;EnableRoad(r);r.road.primitives.resize(r.points.size());
        if(kind!=roadplan::Flat&&r.road.height<=0){r.road.height=5;Status("A 5 m roof was added for the roofed shape.");}
        for(std::size_t i=0;i<r.points.size();++i){if(!g_roadAll&&i!=F7Tile(r))continue;from=r.road.primitives[i].kind;r.road.primitives[i]={kind,roadplan::DefaultParam(kind)};}
        if(!roadplan::Valid(r.road))return false;}
    g_f7NextPrim=kind;
    logger::Write("OBS system=f7.primitive point=%d from=%s to=%s scope=%s",int(F7Tile(*shown))+1,roadplan::KindName(std::max(0,from)),roadplan::KindName(kind),g_roadAll?"all_tiles":"tile");
    return ContextCommit(staged);
}
// Join shape at the preceding marker: 1 rounded bend, 2 sharp join, 3 taper.
bool F7SetLink(int shape){
    trafficdata::Route scratch;const auto* shown=Shown(scratch);if(!shown||shown->ring||shown->points.size()<2)return false;
    if(!AddonAuthoring(32)){Status("Road tiles need a traffic addon with road support; existing routes remain available.");return false;}
    auto staged=ContextBases();
    for(auto& [name,r]:staged){if(r.ring||r.points.size()<2)return false;EnableRoad(r);r.road.tiles.resize(r.points.size());
        const auto index=g_f7LinkPrompt>=0?std::size_t(std::min<int>(g_f7LinkPrompt,int(r.points.size())-2)):std::min(std::size_t(std::max(0,F7Point(r)-1)),r.points.size()-2);
        if(g_roadAll)for(auto& tile:r.road.tiles)tile.connector=shape;else r.road.tiles[index].connector=shape;}
    logger::Write("OBS system=f7.link shape=%s marker=%d scope=%s",f7layers::LinkName(shape),g_f7LinkPrompt,g_roadAll?"all_tiles":"tile");
    g_f7LinkPrompt=-1;return ContextCommit(staged);
}
bool ContextField(int field,double value){
    if(field!=15&&field!=14&&field!=17&&field!=18&&field!=19){static int told=0;if(told<20){++told;logger::Write("OBS system=routeedit.basic event=refused field=%d reason=not_a_primitive_field",field);}return false;}  // road width, plus the restored primitives lane count (14) and divider width (17), 2026-11-06
    if(field==14||field==17)logger::Write("OBS system=routeedit.primitive event=field field=%d value=%.2f",field,value);
    if(field==18||field==19){  // edge no-go bands (left 18, right 19): outside every lane; 0 = none, so older routes read as before
        if(!AddonAuthoring(32)){Status("Road tiles need a traffic addon with road support; existing routes remain available.");return false;}
        if(value<0||value>50){Status("Edge width: 0 to 50 m.");return false;}
        auto staged=ContextBases();for(auto& [name,r]:staged){if(r.ring||r.points.size()<2)return false;EnableRoad(r);(field==18?r.road.edgeLeft:r.road.edgeRight)=value;
            if(!roadplan::Valid(r.road)||trafficroute::Build(r).error){Status("That edge width leaves no room for the lanes.");return false;}}
        logger::Write("OBS system=f7.field event=%s value=%.2f",field==18?"edge_left_width":"edge_right_width",value);return ContextCommit(staged);
    }
    if(field==17){  // red divider width: a median between lanes, the same on every divider
        if(!AddonAuthoring(32)){Status("Road tiles need a traffic addon with road support; existing routes remain available.");return false;}
        if(value<0||value>50){Status("Divider width: 0 to 50 m.");return false;}
        auto staged=ContextBases();for(auto& [name,r]:staged){if(r.ring||r.points.size()<2)return false;EnableRoad(r);r.road.median=value;
            if(!roadplan::Valid(r.road)||trafficroute::Build(r).error){Status("That divider width leaves no room for the lanes.");return false;}}
        logger::Write("OBS system=f7.field event=divider_width value=%.2f",value);return ContextCommit(staged);
    }
    if(field==15||field==16){
        if(!AddonAuthoring(32)){Status("Road tiles need a traffic addon with road support; existing routes remain available.");return false;}
        auto staged=ContextBases();
        for(auto& [name,r]:staged){if(r.ring||r.points.size()<2)return false;EnableRoad(r);
            if(field==15){if(value<1||value>400){Status("Road width: 1 to 400 m.");return false;}r.road.width=value;}
            else {r.road.primitives.resize(r.points.size());for(std::size_t i=0;i<r.points.size();++i){if(!g_roadAll&&i!=F7Tile(r))continue;auto k=roadplan::PrimitiveAt(r.road,i);
                if(k.kind==roadplan::Rounded){if(value<0||value>50){Status("Corner radius: 0 to 50 m.");return false;}k.param=value;}
                else if(k.kind==roadplan::Trapezoid){if(value<10||value>100){Status("Top width: 10 to 100 percent.");return false;}k.param=value/100;}
                else {Status("This shape has no value to set.");return false;}
                r.road.primitives[i]=k;}}
            if(!roadplan::Valid(r.road))return false;
            if(trafficroute::Build(r).error){Status("Dividers may not cross each other or leave the road.");return false;}}
        return ContextCommit(staged);
    }
    if(field==14){
        if(!AddonAuthoring(32)){Status("Road tiles need a traffic addon with road support; existing routes remain available.");return false;}
        if(value!=std::floor(value)||value<1||value>16){Status("Lanes: a whole number from 1 to 16.");return false;}
        auto staged=ContextBases();
        for(auto& [name,r]:staged){if(r.ring||r.points.size()<2)return false;EnableRoad(r);if(!SetRoadLanes(r,int(value)))return false;}
        g_roadDivider=g_roadLane=0;Status("Dividers spread evenly; select each one to move, swing or tilt it.");
        return ContextCommit(staged);
    }
    if(field>=10&&field<=13) {
        if(!AddonAuthoring(32)){Status("Road tiles need a traffic addon with road support; existing routes remain available.");return false;}
        auto staged=ContextBases();
        for(auto& [name,r]:staged){
            if(r.ring||r.points.size()<2)return false;
            EnableRoad(r);
            r.road.tiles.resize(r.points.size());
            if(field==10){if(value<0||value>200)return false;r.road.height=value;}
            else for(std::size_t i=0;i<r.points.size()-(r.closed?0:1);++i){
                if(!g_roadAll&&i!=F7Tile(r))continue;
                auto& t=r.road.dividers.empty()?r.road.tiles[i]:r.road.dividers[std::size_t(std::clamp(g_roadDivider,0,int(r.road.dividers.size())-1))][i];
                if(field==11)t.side=value/100;
                if(field==12)t.swing=value/100;
                if(field==13)t.tilt=value;
            }
            if(!roadplan::Valid(r.road))return false;
            if(trafficroute::Build(r).error){Status("Dividers may not cross each other or leave the road.");return false;}
        }
        return ContextCommit(staged);
    }
    if(field==9)return ContextCornerEdit(value);
    if(ContextPlaceActive()){trafficdata::Route empty;const auto centre=ContextCentre(empty);lanetransform::Vec delta{};if(field<3){(field==0?delta.x:field==1?delta.y:delta.z)=value-(field==0?centre.x:field==1?centre.y:centre.z);return ContextPlaceEdit(g_ed.file,delta,0,true);}if(field==5)return ContextPlaceEdit(g_ed.file,{},value*kDeg,true);return false;}
    if(ContextLane()&&!AddonAuthoring(16)&&field<7){Status("Exact lane transforms require the updated traffic addon.");return false;}

    auto staged=ContextBases();if(staged.empty())return false;
    if(field>=0&&field<3){trafficdata::Route scratch;const auto* shown=Shown(scratch);if(!shown)return false;const auto centre=ContextCentre(*shown);lanetransform::Vec delta{};(field==0?delta.x:field==1?delta.y:delta.z)=value-(field==0?centre.x:field==1?centre.y:centre.z);return ContextCommit(ContextTransform(std::move(staged),delta,lanetransform::Identity()));}
    if(field>=3&&field<=5){if(std::fabs(value)>36000)return false;return ContextCommit(ContextTransform(std::move(staged),{},lanetransform::Rotation(field-3,value*kDeg)));}
    if(field==6){if(value<.01||value>100)return false;return ContextCommit(ContextTransform(std::move(staged),{},{value,0,0,0,value,0,0,0,value}));}
    if(field==7||field==8){if(value<1||value>(field==7?100:200))return false;for(auto& [name,r]:staged){if(!r.lanePlan.enabled){r.lanePlan.enabled=true;r.lanePlan.base.forward=1;r.lanePlan.base.reverse=r.lanes>1?1:0;r.lanePlan.base.profile.width=r.corridor;r.lanePlan.base.gap=std::max(0.0,r.width-r.corridor);r.lanePlan.base.travel=r.travel;laneplan::EnsureSections(r.lanePlan,r.points.size());}
        auto set=[&](int section,int lane){auto s=laneplan::Resolve(r.lanePlan,section,lane);if(field==7)s.profile.width=value;else s.profile.height=value;laneplan::Set(r.lanePlan,section,lane,field==7?laneplan::Width:laneplan::Height,s);};if(ContextLane()){for(const auto& pick:g_ed.lanePicks)if(pick.route==name)set(pick.Resolve(r.lanePlan),pick.lane);}else set(-1,-1);}
        return ContextCommit(staged);
    }return false;
}
void ContextBeginNumber(int field){trafficdata::Route scratch;const auto* r=Shown(scratch);trafficdata::Route empty;if(!r&&ContextPlaceActive())r=&empty;if(!r)return;const auto c=ContextCentre(*r);auto s=laneplan::Resolve(r->lanePlan,g_ed.laneScope?g_ed.point:-1,g_ed.laneFocus);if(!r->lanePlan.enabled){s.profile.width=r->corridor;s.travel=r->travel;}double v=field==0?c.x:field==1?c.y:field==2?c.z:field==6?1:field==7?s.profile.width:field==8?s.profile.height:field==9&&g_ed.point>=0&&g_ed.point<int(r->rounding.size())?r->rounding[g_ed.point]:0;if(field==17)v=r->road.median;else if(field==15)v=r->road.width>0?r->road.width:r->corridor*std::clamp(r->lanes,1,2);else if(field==16)v=roadplan::PrimitiveAt(r->road,F7Tile(*r)).kind==roadplan::Trapezoid?roadplan::PrimitiveAt(r->road,F7Tile(*r)).param*100:roadplan::PrimitiveAt(r->road,F7Tile(*r)).param;else if(field==14)v=RoadLanes(*r);else if(field>=10){const auto t=roadplan::DividerAt(r->road,std::size_t(std::max(0,g_roadDivider)),F7Tile(*r));v=field==10?r->road.height:field==11?t.side*100:field==12?t.swing*100:t.tilt;}char value[64];std::snprintf(value,sizeof value,"%.6g",v);g_contextNumber=field;g_contextText=value;g_contextReplace=true;g_contextNumberKey=ContextKey();}
bool ContextAction(int id,const routemap::Camera& c){
    if(BasicRefused(id))return true;  // the basic road editor (routeedit.cpp BasicTool): not a basic tool, consumed
    if(((id>=9502&&id<=9509)||id==9512)&&!AddonAuthoring(32)){Status("Road tiles need a traffic addon with road support; existing routes remain available.");return true;}
    if(id==9500){g_roadPanel=!g_roadPanel;return true;}
    if(id==9509){auto staged=ContextBases();for(auto& [name,r]:staged){if(r.road.enabled)r.road.enabled=false;else EnableRoad(r);}ContextCommit(staged);return true;}
    if(id==9501){g_roadAll=!g_roadAll;return true;}
    if(id==9508){ContextBeginNumber(14);return true;}
    if(F7WireAction(id))return true;
    if(id==kF7NodeView){g_f7NodeView=!g_f7NodeView;Status(g_f7NodeView?"Node view: drag a landing socket to a route (wire) or to a landing (new route); drag nodes to tidy (double-click returns); Ctrl+click nodes then Enter makes a route; Del removes a selected wire.":"Map view.");logger::Write("OBS system=f7.node event=%s",g_f7NodeView?"view_on":"view_off");return true;}
    if(id==kF7Arrange){g_f7Arrange=!g_f7Arrange;g_f7ArrangeDrag=-1;if(!g_f7Arrange)SaveF7Layout();Status(g_f7Arrange?"Arrange: drag the toolbar, dock, side panel or messages; Reset puts one back. Arrange again to finish.":"Layout saved.");logger::Write("OBS system=f7.arrange event=%s",g_f7Arrange?"on":"off_saved");return true;}
    if(id>=kF7GroupBase&&id<kF7GroupBase+4){g_f7Group=id-kF7GroupBase;g_f7Highlight=-1;if(g_f7Group!=f7layers::Field){const int bit=g_f7Group==f7layers::Flow?0:g_f7Group==f7layers::Position?1:2;g_ed.groups=1<<bit;g_ed.lastGroup=bit;}  // the side panel shows only the dock group
    logger::Write("OBS system=f7.group open=%s",f7layers::GroupName(g_f7Group));return true;}
    if(id>kF7PrimBase&&id<=kF7PrimBase+5){F7SetPrimitive(id-kF7PrimBase);return true;}
    if(id>kF7LinkBase&&id<=kF7LinkBase+3){F7SetLink(id-kF7LinkBase);return true;}
    if(id==kF7Taper){F7SetLink(3);return true;}
    if(id==kF7TagPrev||id==kF7TagNext){trafficdata::Route scratch;const auto* r=Shown(scratch);if(!r)return true;const int kind=roadplan::PrimitiveAt(r->road,F7Tile(*r)).kind;F7SetPrimitive((kind-1+(id==kF7TagNext?1:4))%5+1);return true;}
    if(id==kF7FieldShape){trafficdata::Route scratch;const auto* r=Shown(scratch);if(!r)return true;F7SetPrimitive(roadplan::PrimitiveAt(r->road,F7Tile(*r)).kind%5+1);return true;}
    if(id==kF7FieldWidth||id==kF7FieldParam){ContextBeginNumber(id==kF7FieldWidth?15:16);return true;}
    if(id==kF7FieldMedian){ContextBeginNumber(17);return true;}
    if(id==kF7FieldPrev||id==kF7FieldNext){g_f7FieldPage=std::max(0,g_f7FieldPage+(id==kF7FieldNext?1:-1));return true;}
    if(id==kF7FlowAlong||id==kF7FlowAgainst||id==kF7TagToggle){
        auto staged=ContextBases();
        for(auto& [name,r]:staged){if(r.ring||r.points.size()<2)return true;const int lanes=RoadLanes(r);if(g_roadLane>=lanes)continue;
            if(int(r.road.directions.size())!=lanes){std::vector<int> d;for(int k=0;k<lanes;++k)d.push_back(roadplan::Direction(r.road,k,lanes));r.road.directions=d;}
            auto& d=r.road.directions[std::size_t(g_roadLane)];d=id==kF7FlowAlong?1:id==kF7FlowAgainst?-1:-d;}
        logger::Write("OBS system=f7.flow lane=%d action=%s",g_roadLane+1,id==kF7FlowAlong?"along":id==kF7FlowAgainst?"against":"toggle");
        ContextCommit(staged);return true;
    }
    if(id==kF7PosDown||id==kF7PosUp||id==kF7TagMinus||id==kF7TagPlus){Press(id==kF7PosUp||id==kF7TagPlus?kZUp:kZDown,c);return true;}
    if(id>=kF7TravelBase&&id<kF7TravelBase+3){
        if(ContextLane()){Status("Lane travel: use Travel to step through it.");return ContextAction(kContextTravel,c);}
        auto staged=ContextBases();for(auto& [name,r]:staged)r.travel=routepolicy::Travel(id-kF7TravelBase);
        logger::Write("OBS system=f7.rules travel=%s",routepolicy::Name(routepolicy::Travel(id-kF7TravelBase)));ContextCommit(staged);return true;
    }
    if(id==kF7TagTravel)return ContextAction(kContextTravel,c);
    // A dock value opens its exact field: Field rows in the Field panel, the rest in the pinned Traffic Manager group.
    if(id>=kF7ValueBase&&id<kF7ValueBase+4){
        const int slot=id-kF7ValueBase;
        if(g_f7Group==f7layers::Field||g_f7Group==f7layers::Flow){
            static constexpr int fieldRows[]{9502,kF7FieldWidth,9508,kF7FieldParam},flowRows[]{9508,9511,0,9510};
            g_roadPanel=true;g_context.pinned=false;g_f7FieldPage=0;g_f7Highlight=g_f7Group==f7layers::Field?fieldRows[slot]:flowRows[slot];
        }else{const int bit=g_f7Group==f7layers::Position?1:2;g_roadPanel=false;g_context.pinned=true;g_ed.groups|=1<<bit;g_ed.lastGroup=bit;g_f7Highlight=-1;
            if(g_f7Group==f7layers::Position&&slot==3){ContextBeginNumber(9);}}
        logger::Write("OBS system=f7.value group=%s slot=%d panel=%s",f7layers::GroupName(g_f7Group),slot,g_roadPanel?"field":"traffic_manager");
        return true;
    }
    if(id==9510)g_f7Divider=true;
    if(id==9510||id==9511){trafficdata::Route scratch;const auto* route=Shown(scratch);if(!route)return true;const int lanes=RoadLanes(*route);
        if(id==9510)g_roadDivider=(g_roadDivider+1)%std::max(1,lanes-1);else g_roadLane=(g_roadLane+1)%lanes;return true;}
    if(id==9512){
        auto staged=ContextBases();
        for(auto& [name,r]:staged){if(r.ring||r.points.size()<2)return true;const int lanes=RoadLanes(r);if(g_roadLane>=lanes)continue;
            if(int(r.road.directions.size())!=lanes){std::vector<int> d;for(int k=0;k<lanes;++k)d.push_back(roadplan::Direction(r.road,k,lanes));r.road.directions=d;}
            r.road.directions[g_roadLane]=-r.road.directions[g_roadLane];}
        ContextCommit(staged);return true;
    }
    if(id>=9502&&id<=9505){ContextBeginNumber(id-9492);return true;}
    if(id==9506||id==9507){
        auto staged=ContextBases();
        for(auto& [name,r]:staged){
            if(r.ring||r.points.size()<2)return true;
            EnableRoad(r);
            {
                const auto index=std::min(std::size_t(std::max(0,g_ed.point-1)),r.points.size()-2);
                if(g_roadAll)for(auto& tile:r.road.tiles)tile.connector=id==9506?1:2;else r.road.tiles[index].connector=id==9506?1:2;
            }
        }
        ContextCommit(staged);return true;
    }
    if(id==kContextCorner){ContextBeginNumber(9);return true;}if(id==kContextSharp||id==kContextRound){if(id==kContextRound&&g_ed.draft&&g_ed.draft->Value().spacePlan.enabled)Press(kKitOpen,c);else ContextCornerEdit(id==kContextSharp?0:15);return true;}
    if(id==9016||id==9017){g_contextMenuPage=std::max(0,g_contextMenuPage+(id==9017?1:-1));return true;}
    if(id==kContextBrowse){g_context.Browse();g_ed.sheet=0;g_contextDirectory=false;return true;}
    if(id==kContextPin){g_context.pinned=!g_context.pinned;return true;}
    if(id==kContextBack||((id==kLaneBack||id==kSheetBack)&&g_context.surface!=editorcontext::Surface::Browse)){g_context.surface=editorcontext::Surface::Edit;g_context.popup=editorcontext::Popup::None;g_ed.sheet=0;g_contextDirectory=false;return true;}
    if(id==kContextMore){g_contextCategory=-1;g_context.Advanced();g_contextDirectory=true;g_ed.sheet=0;return true;}
    if(id==kContextWidth||id==kContextHeight){ContextBeginNumber(id==kContextWidth?7:8);return true;}
    if(id==kContextExact){g_context.Open(editorcontext::Popup::Transform);g_ed.sheet=0;return true;}
    if(id==kContextScope){trafficdata::Route scratch;const auto* route=Shown(scratch);if(route&&!route->ring){g_roadPanel=!g_roadPanel;return true;}g_context.sections=!g_context.sections;g_ed.laneScope=g_context.sections?1:0;g_ed.lanePicks.clear();Status(g_context.sections?"Section scope: click a lane stretch; Ctrl adds targets.":"Whole lane scope: click a lane; Ctrl adds targets.");return true;}
    if(id==kContextTravel){if(ContextLane())LaneAction(kLaneEditBase+2*laneplan::Travel+1);else Press(kTravelNext,c);return true;}
    if(id==kContextVehicles||id==kContextMovement){g_context.Open(id==kContextVehicles?editorcontext::Popup::Vehicles:editorcontext::Popup::Movement);g_contextDirectory=false;if(id==kContextVehicles){g_ed.ruleScope=0;g_ed.ruleLane=ContextLane()?g_ed.laneFocus:-1;g_ed.sheet=1;g_types.clear();}else g_ed.sheet=6;return true;}
    if(id>=kContextNumber&&id<kContextNumber+10){ContextBeginNumber(id-kContextNumber);return true;}
    if(id>=9300&&id<9300+int(g_contextLinks.size())){const auto action=g_contextLinks[id-9300].first;g_contextDirectory=false;g_context.Advanced();if(action==kLaneVehicleRules&&!ContextLane()){g_ed.ruleScope=0;g_ed.ruleLane=-1;g_ed.sheet=1;}else if(action==kSheetBack){g_ed.sheet=0;g_ed.groups=0;}else Press(action,c);return true;}
    if(id>=kContextCategories&&id<kContextCategories+10){g_contextCategory=id-kContextCategories;g_contextDirectory=true;g_context.Advanced();g_ed.sheet=0;return true;}
    return false;
}
void ContextNumberInput(bool focused){
    if(g_contextNumber<0)return;inputgate::HoldMapAll();auto append=[&](char ch){if(g_contextReplace){g_contextText.clear();g_contextReplace=false;}if(g_contextText.size()<32)g_contextText+=ch;};
    for(int vk='0';vk<='9';++vk)if(g_keys.Pressed(vk,focused))append(char(vk));for(int vk=VK_NUMPAD0;vk<=VK_NUMPAD9;++vk)if(g_keys.Pressed(vk,focused))append(char('0'+vk-VK_NUMPAD0));
    if(g_keys.Pressed(VK_OEM_PERIOD,focused)||g_keys.Pressed(VK_DECIMAL,focused))append('.');if(g_keys.Pressed(VK_OEM_MINUS,focused)||g_keys.Pressed(VK_SUBTRACT,focused))append('-');if(g_keys.Pressed(VK_BACK,focused)){if(g_contextReplace){g_contextText.clear();g_contextReplace=false;}else if(!g_contextText.empty())g_contextText.pop_back();}
    if(g_keys.Pressed(VK_ESCAPE,focused)){g_contextNumber=-1;Status("Exact entry cancelled.");return;}
    for(int vk='A';vk<='Z';++vk)g_keys.Pressed(vk,focused);
    if(g_keys.Pressed(VK_RETURN,focused)){double value;const int field=g_contextNumber;if(g_contextNumberKey!=ContextKey()){g_contextNumber=-1;Status("Selection changed; exact entry cancelled.");return;}if(!editorcontext::Number(g_contextText,value)){Status("Enter a finite number. Enter commits; Esc cancels.");return;}if(ContextField(field,value))g_contextNumber=-1;else Status("Value is outside the valid range; draft kept unchanged.");}
}
bool ContextStartDrag(int id,const routemap::Camera& c,double x,double y){
    if((id<kContextMove||id>kContextZ)&&id!=kContextCornerDrag)return false;if(ContextLane()&&!AddonAuthoring(16)){Status("Lane handles require the updated traffic addon.");return true;}g_contextPlace=ContextPlaceActive();if(g_contextPlace){trafficdata::Route empty;g_contextPlaceBase=g_ed.file;g_contextDragCentre=ContextCentre(empty);g_contextDrag=id-kContextMove+1;g_contextDragX=x;g_contextDragY=y;return true;}
    g_contextDragBase=ContextBases();if(g_contextDragBase.empty())return true;const auto& r=g_ed.draft->Value();g_contextDragCentre=ContextCentre(r);g_contextDrag=id==kContextCornerDrag?4:id-kContextMove+1;g_contextDragX=x;g_contextDragY=y;g_contextPreview.clear();return true;
}
void ContextDragTick(const routemap::Camera& c,double x,double y,bool held,bool release){
    if(!g_contextDrag)return;lanetransform::Vec delta{};auto matrix=lanetransform::Identity();
    if(g_contextDrag==1){auto camera=c;camera.mouseX=(x-g_ui.bx)/g_ui.ax*c.width;camera.mouseY=(y-g_ui.by)/g_ui.ay*c.height;routemap::Vec3 at,origin;auto initial=c;initial.mouseX=(g_contextDragX-g_ui.bx)/g_ui.ax*c.width;initial.mouseY=(g_contextDragY-g_ui.by)/g_ui.ay*c.height;if(routemap::MouseAt(camera,g_contextDragCentre.z,at)&&routemap::MouseAt(initial,g_contextDragCentre.z,origin))delta={at.x-origin.x,at.y-origin.y,0};}
    if(g_contextDrag==2)matrix=lanetransform::Rotation(2,(x-g_contextDragX)*.01);
    if(g_contextDrag==3){double ax,ay,bx,by;const auto p=g_contextDragCentre;double ppm=Local(c,{p.x,p.y,p.z},ax,ay)&&Local(c,{p.x,p.y,p.z+1},bx,by)?std::max(.3,std::fabs(by-ay)):1;delta.z=(g_contextDragY-y)/ppm;}
    if(g_contextPlace){if(held||release)ContextPlaceEdit(g_contextPlaceBase,delta,g_contextDrag==2?(x-g_contextDragX)*.01:0,false);if(release){ContextPlaceEdit(g_contextPlaceBase,delta,g_contextDrag==2?(x-g_contextDragX)*.01:0,true);g_contextDrag=0;g_contextPlace=false;}return;}
    if(g_contextDrag==4){g_contextPreview=g_contextDragBase;for(auto& [name,r]:g_contextPreview){if(r.spacePlan.enabled)continue;r.rounding.resize(r.points.size());const double before=g_contextDragBase.at(name).rounding.size()>std::size_t(g_ed.point)?g_contextDragBase.at(name).rounding[g_ed.point]:0;r.rounding[g_ed.point]=std::clamp(before+(x-g_contextDragX)*.25,0.0,200.0);}}
    else if(held||release)g_contextPreview=ContextTransform(g_contextDragBase,delta,matrix);
    if(release){if(std::hypot(x-g_contextDragX,y-g_contextDragY)>2)ContextCommit(g_contextPreview);g_contextDrag=0;g_contextPreview.clear();g_contextDragBase.clear();}
}
#include "contextlayout.inl"
