// Shared by production F7 and the headless native-layout regression (both include f7layers.h first).
void ContextLayout(const trafficdata::Route* shown,const routemap::Camera& camera){
    using editorcontext::Surface;using editorcontext::Popup;
    trafficdata::Route placeScratch;const bool hasRoute=shown!=nullptr;if(!shown&&ContextPlaceActive())shown=&placeScratch;
    g_contextRects.clear();const auto key=ContextKey();if(key!=g_contextObserved){g_contextObserved=key;if(shown&&(g_ed.point>=0||ContextLane()||g_ed.spot>=0||g_ed.zone>=0||g_ed.link>=0))g_context.Select(key);}
    if(g_ed.sheet&&g_context.surface==Surface::Edit)g_context.Advanced();
    const bool browse=g_context.surface==Surface::Browse;g_contextPanel=browse||g_context.surface==Surface::Advanced||(g_context.pinned&&g_context.surface==Surface::Edit);const bool sourcePanel=g_contextPanel||(g_context.surface==Surface::Popup&&g_ed.sheet);
    auto add=[&](int id,double x,double y,double w,const std::string& label,bool enabled=true,bool active=false){g_ui.buttons.push_back({id,x,y,w,28,label,active,enabled,KeyOf(id),0});};
    auto box=[&](editorcontext::Rect r){g_contextRects.push_back(r);g_ui.boxes.insert(g_ui.boxes.begin(),{r.x,r.y,r.w,r.h,0x141C29,.97,0x3A4658,1});};
    const auto test=std::find_if(g_ui.buttons.begin(),g_ui.buttons.end(),[](const Button& b){return b.id==kTest;});const double footer=test==g_ui.buttons.end()?g_ui.panelBottom:test->y-6;
    auto keep=[&](double x,double y){return x<PanelX()-1||y<footer-4;};
    std::erase_if(g_ui.buttons,[&](const Button& b){return b.style!=1&&b.style!=4&&(!keep(b.x,b.y)||(!sourcePanel&&b.x>=PanelX()));});
    std::erase_if(g_ui.boxes,[&](const Box& b){return !keep(b.x,b.y)||(!sourcePanel&&b.x>=PanelX());});std::erase_if(g_panelText,[&](const auto& t){return !keep(t.x,t.y)||(!sourcePanel&&t.x>=PanelX());});if(!sourcePanel)g_ui.polygons.clear();
    if(g_context.surface==Surface::Advanced&&g_contextDirectory){std::erase_if(g_ui.buttons,[&](const Button& b){return b.style!=1&&b.style!=4&&b.x>=PanelX();});g_panelText.clear();g_ui.boxes.clear();g_ui.polygons.clear();g_ui.panelBottom=535;g_panelText.push_back({"Advanced controls",PanelX()+14,28,kInner,kText,kUnit});g_contextLinks.clear();const char* categories[]{"Geometry / splines / profiles","Position / lane placement","Vehicle groups / exclusions","Junctions / connections","Journeys / scripted actions","Parking / pickups","Population / schedules","Parents / enforced rules","Inspection / readiness","Save / shared routes"};if(g_contextCategory<0){for(int i=0;i<10;++i)add(kContextCategories+i,PanelX()+14,84+i*34,kInner,categories[i],shown||i==6||i==7||i==8);}
        else {
            switch(g_contextCategory){
            case 0:g_contextLinks={{kLaneDesign,"Lane counts / shape / taper"},{kLaneDimensions,"Cross-section dimensions"},{kKitOpen,"3D spline / incoming / outgoing"},{kKitProfiles,"Loft / cross-section profiles"}};break;
            case 1:g_contextLinks={{kLanePlacement,"Lane position / offsets"},{kLaneRules,"Movement authority / obstacles"},{kLaneTravelRules,"Drive / hover permissions"},{kLaneHeightRules,"Clearance / hover heights"},{kKitOpen,"Freeform path / transforms"}};break;
            case 2:g_contextLinks={{kLaneVehicleRules,"Selected lane / route exclusions"},{kGroupSheet,"Named vehicle groups"}};break;
            case 3:g_contextLinks={{kSheetBack,"Selected junction / turning point"},{kConnect,"Create route connection"},{kNetworkPreview,"Network / itinerary preview"}};break;
            case 4:g_contextLinks={{kJourneySheet,"Stops / dwell / repeat"},{kJourneyActions,"Triggers / timeout / fallback"}};break;
            case 5:g_contextLinks={{kSheetBack,"Selected place / occupancy"},{kLanding,"Place landing / pickup area"},{kPlaceAccess,"Entrance / departure paths"},{kSpawn,"Place spawn point"}};break;
            case 6:g_contextLinks={{kSchedule,"Route population"},{kTimeRules,"Hours / schedules"},{kPopulationSheet,"Fleet readiness / waiting"},{kCity,"City traffic / density / limits"}};break;
            case 7:g_contextLinks={{kRestrictionSheet,"Selected route enforced rules"},{kRestrictionCity,"World enforced rules"},{kGroupSheet,"Groups / inheritance"},{kLaneDesign,"Parent lane defaults"},{kCity,"World traffic defaults"}};break;
            case 8:g_contextLinks={{kInspectMovement,"Active controller / journey"},{kPopulationSheet,"Population readiness"},{kNetworkPreview,"Network / signals / live paths"},{kLaneEnvelope,"Vehicle envelope fit"}};break;
            case 9:g_contextLinks={{kSheetBack,"Selected route / normal draft"},{kPermanentSave,"Permanent Save selected route"},{kPermanentDelete,"Permanent Delete shared copy"},{kReadme,"Controls guide"}};break;
            }g_panelText.push_back({categories[g_contextCategory],PanelX()+14,86,kInner,kTeal,kUnit});for(int i=0;i<int(g_contextLinks.size());++i)add(9300+i,PanelX()+14,118+i*36,kInner,g_contextLinks[i].second);
        }add(kPermanentSave,PanelX()+14,438,170,"Permanent Save",shown);add(kPermanentDelete,PanelX()+190,438,174,"Permanent Delete",shown&&g_ed.permanentFiles.count(shown->name));}
    if(g_contextPanel){std::erase_if(g_ui.buttons,[](const Button& b){return b.style!=1&&b.style!=4&&b.y<78&&b.x>=PanelX();});std::erase_if(g_panelText,[](const auto& t){return t.y<78;});g_panelText.push_back({browse?"Routes / Places":"Advanced controls",PanelX()+14,25,kInner,kText,kUnit});add(kContextBack,PanelX()+14,48,90,"Selection",shown);add(kContextMore,PanelX()+110,48,116,"Categories",true);add(kContextPin,PanelX()+232,48,132,g_context.pinned?"Unpin browser":"Pin browser",true,g_context.pinned);}
    double cx=g_ui.width*.5,cy=g_ui.height*.45;if(shown){const auto centre=ContextCentre(*shown);Local(camera,{centre.x,centre.y,centre.z},cx,cy);}cx=std::clamp(cx,80.0,std::max(80.0,PanelX()-210));cy=std::clamp(cy,140.0,std::max(140.0,g_ui.height-400));
    if(g_context.surface==Surface::Popup&&g_ed.sheet){
        std::erase_if(g_ui.buttons,[](const Button& b){return b.style!=1&&b.style!=4&&b.x>=PanelX()&&b.y<78;});std::erase_if(g_panelText,[](const auto& t){return t.y<78;});
        // The existing sheet remains the source of controls; relocate it beside selection.
        double sheetBottom=78;for(const auto& b:g_ui.buttons)if(b.style!=1&&b.style!=4&&b.x>=PanelX())sheetBottom=std::max(sheetBottom,b.y+b.h);for(const auto& t:g_panelText)if(t.x>=PanelX())sheetBottom=std::max(sheetBottom,t.y+20);
        const auto popup=editorcontext::Clamp({cx+90,cy+40,kPanelW,std::max(180.0,sheetBottom)},g_ui.width,g_ui.height-190);const double dx=popup.x-PanelX(),dy=popup.y-12;
        // Rebuild retained sheet because Edit hides the legacy panel above only for actual edit mode.
        for(auto& b:g_ui.buttons)if(b.style!=1&&b.style!=4&&b.x>=PanelX()){b.x+=dx;b.y+=dy;}for(auto& b:g_ui.boxes)if(b.x>=PanelX()){b.x+=dx;b.y+=dy;}for(auto& t:g_panelText)if(t.x>=PanelX()){t.x+=dx;t.y+=dy;}box(popup);add(kContextBack,popup.x+14,popup.y+40,kInner,"Back to selection");
    }
    double f7TabsTop=g_ui.height-170;bool f7Docked=false;
    if(!browse&&shown&&g_context.surface==Surface::Edit){
        // Three layers follow one selection and one open group (F7 spec 2026-11-03).
        const std::string routeName=shown->label.empty()?shown->name:shown->label;
        const int n=int(shown->points.size()),point=F7Point(*shown);const bool pinned=g_ed.point>=0;
        const bool road=!shown->ring;const int lanes=RoadLanes(*shown);
        const std::size_t tile=n>1?std::size_t(std::min(std::max(0,point),shown->closed?n-1:n-2)):0;
        const auto primitive=roadplan::PrimitiveAt(shown->road,tile);
        std::string title;
        if(g_ed.link>=0)title="Link "+std::to_string(g_ed.link+1);
        else if(g_ed.spot>=0)title="Place "+std::to_string(g_ed.spot+1);
        else if(g_ed.zone>=0)title="Zone "+std::to_string(g_ed.zone+1);
        else if(ContextLane())title="Lane "+std::to_string(g_ed.laneFocus+1)+" - "+routeName;
        else if(n==0)title="This click - "+std::string(f7layers::ShortKind(g_f7NextPrim?g_f7NextPrim:roadplan::Flat));
        else if(point>=0&&g_f7Group==f7layers::Field&&road&&n>1){const int next=int(tile)+1<n?int(tile)+1:0;
            title=g_f7Divider&&lanes>1?"Divider "+std::to_string(g_roadDivider+1)+" - tile "+std::to_string(tile+1)+"-"+std::to_string(next+1):"Tile "+std::to_string(tile+1)+"-"+std::to_string(next+1)+" - "+routeName;}
        else if(point>=0)title="Point "+std::to_string(point+1)+" - "+routeName;
        else title="Route - "+routeName;
        constexpr double kAdv=f7layers::kLetterPx;
        // Layer 2: the dock, fixed on the footer's centre line just above Test / Cancel / Save.
        const auto dock=editorcontext::Clamp({g_ui.width*.5-330+g_f7Offset[1].dx,g_ui.height-162+g_f7Offset[1].dy,660,150},g_ui.width,g_ui.height);box(dock);g_f7AreaRect[1]=dock;
        const auto tabs=dock;f7TabsTop=dock.y;f7Docked=true;
        for(int gi=0;gi<4;++gi)add(kF7GroupBase+gi,dock.x+12+gi*86,dock.y+10,82,f7layers::GroupName(gi),true,g_f7Group==gi);
        add(kContextMore,dock.x+364,dock.y+10,100,"More");
        g_panelText.push_back({f7layers::Fit(std::string(f7layers::GroupName(g_f7Group))+" - "+title,420,kAdv),dock.x+120,dock.y+44,420,kTeal,kUnit});
        add(kTest,dock.x+556,dock.y+48,92,"Test",hasRoute);add(kCancel,dock.x+556,dock.y+82,92,"Cancel");add(kSave,dock.x+556,dock.y+116,92,"Save",true,true);
        // Turning preview of the selected cross-section on its platform, with its dividers.
        {
            const double pcx=dock.x+60,pcy=dock.y+108,scale=30,turn=std::fmod(g_f7Clock*.6,6.283185307179586);
            UiPolygon platform;platform.color=0x8C9AAB;platform.alpha=.18;for(int i=0;i<24;++i){const double a=6.283185307179586*i/24;platform.points.push_back({pcx+std::cos(a)*46,pcy+12+std::sin(a)*11});}g_ui.polygons.push_back(platform);
            const double ratio=shown->road.height>0?std::clamp(shown->road.height/std::max(1.0,shown->road.width>0?shown->road.width*.5:shown->corridor*std::clamp(shown->lanes,1,2)*.5),.15,1.4):.04;
            const auto outline=f7layers::Outline(primitive,ratio);
            for(double depth:{.6,-.6}){UiPolygon face;face.color=depth>0?0x6FE0D0:0x8C9AAB;face.alpha=depth>0?.32:.14;for(auto [cs,ch]:outline)face.points.push_back(f7layers::Project(cs,ch,depth,turn,pcx,pcy,scale));g_ui.polygons.push_back(face);}
            for(int d=0;d<lanes-1;++d){const auto rule=roadplan::DividerAt(shown->road,std::size_t(d),tile);const double side=lanes==2&&shown->road.dividers.empty()?rule.side:rule.side;
                const double top=std::clamp(side+ratio*std::tan(rule.tilt*3.141592653589793/180),-1.0,1.0);
                UiPolygon wall;wall.color=0xE93452;wall.alpha=.45;for(auto q:{std::pair{side,0.0},std::pair{top,ratio}})wall.points.push_back(f7layers::Project(q.first,q.second,.6,turn,pcx,pcy,scale));
                for(auto q:{std::pair{top,ratio},std::pair{side,0.0}})wall.points.push_back(f7layers::Project(q.first,q.second,-.6,turn,pcx,pcy,scale));g_ui.polygons.push_back(wall);}
        }
        // One row of choices for the open group, each also on its key.
        const double cx0=dock.x+120,cy0=dock.y+62,vy=dock.y+100;
        if(g_f7Group==f7layers::Field){
            if(g_f7LinkPrompt>=0&&road)for(int k=1;k<=3;++k)add(kF7LinkBase+k,cx0+(k-1)*140,cy0,136,std::to_string(k)+" "+f7layers::LinkName(k),true,roadplan::At(shown->road,std::size_t(std::max(0,g_f7LinkPrompt))).connector==k);
            else for(int k=1;k<=5;++k)add(kF7PrimBase+k,cx0+(k-1)*84,cy0,80,std::to_string(k)+" "+f7layers::ShortKind(k),road,(n==0?(g_f7NextPrim?g_f7NextPrim:roadplan::Flat):primitive.kind)==k);
        }else if(g_f7Group==f7layers::Flow){
            if(road){const bool along=roadplan::Direction(shown->road,g_roadLane,lanes)>0;add(kF7FlowAlong,cx0,cy0,180,"Lane "+std::to_string(g_roadLane+1)+" along",true,along);add(kF7FlowAgainst,cx0+186,cy0,180,"Lane "+std::to_string(g_roadLane+1)+" against",true,!along);}
            else add(kContextScope,cx0,cy0,180,g_context.sections?"Sections":"Whole lane");
        }else if(g_f7Group==f7layers::Position){add(kF7PosDown,cx0,cy0,150,"Lower 5 m",n>0);add(kF7PosUp,cx0+156,cy0,150,"Raise 5 m",n>0);}
        else {auto rule=laneplan::Resolve(shown->lanePlan,g_ed.laneScope?g_ed.point:-1,g_ed.laneFocus);const auto travel=shown->lanePlan.enabled?rule.travel:shown->travel;
            const char* names[]{"Both","Drive","Hover"};for(int t=0;t<3;++t)add(kF7TravelBase+t,cx0+t*126,cy0,120,names[t],true,int(travel)==t);}
        // Four values; a value opens its exact field in the side panel (layer 3) or its existing sheet.
        auto value=[&](int slot,int id,const std::string& label,bool enabled=true){add(id,cx0+slot*106,vy,102,f7layers::Fit(label+" >",102-10,kAdv),enabled);};
        char a[48],b[48],c2[48],d[48];
        if(g_f7Group==f7layers::Field){
            std::snprintf(a,sizeof a,"Roof %.1f m",shown->road.height);std::snprintf(b,sizeof b,"Width %.1f m",shown->road.width>0?shown->road.width:shown->corridor*std::clamp(shown->lanes,1,2));
            std::snprintf(c2,sizeof c2,"Lanes %d",lanes);
            if(primitive.kind==roadplan::Rounded)std::snprintf(d,sizeof d,"Corner %.1f m",primitive.param);else if(primitive.kind==roadplan::Trapezoid)std::snprintf(d,sizeof d,"Top %.0f%%",primitive.param*100);else std::snprintf(d,sizeof d,"Join %s",f7layers::LinkName(roadplan::At(shown->road,tile).connector));
            value(0,kF7ValueBase,a,road);value(1,kF7ValueBase+1,b,road);value(2,kF7ValueBase+2,c2,road);value(3,kF7ValueBase+3,d,road);
        }else if(g_f7Group==f7layers::Flow){
            std::snprintf(a,sizeof a,"Lanes %d",lanes);value(0,kF7ValueBase,a,road);value(1,kF7ValueBase+1,"Lane "+std::to_string(g_roadLane+1),road&&lanes>1);
            value(2,kContextVehicles,"Vehicles");std::snprintf(d,sizeof d,"Divider %d",g_roadDivider+1);value(3,kF7ValueBase+3,d,road&&lanes>1);
        }else if(g_f7Group==f7layers::Position){
            auto s=laneplan::Resolve(shown->lanePlan,g_ed.laneScope?g_ed.point:-1,g_ed.laneFocus);if(!shown->lanePlan.enabled)s.profile.width=shown->corridor;
            value(0,kContextExact,"Exact XYZ");std::snprintf(b,sizeof b,"Width %.2f",s.profile.width);value(1,kContextWidth,b);
            std::snprintf(c2,sizeof c2,"Z %.0f m",point>=0&&point<n?shown->points[point].z:0.0);value(2,kF7ValueBase+2,c2,point>=0);
            const double radius=point>=0&&point<int(shown->rounding.size())?shown->rounding[point]:0;std::snprintf(d,sizeof d,"Corner %.1f m",radius);value(3,kF7ValueBase+3,d,point>=0&&!shown->spacePlan.enabled);
        }else{
            auto s=laneplan::Resolve(shown->lanePlan,g_ed.laneScope?g_ed.point:-1,g_ed.laneFocus);if(!shown->lanePlan.enabled)s.travel=shown->travel;
            value(0,kContextTravel,std::string("Travel ")+routepolicy::Name(s.travel));value(1,kContextMovement,"Movement");value(2,kContextVehicles,"Vehicles");
        }
        if(g_ed.point>=0&&g_ed.point<n&&!ContextLane()){
            const double requested=g_ed.point<int(shown->rounding.size())?shown->rounding[g_ed.point]:0,actual=std::min(requested,routecurve::CornerLimit(shown->points,g_ed.point,shown->closed));char ctitle[100];std::snprintf(ctitle,sizeof ctitle,"Corner %d: radius %.2f m / actual %.2f m",g_ed.point+1,requested,actual);const auto corner=editorcontext::Clamp({tabs.x+55,tabs.y-84,510,76},g_ui.width,g_ui.height-185);box(corner);g_panelText.push_back({shown->spacePlan.enabled?"Corner uses incoming / outgoing spline tangents":ctitle,corner.x+8,corner.y+6,494,kText,kUnit});add(kContextSharp,corner.x+8,corner.y+34,110,"Sharp");add(kContextRound,corner.x+124,corner.y+34,118,shown->spacePlan.enabled?"Handles":"Rounded");add(kContextCorner,corner.x+248,corner.y+34,124,"Exact radius",!shown->spacePlan.enabled);add(kContextCornerDrag,corner.x+378,corner.y+34,122,"Drag bend",!shown->spacePlan.enabled);
        }
        if(g_context.surface==Surface::Edit){for(int i=0;i<3;++i){const double x=cx-64+i*72;box({x,cy-14,66,30});add(kContextMove+i,x,cy-14,66,i==0?"Move":i==1?"Turn":"Z",i!=1||ContextLane()||ContextPlaceActive()||g_ed.point<0||shown->spacePlan.enabled);}}
        // Layer 1: the tag on the selected item. Nearest stays on screen; a pinned item may carry it off screen.
        if(n>0){
            double ax=0,ay=0;bool anchor=false;
            if(point>=0&&point<n&&!ContextLane()&&g_ed.spot<0&&g_ed.zone<0){const auto q=shown->points[point];anchor=Local(camera,{q.x,q.y,q.z},ax,ay);}
            else {const auto q=ContextCentre(*shown);anchor=Local(camera,{q.x,q.y,q.z},ax,ay);}
            const bool onScreen=anchor&&ax>=0&&ax<=g_ui.width&&ay>=0&&ay<=g_ui.height;
            if(anchor&&(!pinned||onScreen)){
                const std::string badge=pinned?"pinned":"nearest";
                const double tw=std::clamp(double(title.size()+badge.size()+3)*kAdv+24,220.0,380.0),th=50;
                // Reserved zones: everything placed so far (dock, tabs, corner, handles), the side panel, the toolbar.
                std::vector<editorcontext::Rect> reserved=g_contextRects;reserved.push_back({g_ui.barX,g_ui.barY,g_ui.barW,g_ui.barH});
                if(g_contextPanel||g_roadPanel)reserved.push_back({PanelX(),0,kPanelW+12,g_ui.height});
                auto freeAt=[&](editorcontext::Rect r){if(r.x<12||r.y<12||r.x+r.w>g_ui.width-12||r.y+r.h>g_ui.height-12)return false;for(const auto& z:reserved)if(editorcontext::Intersects({r.x-8,r.y-8,r.w+16,r.h+16},z))return false;return true;};
                editorcontext::Rect tag{0,0,0,0};bool placed=false;
                for(auto c:{editorcontext::Rect{ax-tw/2,ay-th-56,tw,th},{ax-tw/2,ay+52,tw,th},{ax+80,ay-th/2,tw,th},{ax-80-tw,ay-th/2,tw,th}})if(freeAt(c)){tag=c;placed=true;break;}
                if(!placed&&!pinned){tag={std::clamp(ax-tw/2,12.0,std::max(12.0,g_ui.width-tw-12)),std::max(96.0,tabs.y-th-12),tw,th};placed=true;}
                if(placed){
                g_contextRects.push_back(tag);
                g_ui.boxes.insert(g_ui.boxes.begin(),{tag.x,tag.y,tag.w,tag.h,0x141C29,.97,pinned?kTeal:0x3A4658u,pinned?1.0:0.0});
                if(!pinned)for(double x=tag.x+14;x<tag.x+tag.w-14;x+=12){g_ui.boxes.push_back({x,tag.y,6,1,kTeal,.8});g_ui.boxes.push_back({x,tag.y+tag.h-1,6,1,kTeal,.8});}
                g_panelText.push_back({f7layers::Fit(title,tw-28-double(badge.size())*kAdv,kAdv),tag.x+12,tag.y+6,tw-24,kText,kUnit});
                g_panelText.push_back({badge,tag.x+tag.w-12-double(badge.size())*kAdv,tag.y+6,double(badge.size())*kAdv+2,pinned?kTeal:kDim,kUnit});
                g_panelText.push_back({std::string(f7layers::GroupName(g_f7Group))+" group",tag.x+12,tag.y+28,tw-24,kDim,kUnit});
                }
            }
        }
    }
    if(shown&&g_context.popup==Popup::Transform){auto r=editorcontext::Clamp({cx+100,cy-90,300,360},g_ui.width,g_ui.height-190);box(r);const char* names[]{"X position (m)","Y position (m)","Z position (m)","Rotate X by (deg)","Rotate Y by (deg)","Rotate Z by (deg)","Scale factor","Width (m)","Height (m)"};for(int i=0;i<9;++i)add(kContextNumber+i,r.x+10,r.y+10+i*33,280,names[i],!ContextPlaceActive()||i<3||i==5);add(kContextBack,r.x+10,r.y+310,280,"Back");}
    if(g_ed.menu==1){std::vector<Button> menu;for(const auto& b:g_ui.buttons)if(b.style==4)menu.push_back(b);std::erase_if(g_ui.buttons,[](const Button& b){return b.style==4;});const int capacity=std::max(3,std::min(8,int((g_ui.height-g_ui.menuY-200)/28)-1));const int pages=std::max(1,(int(menu.size())+capacity-1)/capacity);g_contextMenuPage=std::clamp(g_contextMenuPage,0,pages-1);int row=0;for(int i=g_contextMenuPage*capacity;i<std::min(int(menu.size()),(g_contextMenuPage+1)*capacity);++i){auto b=menu[i];b.y=g_ui.menuY+row++*28;g_ui.buttons.push_back(b);}if(pages>1){g_ui.buttons.push_back({9016,g_ui.menuX,g_ui.menuY+row*28,135,28,"Previous",false,g_contextMenuPage>0,"",4});g_ui.buttons.push_back({9017,g_ui.menuX+135,g_ui.menuY+row++*28,135,28,"More options",false,g_contextMenuPage+1<pages,"",4});}g_ui.menuH=row*28;}
    if(shown&&!shown->ring){
        if(g_roadPanel&&!g_contextPanel){  // one side panel at a time: the pinned Traffic Manager wins
            // N lanes = N-1 red dividers; each divider and each lane is chosen and edited on its own.
            const int lanes=RoadLanes(*shown),dividers=lanes-1,n=int(shown->points.size());
            g_roadDivider=std::clamp(g_roadDivider,0,std::max(0,dividers-1));g_roadLane=std::clamp(g_roadLane,0,lanes-1);
            const std::size_t tile=n>1?std::size_t(std::min(std::max(0,F7Point(*shown)),shown->closed?n-1:n-2)):0;const auto primitive=roadplan::PrimitiveAt(shown->road,tile);
            const double top=12,bottom=std::min(g_ui.height-12.0,f7Docked?f7TabsTop-8:g_ui.height-190.0);
            const auto panel=editorcontext::Rect{PanelX(),top,kPanelW,std::max(160.0,bottom-top)};box(panel);g_f7AreaRect[2]=panel;
            g_panelText.push_back({"Field - tile "+std::to_string(tile+1),panel.x+12,panel.y+8,kInner-90,kTeal,kUnit});add(9500,panel.x+panel.w-96,panel.y+2,84,"Close");
            char dividerWidthText[48];std::snprintf(dividerWidthText,sizeof dividerWidthText,shown->road.median>0?"Divider width %.1f m":"Divider width: a line",shown->road.median);
            char lanesText[48],dividerText[48],laneText[48],directionText[64],shapeText[64],widthText[48],paramText[64];
            std::snprintf(lanesText,sizeof lanesText,"Lanes: %d (%d divider%s)",lanes,dividers,dividers==1?"":"s");
            std::snprintf(dividerText,sizeof dividerText,dividers?"Editing divider %d of %d":"No divider (one way)",g_roadDivider+1,dividers);
            std::snprintf(laneText,sizeof laneText,"Lane %d of %d (right edge first)",g_roadLane+1,lanes);
            std::snprintf(directionText,sizeof directionText,"Lane %d travels %s markers",g_roadLane+1,roadplan::Direction(shown->road,g_roadLane,lanes)>0?"along":"against");
            std::snprintf(shapeText,sizeof shapeText,"Shape: %d %s (1-5 changes)",primitive.kind,roadplan::KindName(primitive.kind));
            std::snprintf(widthText,sizeof widthText,"Road width %.1f m",shown->road.width>0?shown->road.width:shown->corridor*std::clamp(shown->lanes,1,2));
            if(primitive.kind==roadplan::Rounded)std::snprintf(paramText,sizeof paramText,"Corner radius %.1f m",primitive.param);
            else if(primitive.kind==roadplan::Trapezoid)std::snprintf(paramText,sizeof paramText,"Top width %.0f%% of floor",primitive.param*100);
            else std::snprintf(paramText,sizeof paramText,"No shape value for %s",roadplan::KindName(primitive.kind));
            struct Row{int id;std::string label;bool enabled;};
            const std::vector<Row> rows{{9501,g_roadAll?"All tiles":"Selected marker's next tile",true},{kF7FieldShape,shapeText,true},{9502,"Road height (m)",true},{kF7FieldWidth,widthText,true},
                {kF7FieldParam,paramText,primitive.kind==roadplan::Rounded||primitive.kind==roadplan::Trapezoid},{9508,lanesText,true},{9510,dividerText,dividers>1},
                {kF7FieldMedian,dividerWidthText,dividers>0},{9503,"Divider sideways (%)",dividers>0},{9504,"Divider swing (%)",dividers>0},{9505,"Divider tilt (degrees)",dividers>0},{9511,laneText,lanes>1},{9512,directionText,true},
                {9506,"1 Bend at preceding marker",true},{9507,"2 Sharp join",true},{kF7Taper,"3 Taper join",true},{9509,shown->road.enabled?"Use previous path geometry":"Enable road tiles",true}};
            const int perPage=std::max(3,int((panel.h-120)/32));const int pages=std::max(1,(int(rows.size())+perPage-1)/perPage);g_f7FieldPage=std::clamp(g_f7FieldPage,0,pages-1);
            for(int i=g_f7FieldPage*perPage,k=0;i<std::min(int(rows.size()),(g_f7FieldPage+1)*perPage);++i,++k){const auto& row=rows[std::size_t(i)];add(row.id,panel.x+12,panel.y+34+k*32,kInner,f7layers::Fit(row.label,kInner-16,f7layers::kLetterPx),row.enabled,g_f7Highlight==row.id);}
            // Lane width against vehicle size for N lanes and for N+1 (the next divider): red none fit, yellow small only.
            {const double roadFull=shown->road.width>0?shown->road.width:shown->corridor*std::clamp(shown->lanes,1,2);
             for(int k=0;k<2;++k){const int n=lanes+k;const double w=(roadFull-shown->road.median*(n-1))/n;const bool none=w<kLaneNoneFit,tight=w<kLaneAllFit;char b[96];
                 std::snprintf(b,sizeof b,"%s%d lane%s: %.1f m each - %s",k?"With ":"",n,n==1?"":"s",w,none?"no vehicle fits":tight?"small vehicles only":"all vehicles fit");
                 g_panelText.push_back({f7layers::Fit(b,kInner,f7layers::kLetterPx),panel.x+12,panel.y+panel.h-82+k*18,kInner,none?kClearanceRed:tight?kAmber:kDim,kUnit});}}
            if(pages>1){add(kF7FieldPrev,panel.x+12,panel.y+panel.h-36,170,"Previous",g_f7FieldPage>0);add(kF7FieldNext,panel.x+12+kInner-170,panel.y+panel.h-36,170,"More fields",g_f7FieldPage+1<pages);}
        }
    }
    if(!f7Docked){const auto foot=editorcontext::Clamp({g_ui.width*.5-190,g_ui.height-183,380,44},g_ui.width,g_ui.height);box(foot);add(kTest,foot.x+8,foot.y+8,116,"Test",hasRoute);add(kCancel,foot.x+132,foot.y+8,116,"Cancel");add(kSave,foot.x+256,foot.y+8,116,"Save",true,true);}
    if(g_contextPanel)g_f7AreaRect[2]={PanelX(),12,kPanelW,std::max(160.0,g_ui.panelBottom-12)};
    F7WirePanel();  // a selected landing lists its wires in the side panel
    if(g_contextNumber>=0){const auto r=editorcontext::Clamp({g_ui.width*.5-220,110,440,82},g_ui.width,g_ui.height);box(r);g_panelText.push_back({"Exact value: "+g_contextText+"_",r.x+12,r.y+12,r.w-24,kTeal,kUnit});g_panelText.push_back({"Type to replace. Enter commits; Esc cancels.",r.x+12,r.y+40,r.w-24,kDim,kUnit});}
}
