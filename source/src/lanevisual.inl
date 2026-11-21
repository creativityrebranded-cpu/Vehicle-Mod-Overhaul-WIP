// Existing Pen/Local projection only; no new rendering hooks or game calls.
bool PickLaneVolume(const routemap::Camera& camera,double x,double y,bool add) {
    LanePick picked;double best=14;
    auto test=[&](const trafficdata::Route& route) {
        const auto line=EditorLine(route);const auto lanes=trafficroute::Lanes(line,route.lanes);
        for(int lane:lanes)for(int i=0;i<int(line.Span());++i) {
            if(!trafficroute::Available(line,lane,i+.5))continue;
            const auto p=line.lane[lane][i],q=line.lane[lane][(i+1)%line.Count()];double ax,ay,bx,by;
            if(!Local(camera,{p.x,p.y,p.z},ax,ay)||!Local(camera,{q.x,q.y,q.z},bx,by))continue;
            const double dx=bx-ax,dy=by-ay,den=dx*dx+dy*dy,t=den>1e-8?std::clamp(((x-ax)*dx+(y-ay)*dy)/den,0.0,1.0):0;
            const double distance=std::hypot(x-ax-t*dx,y-ay-t*dy);
            if(distance<best){best=distance;picked={route.name,route.ring?-1:int(routepolicy::Segment(route.points.size(),route.closed,line.ToSource(i+t),lane)),lane};}
        }
    };
    if(g_ed.draft)test(g_ed.draft->Value());
    for(const auto& r:g_ed.file.routes)if(Usable(r)&&(!g_ed.draft||r.id!=g_ed.draft->Value().name))test(ToTd(r,EditorKey()));
    for(const auto& other:g_ed.others)if(!g_ed.draft||other.name!=g_ed.draft->Value().name)test(other.route);
    if(picked.route.empty())return false;
    trafficdata::Route currentScratch;const auto* current=Shown(currentScratch);
    if((!current||current->name!=picked.route)&&!(add&&g_ed.draft)){const auto prior=g_ed.lanePicks;const int index=RouteIndex(picked.route);if(index>=0)SelectRow(index);else {for(std::size_t i=0;i<g_ed.others.size();++i)if(g_ed.others[i].name==picked.route){SelectRow(int(g_ed.file.routes.size()+i));break;}}current=Shown(currentScratch);if(!current||current->name!=picked.route)return true;if(add)g_ed.lanePicks=prior;}
    if(!g_context.sections)picked.section=-1;
    g_context.Select(picked.route);
    auto identity=[&](const trafficdata::Route& r){if(r.name==picked.route&&picked.section>=0&&picked.section<int(r.lanePlan.sectionIds.size()))picked.identity=r.lanePlan.sectionIds[picked.section];};
    if(g_ed.draft)identity(g_ed.draft->Value());for(const auto& r:g_ed.file.routes)if(!g_ed.draft||r.id!=g_ed.draft->Value().name)identity(ToTd(r,EditorKey()));
    if(!add)g_ed.lanePicks.clear();
    auto found=std::find(g_ed.lanePicks.begin(),g_ed.lanePicks.end(),picked);
    if(found==g_ed.lanePicks.end())g_ed.lanePicks.push_back(picked);else if(add)g_ed.lanePicks.erase(found);
    g_ed.picks.clear();g_ed.spot=-1;g_ed.zone=-1;g_ed.link=-1;g_ed.laneScope=1;g_ed.laneFocus=picked.lane;
    if(current&&current->name==picked.route)g_ed.point=picked.section;
    Status(std::to_string(g_ed.lanePicks.size())+" lane target(s) selected. Ctrl+click adds or removes.");return true;
}
void DrawLaneVolumes(Pen& dynamicPen,const routemap::Camera& camera,const trafficroute::Line& line,unsigned color,double alpha) {
    if(!g_ui.volumeDirty)return;Pen pen(g_ui.volumes);
    struct Cached { std::uint64_t key=0;lanevolume::Mesh mesh;std::vector<std::pair<int,int>> sections; };
    static std::vector<Cached> cache;
    std::uint64_t hash=1469598103934665603ull;
    auto bytes=[&](const void* address,std::size_t count){const auto* p=static_cast<const unsigned char*>(address);for(std::size_t i=0;i<count;++i)hash=(hash^p[i])*1099511628211ull;};
    const auto config=laneplan::Write(line.plan)+splinekit::Write(line.space);bytes(config.data(),config.size());bytes(&line.closed,sizeof line.closed);bytes(&line.sourceCount,sizeof line.sourceCount);for(double u:line.authored)bytes(&u,sizeof u);
    for(int lane:line.lanes)for(const auto& p:line.lane[lane]){bytes(&p.x,sizeof p.x);bytes(&p.y,sizeof p.y);bytes(&p.z,sizeof p.z);}
    auto found=std::find_if(cache.begin(),cache.end(),[&](const Cached& c){return c.key==hash;});
    if(found==cache.end()) {
        if(cache.size()>=64)cache.erase(cache.begin());
        Cached entry;entry.key=hash;
        for(int lane:line.lanes) {
            if(trafficroute::Spatial(line)){
                // Display LOD is bounded independently of controller resolution.
                // Selected rings use the exact compiled positions and frames.
                const int budget=std::max(8,2048/std::max(1,line.space.profileSamples*int(line.lanes.size())));const int step=std::max(1,int(std::ceil(double(line.Count())/budget)));
                std::vector<lanevolume::Station> rings;std::vector<int> indices;lanevolume::Basis frames;
                // The compiled lane frames only where the route has them for every sample (2026-11-06 12:12: a route in road mode with lane
                // transforms is Spatial but trafficroute::Build returns before the frames are made; copying frame 0 of an empty list read
                // address 0, every F8 visit faulted and photo mode closed). Without them the loft makes its own transport frames.
                const bool haveFrames=lane>=0&&lane<int(line.basis.size())&&line.basis[lane].right.size()>=std::size_t(line.Count())&&line.basis[lane].up.size()>=std::size_t(line.Count());
                if(!haveFrames){static std::uint64_t loggedFor=0;if(loggedFor!=hash){loggedFor=hash;logger::Write("OBS routeedit.volumes event=no_lane_frames lane=%d samples=%d right=%zu up=%zu space=%d transforms=%zu note=loft_makes_its_own",
                    lane,line.Count(),lane>=0&&lane<int(line.basis.size())?line.basis[lane].right.size():0,lane>=0&&lane<int(line.basis.size())?line.basis[lane].up.size():0,int(line.space.enabled),line.plan.transforms.records.size());}}
                for(int i=0;i<line.Count();++i)if(i%step==0||i+1==line.Count()){auto p=trafficroute::SettingsAt(line,i,lane).profile;p.offset=0;p.elevation=-p.height*.5;const auto c=line.lane[lane][i];rings.push_back({{c.x,c.y,c.z},p});indices.push_back(i);if(haveFrames){frames.right.push_back(line.basis[lane].right[i]);frames.up.push_back(line.basis[lane].up[i]);}}
                auto mesh=lanevolume::Build(rings,line.closed,lanevolume::Frame::Transport,2,haveFrames?&frames:nullptr);if(mesh){const auto start=entry.mesh.vertices.size();entry.mesh.vertices.insert(entry.mesh.vertices.end(),mesh.vertices.begin(),mesh.vertices.end());for(std::size_t f=0;f<mesh.triangles.size();++f){const int station=indices[mesh.faceStation[f]];if(!trafficroute::Available(line,lane,station))continue;const auto t=mesh.triangles[f];entry.mesh.triangles.push_back({start+t[0],start+t[1],start+t[2]});entry.sections.push_back({int(routepolicy::Segment(line.sourceCount,line.closed,line.ToSource(station),lane)),lane});}}
                continue;
            }
            std::vector<lanevolume::Station> stations;
            int chunkSection=-1;
            auto append=[&](bool closed) {
                if(stations.size()<2){stations.clear();return;}
                auto mesh=lanevolume::Build(stations,closed,lanevolume::Frame::Transport,2);
                if(mesh){const auto start=entry.mesh.vertices.size();entry.mesh.vertices.insert(entry.mesh.vertices.end(),mesh.vertices.begin(),mesh.vertices.end());for(auto t:mesh.triangles){entry.mesh.triangles.push_back({start+t[0],start+t[1],start+t[2]});entry.sections.push_back({chunkSection,lane});}}
                stations.clear();
            };
            bool full=true;
            for(int i=0;i<line.Count();++i) {
                auto s=trafficroute::SettingsAt(line,double(i),lane);
                if(!laneplan::Active(s,lane)){full=false;append(false);continue;}
                s.profile.offset=0;s.profile.elevation=-s.profile.height*.5; // envelope centred on the preferred trajectory
                const auto p=line.lane[lane][i];
                const int section=line.sourceCount?int(routepolicy::Segment(line.sourceCount,line.closed,line.ToSource(double(i)),lane)):-1;
                if(!stations.empty()&&section!=chunkSection){stations.push_back({{p.x,p.y,p.z},s.profile});append(false);full=false;}
                chunkSection=section;
                if(!stations.empty()&&stations.back().profile.shape!=s.profile.shape) {append(false);full=false;}
                stations.push_back({{p.x,p.y,p.z},s.profile});
                // Collinear samples with a linear loft need no internal mesh
                // rings. Keep every bend and every profile/bank discontinuity.
                while(stations.size()>=3) {
                    const auto& a=stations[stations.size()-3];const auto& b=stations[stations.size()-2];const auto& c=stations.back();
                    const auto ab=lanevolume::Sub(b.centre,a.centre),ac=lanevolume::Sub(c.centre,a.centre);const double length=lanevolume::Length(ac);
                    if(length<1e-8||lanevolume::Length(routemap::Cross(ab,ac))>1e-7*length||routemap::Dot(ab,ac)<0)break;
                    const double f=lanevolume::Length(ab)/length;
                    auto linear=[&](double x,double y,double z){return std::fabs(y-(x+(z-x)*f))<1e-7;};
                    if(a.profile.shape!=b.profile.shape||b.profile.shape!=c.profile.shape||!linear(a.profile.width,b.profile.width,c.profile.width)||!linear(a.profile.height,b.profile.height,c.profile.height)||!linear(a.profile.radius,b.profile.radius,c.profile.radius)||!linear(a.profile.topWidth,b.profile.topWidth,c.profile.topWidth)||std::fabs(a.profile.bank-b.profile.bank)>1e-8||std::fabs(b.profile.bank-c.profile.bank)>1e-8)break;
                    stations.erase(stations.end()-2);
                }
            }
            if(line.closed&&!full&&!stations.empty()){auto profile=trafficroute::SettingsAt(line,0,lane).profile;profile.offset=0;profile.elevation=-profile.height*.5;const auto p=line.lane[lane][0];stations.push_back({{p.x,p.y,p.z},profile});}
            append(line.closed&&full);
        }
        cache.push_back(std::move(entry));found=std::prev(cache.end());
    }
    struct Face {std::array<std::size_t,3> v;double distance;bool selected=false;bool inside=false;};
    std::vector<Face> visible;
    const auto& mesh=found->mesh;
    for(std::size_t faceIndex=0;faceIndex<mesh.triangles.size();++faceIndex) {
        const auto t=mesh.triangles[faceIndex];
        const auto a=mesh.vertices[t[0]],b=mesh.vertices[t[1]],c=mesh.vertices[t[2]];
        const auto normal=routemap::Cross(lanevolume::Sub(b,a),lanevolume::Sub(c,a));
        const auto centre=lanevolume::Mul(lanevolume::Add(lanevolume::Add(a,b),c),1.0/3);
        // Photo view: faces turned away are the inside of the volume; drawn too (fainter) so the route stays visible
        // with the camera inside its field (user, 2026-11-04). The map view keeps its outside-only look.
        const bool inside=routemap::Dot(normal,lanevolume::Sub(camera.eye,centre))<=0;
        if(inside&&!camera.frontOnly)continue;
        const auto [section,lane]=found->sections[faceIndex];
        const bool selected=std::any_of(g_ed.lanePicks.begin(),g_ed.lanePicks.end(),[&](const LanePick& p){return p.route==line.routeName&&(p.Resolve(line.plan)==-1||p.Resolve(line.plan)==section)&&(p.lane<0||p.lane==lane);});
        const auto delta=lanevolume::Sub(centre,camera.eye);visible.push_back({t,routemap::Dot(delta,delta),selected,inside});
    }
    std::sort(visible.begin(),visible.end(),[](const Face& a,const Face& b){return a.distance>b.distance;});
    pen.NoLine();
    for(const auto& face:visible) {
        // Photo view: a triangle reaching behind the camera is cut at it, not dropped (2026-11-04 video: the road vanished).
        const auto poly=routemap::ClipPolygon(camera,{mesh.vertices[face.v[0]],mesh.vertices[face.v[1]],mesh.vertices[face.v[2]]});
        if(poly.size()<3)continue;
        std::vector<std::pair<double,double>> xy;xy.reserve(poly.size());bool valid=true;
        for(const auto& v:poly){double px=0,py=0;if(!Local(camera,v,px,py)){valid=false;break;}xy.push_back({px,py});}
        if(!valid)continue;
        const double fill=face.selected?.48:std::clamp(alpha*.28,.04,.28);
        pen.Fill(face.selected?0x72F3D2:color,face.inside?fill*.5:fill);
        pen.Move(xy[0].first,xy[0].second);for(std::size_t i=1;i<xy.size();++i)pen.Line(xy[i].first,xy[i].second);pen.Line(xy[0].first,xy[0].second);pen.End();
    }
    pen.NoLine();
}
