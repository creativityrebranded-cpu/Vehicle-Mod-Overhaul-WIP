// Included inside routeedit's existing editor namespace, after action IDs.
bool LaneAction(int id) {
    if(((id>=kRestrictionSheet&&id<=kRestrictionZoneTop)||(id>=kWorldDefaultsFromRoute&&id<=kLaneUseDefaults))&&!AddonAuthoring(1)){Status("This authoring feature needs the matching traffic addon. Existing records are retained; older route controls remain available.");return true;}
    if(id>=kPlaceAccess&&id<=kAccessDone&&!AddonAuthoring(2)){Status("Parking-path authoring is unavailable in this addon. Existing access records are retained.");return true;}
    if(id>=kJourneyActions&&id<=kJourneyRelease&&!AddonAuthoring(4)){Status("Advanced journey actions are unavailable in this addon. Existing records are retained.");return true;}

    if(id>=kNetworkPreview&&id<=kPreviewLivePaths){
        if(id==kNetworkPreview){g_ed.sheet=18;g_ed.menu=0;return true;}
        if(id==kPreviewDestination){if(!g_ed.file.landings.empty())g_ed.previewDestination=(g_ed.previewDestination+1)%int(g_ed.file.landings.size());g_ed.itineraryPreview.clear();return true;}
        if(id==kPreviewSignalNext){g_ed.signalPreviewSeconds=std::fmod(g_ed.signalPreviewSeconds+1,3600.0);return true;}
        if(id==kPreviewLivePaths){g_ed.showLivePaths=!g_ed.showLivePaths;return true;}
        trafficdata::Route scratch;const auto* source=Shown(scratch);if(!source||g_ed.file.landings.empty())return true;if(!source->ring&&source->points.empty()){g_ed.itineraryExplanation="Place a source route point first.";return true;}roadnetwork::Network network;std::vector<trafficdata::Route> routes;
        if(g_ed.draft)routes.push_back(g_ed.draft->Value());for(const auto& route:g_ed.file.routes)if(Usable(route)&&(!g_ed.draft||route.id!=g_ed.draft->Value().name))routes.push_back(ToTd(route,EditorKey()));for(const auto& other:g_ed.others)routes.push_back(other.route);
        std::vector<trafficdata::Link> links=g_ed.othersData.links;for(const auto& link:g_ed.file.links){trafficdata::Link out;out.name=link.id;out.from=link.from;out.to=link.to;out.at=link.at;out.toAt=link.toAt;out.toEnd=link.toEnd;out.radius=link.radius;out.share=link.share;out.type=link.type;out.fromLane=link.fromLane;out.toLane=link.toLane;links.push_back(out);}network.Build(std::move(routes),links);
        const auto& bay=g_ed.file.landings[std::min(std::size_t(std::max(0,g_ed.previewDestination)),g_ed.file.landings.size()-1)];const auto from=source->ring?trafficdata::Vec{source->shape.cx,source->shape.cy,source->shape.z}:source->points[std::min(std::size_t(std::max(0,g_ed.point)),source->points.size()-1)];trafficdata::Vec to=bay.at;if(!bay.access.entrance.empty()){const auto& p=bay.access.entrance.front();to={p.x,p.y,p.z};}const auto journey=network.Plan(from,to,600,{},bay.access.entryRoute,bay.access.entryLane);g_ed.itineraryPreview=networkpreview::Trace(network,journey,links);g_ed.itineraryExplanation=g_ed.itineraryPreview.empty()?"No connected itinerary to this place.":std::to_string(journey.routes.size())+" connected route(s); geometry preview only.";return true;
    }

    if(id>=kWorldDefaultsFromRoute&&id<=kLaneUseDefaults){
        if(id==kLaneUseDefaults){if(!EnsureDraft())return true;auto route=g_ed.draft->Value();const auto models=route.lanePlan.vehicles;const auto transforms=route.lanePlan.transforms;route.lanePlan={};route.lanePlan.vehicles=models;route.lanePlan.transforms=transforms;auto city=g_ed.file.city;city.world=route.world;route.lanePlan=trafficdata::LaneDefaults({city},g_ed.routeGroups,route);PushSnap();g_ed.draft->Edit(route);g_ed.massUndo.back().draftChanged=true;g_ed.massUndo.back().depth=DraftDepth();Status("Route now follows parent lane defaults. Save keeps it.");return true;}
        trafficdata::Route scratch;const auto* selected=Shown(scratch);laneplan::Plan plan;if(id==kWorldDefaultsFromRoute||id==kGroupDefaultsFromRoute){if(!selected||!selected->lanePlan.enabled)return true;plan.enabled=true;plan.base=laneplan::Resolve(selected->lanePlan,-1,g_ed.laneFocus);}
        PushSnap();if(id==kWorldDefaultsFromRoute||id==kWorldDefaultsClear){g_ed.file.city.laneDefaults=plan;g_ed.file.cityWritten=true;}else{const auto name=g_ed.ruleScope==2?std::string(trafficdata::GlobalGroup()):g_ed.ruleGroup;auto group=std::find_if(g_ed.routeGroups.begin(),g_ed.routeGroups.end(),[&](const auto& value){return value.name==name;});if(group==g_ed.routeGroups.end()){g_ed.routeGroups.push_back({name,name,{}});group=std::prev(g_ed.routeGroups.end());}group->laneDefaults=plan;}
        Status("Parent lane defaults changed. Explicit route setups keep their overrides; Save applies new assignments.");return true;
    }

    if(id==kLanePlacement){g_ed.sheet=16;return true;}
    if(id==kLaneMoveMap){g_ed.mode=Mode::LanePlace;Status("Click where the selected lane centre should sit sideways. Height offset moves it vertically.");return true;}
    if(id>=kLaneEnvelope&&id<=kEnvelopeHeightUp){if(id==kLaneEnvelope){if(g_ed.sheet==17)g_ed.envelopePreview=!g_ed.envelopePreview;else{g_ed.sheet=17;g_ed.envelopePreview=true;}}if(id==kEnvelopeWidthDown)g_ed.previewShape.halfWidth=std::max(.25,g_ed.previewShape.halfWidth-.25);if(id==kEnvelopeWidthUp)g_ed.previewShape.halfWidth=std::min(20.0,g_ed.previewShape.halfWidth+.25);if(id==kEnvelopeLengthDown)g_ed.previewShape.halfLength=std::max(.5,g_ed.previewShape.halfLength-.25);if(id==kEnvelopeLengthUp)g_ed.previewShape.halfLength=std::min(40.0,g_ed.previewShape.halfLength+.25);if(id==kEnvelopeHeightDown)g_ed.previewShape.height=std::max(.5,g_ed.previewShape.height-.25);if(id==kEnvelopeHeightUp)g_ed.previewShape.height=std::min(40.0,g_ed.previewShape.height+.25);return true;}

    if(id>=kPlaceAccess&&id<=kAccessDone){
        if(!AreaOn())return true;auto& bay=g_ed.file.landings[std::size_t(g_ed.spot)];auto& access=bay.access;
        if(id==kPlaceAccess){g_ed.sheet=15;g_ed.accessHeight=bay.at.z;return true;}
        if(id==kAccessDone){g_ed.sheet=0;g_ed.mode=Mode::Select;return true;}
        if(!access.unsupported.empty())return true;
        if(id==kAccessEntry||id==kAccessExit){g_ed.mode=id==kAccessEntry?Mode::AccessEntry:Mode::AccessExit;const auto& path=id==kAccessEntry?access.entrance:access.departure;g_ed.accessHeight=path.empty()?bay.at.z:path.back().z;Status("Click the map to add parking path points; use New point Z for height. Back finishes drawing.");return true;}
        if(id==kAccessHeightDown||id==kAccessHeightUp){g_ed.accessHeight+=id==kAccessHeightUp?.5:-.5;return true;}
        PushSnap();
        if(id==kAccessUndoEntry&&!access.entrance.empty())access.entrance.pop_back();if(id==kAccessUndoExit&&!access.departure.empty())access.departure.pop_back();
        if(id==kAccessEntryRoute||id==kAccessExitRoute){std::vector<std::string> routes{std::string()};for(const auto& route:g_ed.file.routes)if(Usable(route))routes.push_back(route.id);for(const auto& other:g_ed.others)routes.push_back(other.name);auto& name=id==kAccessEntryRoute?access.entryRoute:access.exitRoute;const auto found=std::find(routes.begin(),routes.end(),name);name=found==routes.end()||std::next(found)==routes.end()?routes.front():*std::next(found);(id==kAccessEntryRoute?access.entryLane:access.exitLane)=-1;}
        if(id==kAccessEntryLane||id==kAccessExitLane){const bool entry=id==kAccessEntryLane;trafficroute::Line line;bool ring=false;if(LineOf(entry?access.entryRoute:access.exitRoute,line,ring)){auto lanes=trafficroute::Lanes(line,LanesOf(entry?access.entryRoute:access.exitRoute));lanes.insert(lanes.begin(),-1);auto& lane=entry?access.entryLane:access.exitLane;const auto found=std::find(lanes.begin(),lanes.end(),lane);lane=found==lanes.end()||std::next(found)==lanes.end()?lanes.front():*std::next(found);}}
        if(id==kAccessWidthDown)access.width=std::max(1.0,access.width-.5);if(id==kAccessWidthUp)access.width=std::min(100.0,access.width+.5);
        if(id==kAccessClearanceDown)access.height=std::max(1.0,access.height-.5);if(id==kAccessClearanceUp)access.height=std::min(200.0,access.height+.5);
        Status("Parking access changed. Save keeps it.");return true;
    }

    if(id==kPopulationSheet){g_ed.sheet=14;g_ed.menu=0;g_ed.populationIndex=0;return true;}
    if(id==kPopulationNext){const auto addon=GetModuleHandleW(L"FVO_Traffic.dll");const auto query=addon?reinterpret_cast<FvoPopulationQueryV1>(GetProcAddress(addon,"FVO_Traffic_PopulationV1")):nullptr;FvoPopulationInspectV1 row;++g_ed.populationIndex;if(!query||!query(g_ed.populationIndex,&row))g_ed.populationIndex=0;return true;}

    if(id>=kRestrictionSheet&&id<=kRestrictionZoneTop){
        if(id==kRestrictionBack){g_ed.sheet=g_ed.restrictionScope==1?3:g_ed.restrictionScope==2?1:0;return true;}
        if(id==kRestrictionSheet||id==kRestrictionCity||id==kRestrictionGroup||id==kRestrictionZone){g_ed.restrictionScope=id==kRestrictionCity?1:id==kRestrictionGroup?2:id==kRestrictionZone?3:0;g_ed.sheet=13;g_ed.menu=0;return true;}
        trafficdata::Route route;trafficconstraints::Rules* rules=nullptr;
        if(g_ed.restrictionScope==1)rules=&g_ed.file.city.restrictions;
        else if(g_ed.restrictionScope==2){const auto name=g_ed.ruleScope==2?std::string(trafficdata::GlobalGroup()):g_ed.ruleGroup;auto found=std::find_if(g_ed.routeGroups.begin(),g_ed.routeGroups.end(),[&](const auto& group){return group.name==name;});if(found==g_ed.routeGroups.end()&&!name.empty()){g_ed.routeGroups.push_back({name,name,{}});found=std::prev(g_ed.routeGroups.end());}if(found!=g_ed.routeGroups.end())rules=&found->restrictions;}
        else if(g_ed.restrictionScope==3&&ZoneOn())rules=&g_ed.file.zones[std::size_t(g_ed.zone)].restrictions;
        else{if(!EnsureDraft())return true;route=g_ed.draft->Value();rules=&route.restrictions;}
        if(!rules||!rules->unsupported.empty())return true;auto next=*rules;
        if(g_ed.restrictionScope==3&&ZoneOn()&&(id==kRestrictionZoneFloor||id==kRestrictionZoneTop)){PushSnap();auto& zone=g_ed.file.zones[std::size_t(g_ed.zone)];if(id==kRestrictionZoneFloor){zone.restrictionFloor=zone.restrictionFloor<=-1000000000?zone.at.z:zone.restrictionFloor+5;if(zone.restrictionFloor>zone.at.z+200)zone.restrictionFloor=-1000000000;if(zone.restrictionFloor>zone.restrictionTop)zone.restrictionTop=zone.restrictionFloor;}else{zone.restrictionTop=zone.restrictionTop>=1000000000?zone.at.z+10:zone.restrictionTop+5;if(zone.restrictionTop>zone.at.z+200)zone.restrictionTop=1000000000;}Status("Zone restriction height changed. Save keeps it.");return true;}

        if(id==kRestrictionModes)next.modes=next.modes==3?1:next.modes==1?2:3;
        if(id==kRestrictionClosure)next.closure=(next.closure+1)%3;
        if(id==kRestrictionSpeedDown)next.speed=std::max(0.0,next.speed-1);
        if(id==kRestrictionSpeedUp)next.speed=std::min(200.0,next.speed+1);
        if(id==kRestrictionFloor)next.minHeight=next.minHeight+1>next.maxHeight?0:next.minHeight+1;
        if(id==kRestrictionCeiling)next.maxHeight=next.maxHeight-5<next.minHeight?200:next.maxHeight-5;
        if(id==kRestrictionWindow){next.start=next.start<0?0:next.start>=1380?-1:next.start+60;if(next.start<0)next.end=-1;else if(next.end<0)next.end=0;}
        if(id==kRestrictionEnd)next.end=(std::max(0,next.end)+60)%1440;
        if(id==kRestrictionNoChange)next.noLaneChange=!next.noLaneChange;
        if(!trafficconstraints::Valid(next))return true;PushSnap();*rules=next;
        if(g_ed.restrictionScope==0){g_ed.draft->Edit(route);g_ed.massUndo.back().draftChanged=true;g_ed.massUndo.back().depth=DraftDepth();}
        else if(g_ed.restrictionScope==1)g_ed.file.cityWritten=true;
        Status("Enforced rules changed. Save applies them without resetting traffic.");return true;
    }

    if(id>=kJourneySheet&&id<=kJourneyRelease){
        if(id==kJourneyActions){g_ed.sheet=12;return true;}
        if(id==kJourneySheet){g_ed.sheet=11;g_ed.journeyIndex=0;g_ed.menu=0;return true;}
        if(!EnsureDraft())return true;auto route=g_ed.draft->Value();auto& plan=route.journey;
        if(!plan.unsupported.empty()){Status("Unsupported journey record preserved.");return true;}
        auto index=plan.stops.empty()?0:std::min(std::size_t(std::max(0,g_ed.journeyIndex)),plan.stops.size()-1);
        if(id==kJourneyNext){g_ed.journeyIndex=plan.stops.empty()?0:int((index+1)%plan.stops.size());return true;}
        if(id==kJourneyRelease){
            if(plan.stops.empty())return true;const auto addon=GetModuleHandleW(L"FVO_Traffic.dll");using Signal=int (*)(const char*,unsigned);const auto signal=addon?reinterpret_cast<Signal>(GetProcAddress(addon,"FVO_Traffic_SignalJourneyV1")):nullptr;
            const int count=signal?signal(route.name.c_str(),plan.stops[index].trigger):0;Status(std::to_string(count)+" active journey trigger(s) released; draft unchanged.");return true;
        }
        if(id==kJourneyAdd){if(route.ring||g_ed.point<0||g_ed.point>=int(route.points.size())||plan.stops.size()>=128)return true;const auto p=route.points[g_ed.point];plan.stops.push_back({p.x,p.y,p.z,5});g_ed.journeyIndex=int(plan.stops.size()-1);}
        else if(id==kJourneyEnable){if(plan.stops.empty())return true;plan.enabled=!plan.enabled;}
        else if(id==kJourneyLoop)plan.loop=!plan.loop;
        else if(!plan.stops.empty()){
            auto& stop=plan.stops[index];
            if(id==kJourneyWait)stop.wait=journeyplan::Wait((int(stop.wait)+1)%4);
            if(id==kJourneyTrigger)stop.trigger=stop.trigger%16+1;
            if(id==kJourneyTimeout)stop.timeout=stop.timeout>=600?0:stop.timeout+30;
            if(id==kJourneyFallback)stop.fallback=stop.fallback+1>=int(plan.stops.size())?-2:stop.fallback+1;
            if(id==kJourneySpeedUp)stop.speed=std::min(200.0,stop.speed+1);
            if(id==kJourneySpeedDown)stop.speed=std::max(0.0,stop.speed-1);
            if(id==kJourneyMode)stop.mode=(stop.mode+1)%4;
            if(id==kJourneyRemove){journeyplan::Remove(plan,index);g_ed.journeyIndex=0;}
            if(id==kJourneyDwellUp)plan.stops[index].dwell=std::min(86400.0,plan.stops[index].dwell+5);
            if(id==kJourneyDwellDown)plan.stops[index].dwell=std::max(0.0,plan.stops[index].dwell-5);
            if(id==kJourneyEarlier&&index>0){journeyplan::Swap(plan,index,index-1);g_ed.journeyIndex=int(index-1);}
            if(id==kJourneyLater&&index+1<plan.stops.size()){journeyplan::Swap(plan,index,index+1);g_ed.journeyIndex=int(index+1);}
        }
        PushSnap();g_ed.draft->Edit(route);g_ed.massUndo.back().draftChanged=true;g_ed.massUndo.back().depth=DraftDepth();Status("Journey sequence changed. Save keeps it.");return true;
    }
    if(id==kInspectMovement){g_ed.sheet=10;g_ed.inspectIndex=0;g_ed.menu=0;return true;}
    if(id==kInspectNext){
        trafficdata::Route scratch;const auto* r=Shown(scratch);const auto addon=GetModuleHandleW(L"FVO_Traffic.dll");
        const auto query=addon?reinterpret_cast<FvoTrafficInspectQueryV1>(GetProcAddress(addon,"FVO_Traffic_InspectV1")):nullptr;
        FvoTrafficInspectV1 observed;++g_ed.inspectIndex;if(!r||!query||!query(r->name.c_str(),g_ed.inspectIndex,&observed))g_ed.inspectIndex=0;return true;
    }
    if(id>=kLinkLaneFromDown&&id<=kLinkLaneToUp) {
        if(g_ed.link<0||g_ed.link>=int(g_ed.file.links.size()))return true;
        auto& link=g_ed.file.links[g_ed.link];const bool from=id<=kLinkLaneFromUp;
        trafficroute::Line line;bool ring=false;if(!LineOf(from?link.from:link.to,line,ring))return true;
        auto available=trafficroute::Lanes(line,LanesOf(from?link.from:link.to));available.insert(available.begin(),-1);
        const int current=from?link.fromLane:link.toLane;auto found=std::find(available.begin(),available.end(),current);
        const int index=found==available.end()?0:int(found-available.begin());const int step=id==kLinkLaneFromUp||id==kLinkLaneToUp?1:-1;
        PushSnap();(from?link.fromLane:link.toLane)=available[(index+step+int(available.size()))%available.size()];
        Status("Junction lane connection changed (Save to keep it)");return true;
    }
    if(id==kLaneVolumes){g_ed.view.volumes=!g_ed.view.volumes;return true;}
    if(id==kLaneScope){g_ed.laneScope^=1;return true;}
    if(id==kLaneFocus) {
        trafficdata::Route scratch;const auto* r=Shown(scratch);if(!r)return true;
        const int section=g_ed.laneScope&&!r->ring&&g_ed.point>=0?int(routepolicy::Segment(r->points.size(),r->closed,g_ed.point,0)):-1;
        auto lanes=laneplan::Lanes(laneplan::Resolve(r->lanePlan,section));auto found=std::find(lanes.begin(),lanes.end(),g_ed.laneFocus);
        g_ed.laneFocus=lanes.empty()?-1:found==lanes.end()?lanes.front():std::next(found)==lanes.end()?-1:*std::next(found);
        for(auto& p:g_ed.lanePicks)p.lane=g_ed.laneFocus;return true;
    }
    if(id==kLaneBack){if(g_ed.sheet>=19)g_ed.mode=Mode::Select;g_ed.sheet=g_ed.sheet==9?8:g_ed.sheet==8?6:(g_ed.sheet==5||g_ed.sheet>=10)?0:5;return true;}
    if(id==kLaneRules){g_ed.sheet=6;return true;}
    if(id==kLaneDimensions){g_ed.sheet=7;return true;}
    if(id==kLaneTravelRules){g_ed.sheet=8;return true;}
    if(id==kLaneHeightRules){g_ed.sheet=9;return true;}
    const bool setting=id>=kLaneEditBase&&id<kLaneEditBase+laneplan::Fields*2;
    if(id!=kLaneDesign&&id!=kLanePreset&&id!=kLaneReset&&id!=kLanePlaceApply&&!setting)return false;
    const auto picks=PicksNow();
    if(!EnsureDraft())return true;
    const auto primary=g_ed.draft->Value().name;
    std::set<std::string> targets{primary};
    for(const auto& p:picks)if(p.kind==kPickRoute||p.kind==kPickPoint)targets.insert(p.id);
    if(!g_ed.lanePicks.empty()){targets.clear();for(const auto& p:g_ed.lanePicks)targets.insert(p.route);}
    bool draftChanged=false;int edited=0;std::string invalidReason;
    auto edit=[&](trafficdata::Route& r) {
        if(!r.lanePlan.unsupported.empty())return false;
        auto& plan=r.lanePlan;plan.inherited=false;
        if(!plan.enabled) {
            plan.enabled=true;plan.base.forward=1;plan.base.reverse=r.lanes>1?1:0;
            plan.base.profile.width=r.corridor;plan.base.gap=std::max(0.0,r.width-r.corridor);
            plan.base.travel=r.travel;
        } else if(id==kLaneDesign)return false;
        laneplan::EnsureSections(plan,r.ring?0:r.points.size());
        if(id==kLaneDesign)return true;
        std::vector<std::pair<int,int>> sections;
        if(g_ed.laneScope&&!g_ed.lanePicks.empty()) {
            for(const auto& p:g_ed.lanePicks)if(p.route==r.name){int section=p.section;if(p.identity){const auto found=std::find(plan.sectionIds.begin(),plan.sectionIds.end(),p.identity);if(found==plan.sectionIds.end()){invalidReason="A selected section no longer exists; select it again.";continue;}section=int(found-plan.sectionIds.begin());}sections.push_back({section,p.lane});}
            std::sort(sections.begin(),sections.end());sections.erase(std::unique(sections.begin(),sections.end()),sections.end());
            if(sections.empty())return false;
        } else if(g_ed.laneScope&&!r.ring) {
            std::set<int> points;for(const auto& p:picks)if(p.kind==kPickPoint&&p.id==r.name)points.insert(p.index);
            for(std::size_t i=0;i<(r.closed?r.points.size():r.points.size()?r.points.size()-1:0);++i)
                if(points.count(int(i))&&points.count(int((i+1)%r.points.size())))sections.push_back({int(i),g_ed.laneFocus});
            if(points.size()<2&&r.name==primary&&g_ed.point>=0)sections.push_back({int(routepolicy::Segment(r.points.size(),r.closed,g_ed.point,0)),g_ed.laneFocus});
            if(sections.empty())return false;
        } else sections.push_back({-1,g_ed.laneFocus});
        for(auto [section,lane]:sections) {
            auto s=laneplan::Resolve(plan,section,lane);
            if(id==kLanePlaceApply){s.profile.offset+=g_ed.laneMoveSide;s.profile.elevation+=g_ed.laneMoveHeight;if(laneplan::Valid(s)){laneplan::Set(plan,section,lane,laneplan::Offset,s);laneplan::Set(plan,section,lane,laneplan::Elevation,s);}else invalidReason="Selected lane placement is invalid.";continue;}
            if(id==kLaneReset) {
                std::erase_if(plan.overrides,[&](const laneplan::Override& rule){return (section<0||rule.section==section)&&(lane<0||rule.lane==lane);});continue;
            }
            auto set=[&](laneplan::Field f){laneplan::Set(plan,section,lane,f,s);};
            if(id==kLanePreset) {
                s.travel=s.travel==routepolicy::Travel::Drive?routepolicy::Travel::Hover:s.travel==routepolicy::Travel::Hover?routepolicy::Travel::Both:routepolicy::Travel::Drive;
                s.movement=laneplan::Movement::Locked;s.obstacles=s.moving=s.people=laneplan::Obstacles::Stop;s.recovery=false;
                set(laneplan::Travel);set(laneplan::Authority);set(laneplan::Static);set(laneplan::Moving);set(laneplan::People);set(laneplan::Recovery);continue;
            }
            const auto field=laneplan::Field((id-kLaneEditBase)/2);const int step=(id-kLaneEditBase)%2?1:-1;
            switch(field) {
            case laneplan::Forward:if(lane<0)s.forward=std::clamp(s.forward+step,0,laneplan::kMaxEachWay);break;
            case laneplan::Reverse:if(lane<0)s.reverse=std::clamp(s.reverse+step,0,laneplan::kMaxEachWay);break;
            case laneplan::Width:s.profile.width=std::clamp(s.profile.width+step*.5,1.0,100.0);break;
            case laneplan::Height:s.profile.height=std::clamp(s.profile.height+step*.5,1.0,200.0);break;
            case laneplan::Gap:s.gap=std::clamp(s.gap+step*.5,0.0,100.0);break;
            case laneplan::Shape:s.profile.shape=lanevolume::Shape((int(s.profile.shape)+step+4)%4);break;
            case laneplan::Radius:s.profile.radius=std::clamp(s.profile.radius+step*.25,0.0,std::min(s.profile.width,s.profile.height)*.5);break;
            case laneplan::TopWidth:s.profile.topWidth=std::clamp(s.profile.topWidth+step*.5,1.0,100.0);break;
            case laneplan::Offset:s.profile.offset+=step*.5;break;
            case laneplan::Elevation:s.profile.elevation+=step*.5;break;
            case laneplan::Bank:s.profile.bank=std::clamp(s.profile.bank+step*lanevolume::kPi/180,-lanevolume::kPi,lanevolume::kPi);break;
            case laneplan::Taper:s.taper=std::clamp(s.taper+step*5.0,0.0,2000.0);break;
            case laneplan::Travel:s.travel=routepolicy::Travel((int(s.travel)+1+step+4)%4-1);break;
            case laneplan::Authority:s.movement=laneplan::Movement((int(s.movement)+step+6)%6);break;
            case laneplan::Static:s.obstacles=laneplan::Obstacles((int(s.obstacles)+step+3)%3);break;
            case laneplan::Moving:s.moving=laneplan::Obstacles((int(s.moving)+step+3)%3);break;
            case laneplan::People:s.people=laneplan::Obstacles((int(s.people)+step+3)%3);break;
            case laneplan::Recovery:s.recovery=!s.recovery;break;
            case laneplan::ModeRule:s.rules.mode=(s.rules.mode+step+4)%4;break;
            case laneplan::BypassRule:s.rules.bypass=(s.rules.bypass+step+4)%4;break;
            case laneplan::MinHeight:s.rules.minHeight=std::clamp(s.rules.minHeight+step*.5,0.0,s.rules.maxHeight);s.rules.hoverHeight=std::max(s.rules.hoverHeight,s.rules.minHeight);break;
            case laneplan::MaxHeight:s.rules.maxHeight=std::clamp(s.rules.maxHeight+step, s.rules.minHeight,200.0);s.rules.hoverHeight=std::min(s.rules.hoverHeight,s.rules.maxHeight);break;
            case laneplan::HoverHeight:s.rules.hoverHeight=std::clamp(s.rules.hoverHeight+step*.5,s.rules.minHeight,s.rules.maxHeight);break;
            case laneplan::SupportRange:s.rules.supportRange=std::clamp(s.rules.supportRange+step*.5,.5,200.0);break;
            case laneplan::SettleTime:s.rules.settleSeconds=std::clamp(s.rules.settleSeconds+step*.25,.25,30.0);break;
            default:break;
            }
            if(laneplan::Valid(s)){set(field);if(field==laneplan::MinHeight||field==laneplan::MaxHeight)set(laneplan::HoverHeight);}
            else invalidReason="These dimensions or lane counts are invalid.";
        }
        return true;
    };
    std::vector<trafficdata::Route> staged;
    for(const auto& name:targets) {
        if(name==primary) {auto r=g_ed.draft->Value();if(edit(r))staged.push_back(std::move(r));}
        else if(const int k=RouteIndex(name);k>=0&&Usable(g_ed.file.routes[k])) {
            auto r=ToTd(g_ed.file.routes[k],EditorKey());if(edit(r))staged.push_back(std::move(r));
        }
    }
    for(const auto& r:staged)if(laneplan::Problem(r.lanePlan))invalidReason="This change conflicts with inherited lane dimensions or counts.";
    if(!invalidReason.empty()){Status(invalidReason+" No selected routes were changed.");return true;}
    if(!staged.empty())PushSnap();
    for(const auto& r:staged){if(r.name==primary){g_ed.draft->Edit(r);draftChanged=true;}else g_ed.file.routes[RouteIndex(r.name)]=FromTd(r);++edited;}
    if(draftChanged){g_ed.massUndo.back().draftChanged=true;g_ed.massUndo.back().depth=DraftDepth();}
    if(id==kLaneDesign){g_ed.sheet=5;g_ed.laneFocus=-1;g_ed.mode=Mode::Select;}
    Status(edited?"Lane setup changed. Test previews; Save keeps it.":"No editable target: select stretches or check the lane record.");
    return true;
}
