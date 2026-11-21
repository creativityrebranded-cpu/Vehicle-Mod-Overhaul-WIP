const auto& plan=shown->spacePlan;const auto& compiled=EditorLine(*shown);
section(g_ed.sheet==19?"3D spline toolkit":"Corridor profiles",plan.enabled?"Enabled":"Legacy geometry",kTeal);
action("Geometry",plan.enabled?"3D cubic spline":"Legacy rounding retained",kKitEnable,plan.enabled?"Disable":"Enable",!shown->ring&&shown->points.size()>=2&&plan.unsupported.empty());
if(!plan.unsupported.empty())line("Unsupported record retained; editing disabled.",kAmber);
if(compiled.error)line(compiled.detailError.empty()?compiled.error:compiled.detailError,kCoral);
line(std::to_string(compiled.Count())+" samples; tolerance "+routefiles::Num(plan.tolerance)+" m",kDim);
action("Scope",g_ed.laneFocus<0?"Shared spline / all lanes":"Lane "+std::to_string(g_ed.laneFocus),kKitLaneNext,"Next",plan.enabled);
action("Edit step",routefiles::Num(g_ed.kitStep)+" m",kKitStep,"Change",true);
auto number=[&](int base,int field,const char* title,double value,bool enabled=true){text(title,x0,y+4,132,kText);text(routefiles::Num(value),x0+132,y+4,94,kDim);button(base+field*2,x0+kInner-64,y,28,22,"-",false,enabled,0);button(base+field*2+1,x0+kInner-32,y,28,22,"+",false,enabled,0);y+=28;};
if(g_ed.sheet==19){
    const auto index=std::min(std::size_t(std::max(0,g_ed.point)),shown->points.empty()?0:shown->points.size()-1);const auto independent=std::find_if(plan.lanes.begin(),plan.lanes.end(),[&](const auto& p){return p.lane==g_ed.laneFocus;});
    action("Lane path",independent==plan.lanes.end()?"Follows shared spline":"Independent anchors",kKitDetach,independent==plan.lanes.end()?"Detach":"Rejoin",g_ed.laneFocus>=0&&plan.enabled);
    action("Anchor",std::to_string(index+1)+" / "+std::to_string(shown->points.size()),kKitAnchorNext,"Next",plan.enabled);
    action("Target",g_ed.kitTarget==0?"Anchor":g_ed.kitTarget==1?"Incoming handle":"Outgoing handle",kKitSide,"Next",plan.enabled);
    if(plan.enabled&&index<plan.handles.size()&&index<shown->points.size()){
        const auto& h=independent==plan.lanes.end()?plan.handles[index]:independent->anchors[index].handle;const auto p=independent==plan.lanes.end()?splinekit::Vec{shown->points[index].x,shown->points[index].y,shown->points[index].z}:independent->anchors[index].point;const auto target=g_ed.kitTarget==0?p:g_ed.kitTarget==1?h.incoming:h.outgoing;
        line(g_ed.kitTarget?"Handle values are offsets from the anchor.":"Anchor values are world coordinates.",kDim);
        number(kKitCoordBase,0,"X",target.x);number(kKitCoordBase,1,"Y",target.y);number(kKitCoordBase,2,"Z",target.z);
        action("Tangents",h.linked?"Linked / smooth":"Independent / corner",kKitLink,"Change",true);
        action("Map placement","At selected target Z",kKitPlace,"Place",true);
    }
    action("Tangents","Recalculate this path",kKitAuto,"Smooth",plan.enabled);
    action("Sampling error",routefiles::Num(plan.tolerance)+" m",kKitQuality,"Change",plan.enabled);
    action("Maximum step",routefiles::Num(plan.maxStep)+" m",kKitSpacing,"Change",plan.enabled);
    action("Profiles",std::to_string(plan.profiles.size())+" stations",kKitProfiles,"Shape",plan.enabled);
    if(independent!=plan.lanes.end()){
        section("Whole independent lane","Around lane centre",kTeal);
        const char* labels[]{"Move X","Move Y","Move Z","Yaw (5 degrees)","Pitch (5 degrees)","Roll (5 degrees)","Scale X (5%)","Scale Y (5%)","Scale Z (5%)"};for(int i=0;i<9;++i)number(kKitWholeBase,i,labels[i],i<3?g_ed.kitStep:i<6?5:1.05);
    }
    line("3D handles replace legacy rounding while enabled. Profiles follow transported frames. Intersections need explicit junctions. A vertical path still depends on vehicle capability.",kDim);
}else{
    action("At selected anchor","Uses current lane scope",kKitProfileAdd,"Add",plan.enabled);
    if(!plan.profiles.empty()){
        const auto index=std::min(std::size_t(std::max(0,g_ed.kitProfile)),plan.profiles.size()-1);const auto& key=plan.profiles[index];const auto& p=key.profile;
        action("Profile",std::to_string(index+1)+" / "+std::to_string(plan.profiles.size())+" (ID "+std::to_string(key.id)+")",kKitProfileNext,"Next",true);
        action("Profile scope",key.lane<0?"All lanes":"Lane "+std::to_string(key.lane),kKitProfileScope,"Set scope",true);
        const char* names[]{"Rectangle","Rounded rectangle","Ellipse","Trapezoid","Polygon"};action("Outline",names[std::clamp(int(p.shape),0,4)],kKitProfileShape,"Next",true);
        number(kKitProfileBase,0,"Source anchor + fraction",key.at);number(kKitProfileBase,1,"Width (m)",p.width);number(kKitProfileBase,2,"Height (m)",p.height);number(kKitProfileBase,3,"Twist (degrees)",p.bank*180/lanevolume::kPi);number(kKitProfileBase,4,"Side offset (m)",p.offset);number(kKitProfileBase,5,"Elevation (m)",p.elevation);number(kKitProfileBase,6,"Corner radius (m)",p.radius);number(kKitProfileBase,7,"Top width (m)",p.topWidth);
        auto outline=lanevolume::Section(p,8);if(p.shape==lanevolume::Shape::Polygon&&outline.empty()){outline=p.polygon;for(auto& v:outline){v.x*=p.width;v.y*=p.height;}}if(!outline.empty()){UiPolygon preview;preview.color=lanevolume::Valid(p)?kTeal:kCoral;preview.alpha=.3;for(auto v:outline)preview.points.push_back({x0+kInner*.5+v.x/p.width*180,y+110-v.y/p.height*100});g_ui.polygons.push_back(std::move(preview));y+=120;}
        if(p.shape==lanevolume::Shape::Polygon&&!p.polygon.empty()){
            const auto v=std::min(std::size_t(std::max(0,g_ed.kitVertex)),p.polygon.size()-1);action("Vertex",std::to_string(v+1)+" / "+std::to_string(p.polygon.size()),kKitVertexNext,"Next",true);number(kKitProfileBase,8,"Vertex X (-.5 to .5)",p.polygon[v].x);number(kKitProfileBase,9,"Vertex Y (0 to 1)",p.polygon[v].y);action("After selected vertex","Midpoint on edge",kKitVertexAdd,"Insert",p.polygon.size()<64);action("Selected vertex","",kKitVertexRemove,"Remove",p.polygon.size()>3);
        }
        action("Profile station","",kKitProfileRemove,"Remove",true);
    }
    line("Stations interpolate dimensions, outline and unwrapped twist. Invalid or crossed outlines stay editable but traffic waits. The volume is centred on the lane trajectory.",kDim);
    action("Spline / lane path","",kKitPath,"Edit",true);
}
button(kLaneBack,x0,y,kInner,24,"Back",true,true,0);y+=32;
