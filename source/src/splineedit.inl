// F7 authoring only. Uses existing map camera maths and draft/undo/save workflow.
bool KitAction(int id){
    if(id<kKitOpen||id>kKitLast)return false;
    if(id==kKitOpen){g_ed.sheet=19;g_ed.mode=Mode::Select;g_ed.menu=0;return true;}
    if(id==kKitProfiles){g_ed.sheet=20;return true;}if(id==kKitPath){g_ed.sheet=19;return true;}
    if(!AddonAuthoring(8)){Status("Freeform authoring requires the matching core and traffic addon. Existing records are retained.");return true;}
    if(!EnsureDraft())return true;auto route=g_ed.draft->Value();auto& plan=route.spacePlan;
    if(route.ring||route.points.size()<2){Status("Use a point path with at least two anchors. Bake a procedural ring first.");return true;}
    if(!plan.unsupported.empty()){Status("Unsupported toolkit record is preserved; editing disabled.");return true;}
    if(id==kKitEnable){if(plan.handles.empty())splinekit::AutoHandles(plan,splinekit::Points(route.points),route.closed);plan.enabled=!plan.enabled;}
    else if(!plan.enabled){Status("Enable the 3D toolkit first. Legacy rounding is retained for switching back.");return true;}
    else {
        auto lane=std::find_if(plan.lanes.begin(),plan.lanes.end(),[&](const auto& p){return p.lane==g_ed.laneFocus;});
        const auto index=std::min(std::size_t(std::max(0,g_ed.point)),route.points.size()-1);
        if(id==kKitAnchorNext){g_ed.point=int((index+1)%route.points.size());return true;}
        if(id==kKitLaneNext){const auto lanes=trafficroute::Lanes(EditorLine(route),route.lanes);auto it=std::find(lanes.begin(),lanes.end(),g_ed.laneFocus);if(it==lanes.end())g_ed.laneFocus=lanes.empty()?-1:lanes.front();else {++it;g_ed.laneFocus=it==lanes.end()?-1:*it;}return true;}
        if(id==kKitSide){g_ed.kitTarget=(g_ed.kitTarget+1)%3;return true;}
        if(id==kKitStep){g_ed.kitStep=g_ed.kitStep==.1?1:g_ed.kitStep==1?5:g_ed.kitStep==5?25:.1;return true;}
        if(id==kKitPlace){g_ed.mode=Mode::KitPlace;Status("Click the map to place the selected anchor or handle at its current Z. Use Z controls to change the placement plane.");return true;}
        if(id==kKitDetach){if(g_ed.laneFocus<0){Status("Select an individual lane first.");return true;}if(lane==plan.lanes.end()){splinekit::LanePath path;path.lane=g_ed.laneFocus;const auto& line=EditorLine(route);for(std::size_t i=0;i<route.points.size();++i){const auto p=trafficroute::At(line,path.lane,line.ToLine(double(i)));path.anchors.push_back({{p.x,p.y,p.z},plan.handles[i]});}plan.lanes.push_back(std::move(path));}else plan.lanes.erase(lane);}
        else if(id==kKitAuto){if(lane==plan.lanes.end())splinekit::AutoHandles(plan,splinekit::Points(route.points),route.closed);else {splinekit::Plan temp;std::vector<splinekit::Vec> points;for(auto a:lane->anchors)points.push_back(a.point);splinekit::AutoHandles(temp,points,route.closed);for(std::size_t i=0;i<temp.handles.size();++i)lane->anchors[i].handle=temp.handles[i];}}
        else if(id==kKitQuality){plan.tolerance=plan.tolerance>.1?.05:plan.tolerance>.02?.01:.15;}
        else if(id==kKitSpacing){plan.maxStep=plan.maxStep==2?5:plan.maxStep==5?10:plan.maxStep==10?20:2;}
        else if(id>=kKitWholeBase&&id<kKitWholeBase+18){if(lane==plan.lanes.end()){Status("Make this lane independent to transform its whole path.");return true;}const int field=(id-kKitWholeBase)/2;const double sign=(id-kKitWholeBase)%2?1:-1;splinekit::Vec centre{};for(auto a:lane->anchors)centre=splinekit::Add(centre,a.point);centre=splinekit::Mul(centre,1.0/lane->anchors.size());splinekit::TransformLane(*lane,centre,field,sign,g_ed.kitStep);}
        else if(id>=kKitCoordBase&&id<kKitCoordBase+6){const int axis=(id-kKitCoordBase)/2;const double delta=((id-kKitCoordBase)%2?1:-1)*g_ed.kitStep;auto& h=lane==plan.lanes.end()?plan.handles[index]:lane->anchors[index].handle;splinekit::Vec anchor=lane==plan.lanes.end()?splinekit::Vec{route.points[index].x,route.points[index].y,route.points[index].z}:lane->anchors[index].point;auto target=g_ed.kitTarget==0?anchor:g_ed.kitTarget==1?h.incoming:h.outgoing;double* component=axis==0?&target.x:axis==1?&target.y:&target.z;*component+=delta;if(g_ed.kitTarget==0){if(lane==plan.lanes.end())route.points[index]={target.x,target.y,target.z};else lane->anchors[index].point=target;}else splinekit::SetHandle(h,g_ed.kitTarget==1,target);}
        else if(id==kKitLink){auto& h=lane==plan.lanes.end()?plan.handles[index]:lane->anchors[index].handle;h.linked=!h.linked;if(h.linked)splinekit::SetHandle(h,false,h.outgoing);}
        else {
            if(id==kKitProfileAdd){const double at=double(index);if(plan.profiles.size()>=512)return true;if(std::any_of(plan.profiles.begin(),plan.profiles.end(),[&](const auto& k){return k.at==at&&k.lane==g_ed.laneFocus;})){Status("A profile already exists here for this scope.");return true;}splinekit::ProfileKey key;key.id=plan.nextId++;key.at=at;key.lane=g_ed.laneFocus;key.profile=trafficroute::SettingsAt(EditorLine(route),EditorLine(route).ToLine(at),std::max(0,g_ed.laneFocus)).profile;key.profile.offset=0;key.profile.elevation=0;plan.profiles.push_back(key);g_ed.kitProfile=int(plan.profiles.size()-1);}
            else if(!plan.profiles.empty()){
                auto at=std::min(std::size_t(std::max(0,g_ed.kitProfile)),plan.profiles.size()-1);auto& key=plan.profiles[at];auto& p=key.profile;
                if(id==kKitProfileNext){g_ed.kitProfile=int((at+1)%plan.profiles.size());g_ed.kitVertex=0;return true;}
                if(id==kKitProfileRemove){plan.profiles.erase(plan.profiles.begin()+at);g_ed.kitProfile=0;}
                else if(id==kKitProfileShape){p.shape=lanevolume::Shape((int(p.shape)+1)%5);p.radius=std::min(p.width,p.height)*.2;if(p.shape==lanevolume::Shape::Polygon)p.polygon={{-.5,0},{.5,0},{.5,1},{-.5,1}};}
                else if(id==kKitProfileScope){key.lane=g_ed.laneFocus;}
                else if(id==kKitVertexNext){if(!p.polygon.empty())g_ed.kitVertex=(g_ed.kitVertex+1)%int(p.polygon.size());return true;}
                else if(id==kKitVertexAdd){if(p.shape!=lanevolume::Shape::Polygon||p.polygon.size()>=64)return true;const auto i=std::min(std::size_t(std::max(0,g_ed.kitVertex)),p.polygon.size()-1),j=(i+1)%p.polygon.size();const auto a=p.polygon[i],b=p.polygon[j];p.polygon.insert(p.polygon.begin()+i+1,{(a.x+b.x)*.5,(a.y+b.y)*.5});g_ed.kitVertex=int(i+1);}
                else if(id==kKitVertexRemove){if(p.polygon.size()<=3)return true;p.polygon.erase(p.polygon.begin()+std::min(std::size_t(std::max(0,g_ed.kitVertex)),p.polygon.size()-1));g_ed.kitVertex=0;}
                else if(id>=kKitProfileBase&&id<kKitProfileBase+20){const int field=(id-kKitProfileBase)/2;const double sign=(id-kKitProfileBase)%2?1:-1;if(field==0)key.at=std::clamp(key.at+sign*.1,0.0,double(route.points.size()-1)+(route.closed?.9:0));if(field==1)p.width=std::clamp(p.width+sign*g_ed.kitStep,.25,100.0);if(field==2)p.height=std::clamp(p.height+sign*g_ed.kitStep,.25,200.0);if(field==3)p.bank=std::clamp(p.bank+sign*5*lanevolume::kPi/180,-64*lanevolume::kPi,64*lanevolume::kPi);if(field==4)p.offset+=sign*g_ed.kitStep;if(field==5)p.elevation+=sign*g_ed.kitStep;if(field==6)p.radius=std::clamp(p.radius+sign*.1,0.0,std::min(p.width,p.height)*.5);if(field==7)p.topWidth=std::clamp(p.topWidth+sign*g_ed.kitStep,.25,100.0);if(field>=8&&!p.polygon.empty()){auto& v=p.polygon[std::min(std::size_t(std::max(0,g_ed.kitVertex)),p.polygon.size()-1)];if(field==8)v.x=std::clamp(v.x+sign*.025,-.5,.5);else v.y=std::clamp(v.y+sign*.025,0.0,1.0);}}
            }else return true;
        }
    }
    plan.prepared=false;for(auto& key:plan.profiles)key.sampled.clear();
    // Invalid drafts stay editable, and are never admitted to traffic.
    PushSnap();g_ed.draft->Edit(route);g_ed.massUndo.back().draftChanged=true;g_ed.massUndo.back().depth=DraftDepth();g_ed.view.volumes=true;Status("Toolkit draft changed. Undo / Test / Cancel / Save use the existing workflow.");return true;
}
