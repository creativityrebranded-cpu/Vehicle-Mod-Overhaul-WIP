// ---- Wire editing (F7 spec 7b): add, remove, retarget and rebuild a landing's wires; every change undoable. ----
constexpr int kWireAddIn=9690,kWireAddOut=9691,kWireBase=9700;  // + wire * 4: next route, rebuild, remove
// Builds (or rebuilds) one wire of landing `index` on `route` (empty: the nearest lane), replacing wire `replace` when >= 0.
bool BuildWire(int index,int socket,std::string route,int replace){
    if(index<0||index>=int(g_ed.file.landings.size()))return false;auto& spot=g_ed.file.landings[std::size_t(index)];
    double best=1e300;int lane=-1;double u=0;trafficroute::Line line;std::string chosen;
    for(const auto& r:g_ed.file.routes){if(!Usable(r)||(!route.empty()&&r.id!=route))continue;trafficroute::Line l;bool ring=false;if(!LineOf(r.id,l,ring)||l.error)continue;
        for(int k:trafficroute::Lanes(l,LanesOf(r.id))){const double at=trafficroute::Nearest(l,k,spot.at.x,spot.at.y);const auto q=trafficroute::At(l,k,at);const double d=std::hypot(q.x-spot.at.x,q.y-spot.at.y);if(d<best){best=d;lane=k;u=at;line=l;chosen=r.id;}}}
    if(chosen.empty()||(route.empty()&&best>250)){Status("No route in reach for this wire.");logger::Write("OBS system=traffic.place event=wire_refused place=%s reason=no_route",spot.id.c_str());return false;}
    trafficdata::Route td;for(const auto& r:g_ed.file.routes)if(r.id==chosen)td=ToTd(r,"");
    const auto end=trafficroute::At(line,lane,trafficroute::LeadU(line,lane,u,30,socket==0));const trafficdata::Vec landing{spot.at.x,spot.at.y,spot.at.z};
    const auto a=socket?landing:end,b=socket?end:landing;const auto r=RouteWire(a,b,&td);
    placeaccess::Wire w;w.socket=socket;w.route=chosen;w.lane=lane;std::vector<bool> unknown;
    if(r.ok)for(std::size_t n=socket?1:0;n+(socket?0:1)<r.points.size()&&w.path.size()<128;++n){w.path.push_back({r.points[n].x,r.points[n].y,r.points[n].z});unknown.push_back(r.points[n].unknown);}
    if(w.path.empty()){w.path.push_back(socket?placeaccess::Point{b.x,b.y,b.z}:placeaccess::Point{a.x,a.y,a.z});unknown.push_back(true);}
    PushSnap();
    spot.wires=placeaccess::Merge(spot.access,spot.wires);
    const std::size_t slot=replace>=0&&replace<int(spot.wires.size())?std::size_t(replace):spot.wires.size();
    if(slot<spot.wires.size())spot.wires[slot]=w;else spot.wires.push_back(w);
    g_wireUnknownDraw[spot.id+"#"+std::to_string(slot)]=unknown;placeaccess::Mirror(spot.access,spot.wires);
    logger::Write("OBS system=f7.autoroute event=%s place=%s socket=%s route=%s points=%zu reason=%s",r.ok?"done":"fallback",spot.id.c_str(),socket?"out":"in",chosen.c_str(),w.path.size(),r.why);
    logger::Write("OBS system=traffic.place event=wire_built place=%s route=%s socket=%s action=%s",spot.id.c_str(),chosen.c_str(),socket?"out":"in",replace>=0?"rebuild":"add");
    Status(std::string(socket?"Out":"In")+" wire "+(replace>=0?"rebuilt on ":"added to ")+chosen+". Save keeps it.");return true;
}
bool RemoveWire(int index,int wire){
    if(index<0||index>=int(g_ed.file.landings.size()))return false;auto& spot=g_ed.file.landings[std::size_t(index)];
    auto wires=placeaccess::Merge(spot.access,spot.wires);if(wire<0||wire>=int(wires.size()))return false;
    PushSnap();const auto gone=wires[std::size_t(wire)];wires.erase(wires.begin()+wire);spot.wires=wires;placeaccess::Mirror(spot.access,spot.wires);g_wireUnknownDraw.clear();
    logger::Write("OBS system=traffic.place event=wire_removed place=%s route=%s socket=%s",spot.id.c_str(),gone.route.c_str(),gone.socket?"out":"in");
    Status("Wire removed: "+gone.route+" no longer "+(gone.socket?"leaves":"arrives")+" here. Save keeps it.");return true;
}
std::string NextRoute(const std::string& current){std::vector<std::string> ids;for(const auto& r:g_ed.file.routes)if(Usable(r))ids.push_back(r.id);if(ids.empty())return {};
    const auto at=std::find(ids.begin(),ids.end(),current);return at==ids.end()||at+1==ids.end()?ids.front():*(at+1);}
bool F7WireAction(int id){
    if(g_ed.spotKind!=1||g_ed.spot<0||g_ed.spot>=int(g_ed.file.landings.size()))return false;
    if(id==kWireAddIn||id==kWireAddOut){BuildWire(g_ed.spot,id==kWireAddOut?1:0,"",-1);return true;}
    if(id<kWireBase||id>=kWireBase+64*4)return false;
    const int wire=(id-kWireBase)/4,op=(id-kWireBase)%4;const auto& spot=g_ed.file.landings[std::size_t(g_ed.spot)];
    const auto wires=placeaccess::Merge(spot.access,spot.wires);if(wire>=int(wires.size()))return true;const auto w=wires[std::size_t(wire)];
    if(op==0)BuildWire(g_ed.spot,w.socket,NextRoute(w.route),wire);else if(op==1)BuildWire(g_ed.spot,w.socket,w.route,wire);else RemoveWire(g_ed.spot,wire);
    return true;
}
// The landing's wires in the side panel position (one row each), only while no other side panel is open.
void F7WirePanel(){
    if(g_ed.spotKind!=1||g_ed.spot<0||g_ed.spot>=int(g_ed.file.landings.size())||g_contextPanel||g_roadPanel)return;
    const auto& spot=g_ed.file.landings[std::size_t(g_ed.spot)];const auto wires=placeaccess::Merge(spot.access,spot.wires);
    const double x=PanelX(),y=12,w=kPanelW;const int rows=std::min<int>(int(wires.size()),std::max(0,int((g_ui.height-260)/64)));const double h=std::max(150.0,96+rows*64.0);
    const editorcontext::Rect r{x,y,w,h};g_contextRects.push_back(r);g_ui.boxes.insert(g_ui.boxes.begin(),{x,y,w,h,0x141C29,.97,0x3A4658,1});g_f7AreaRect[2]=r;
    g_panelText.push_back({"Landing wires - "+spot.id,x+12,y+8,w-24,kTeal,kUnit});
    for(int i=0;i<rows;++i){const auto& one=wires[std::size_t(i)];const double ry=y+36+i*64;
        g_panelText.push_back({f7layers::Fit(std::string(one.socket?"Out":"In")+" - "+one.route+(one.lane>=0?" lane "+std::to_string(one.lane+1):""),w-24,f7layers::kLetterPx),x+12,ry,w-24,kText,kUnit});
        const char* ops[]{"Next route","Rebuild","Remove"};for(int k=0;k<3;++k)g_ui.buttons.push_back({kWireBase+i*4+k,x+12+k*118,ry+22,112,28,ops[k],false,true,"",0});}
    if(int(wires.size())>rows)g_panelText.push_back({std::to_string(int(wires.size())-rows)+" more wire(s) not shown",x+12,y+h-62,w-24,kDim,kUnit});
    g_ui.buttons.push_back({kWireAddIn,x+12,y+h-36,170,28,"Add In wire",false,true,"",0});g_ui.buttons.push_back({kWireAddOut,x+w-182,y+h-36,170,28,"Add Out wire",false,true,"",0});
    if(wires.empty())g_panelText.push_back({"Not connected. Add a wire to join a route.",x+12,y+40,w-24,kAmber,kUnit});
}

// ---- Node view (F7 spec 7c): the network over the dimmed map, each node on its real location. ----
struct NodeHit{int kind=0,index=-1;double x=0,y=0,ax=0,ay=0;trafficdata::Vec at{};std::string key;};  // x,y shown (may be dragged); ax,ay anchor  // kind 1 route (midpoint), 2 landing, 3 junction link, 4 zone
int g_nodeDragLanding=-1,g_nodeDragSocket=-1,g_nodeWireLanding=-1,g_nodeWire=-1;double g_nodeDragX=0,g_nodeDragY=0;
// Display-only node offsets (screen px), saved in f7layout.ini; world geometry never moves.
auto& g_nodeOffset=g_f7NodeOffsets();std::vector<NodeHit> g_nodePicks;
std::string g_nodeMoving;double g_nodeMoveX=0,g_nodeMoveY=0;std::pair<double,double> g_nodeMoveFrom;ULONGLONG g_nodeLastClick=0;std::string g_nodeLastKey;
std::vector<NodeHit> NodeLayout(const routemap::Camera& c){
    std::vector<NodeHit> out;
    auto push=[&](int kind,int index,trafficdata::Vec at,const std::string& key){double x,y;if(!Local(c,{at.x,at.y,at.z},x,y))return;NodeHit h{kind,index,x,y,x,y,at,key};
        if(const auto o=g_nodeOffset.find(key);o!=g_nodeOffset.end()){h.x+=o->second.first;h.y+=o->second.second;}out.push_back(h);};
    for(std::size_t i=0;i<g_ed.file.routes.size();++i){const auto& r=g_ed.file.routes[i];if(!Usable(r))continue;trafficroute::Line l;bool ring=false;if(!LineOf(r.id,l,ring)||l.error||l.Count()<2)continue;
        const auto p=trafficroute::At(l,0,l.Span()*.5);push(1,int(i),p,"route:"+r.id);}
    for(std::size_t i=0;i<g_ed.file.landings.size();++i){const auto& s=g_ed.file.landings[i];push(2,int(i),{s.at.x,s.at.y,s.at.z},"landing:"+s.id);}
    for(std::size_t i=0;i<g_ed.file.links.size();++i){const auto& k=g_ed.file.links[i];trafficroute::Line l;bool ring=false;if(!LineOf(k.from,l,ring)||l.error||l.Count()<2)continue;
        const auto p=trafficroute::At(l,0,ring?k.at:l.ToLine(k.at));push(3,int(i),p,"link:"+k.id);}
    for(std::size_t i=0;i<g_ed.file.zones.size();++i){const auto& z=g_ed.file.zones[i];push(4,int(i),z.at,"zone:"+z.name);}
    return out;
}
const NodeHit* RouteNode(const std::vector<NodeHit>& n,const std::string& id){for(const auto& h:n)if(h.kind==1&&g_ed.file.routes[std::size_t(h.index)].id==id)return &h;return nullptr;}
void DrawNodeView(Pen& pen,const routemap::Camera& c,uptr movie){
    pen.NoLine();pen.Fill(0x05080C,.55);pen.Rect(0,0,g_ui.width,g_ui.height);pen.End();
    const auto nodes=NodeLayout(c);
    auto wire=[&](double ax,double ay,double bx,double by,unsigned col,double alpha,double width){pen.Style(width,col,alpha);pen.Move(ax,ay);const double mx=(ax+bx)*.5;pen.Line(mx,ay);pen.Line(mx,by);pen.Line(bx,by);pen.NoLine();};
    for(const auto& k:g_ed.file.links){const auto* a=RouteNode(nodes,k.from);const auto* b=RouteNode(nodes,k.to);const NodeHit* j=nullptr;for(const auto& h:nodes)if(h.kind==3&&g_ed.file.links[std::size_t(h.index)].id==k.id)j=&h;
        if(a&&j)wire(a->x,a->y,j->x,j->y,0x9FD7FF,.8,2);if(j&&b)wire(j->x,j->y,b->x,b->y,0x9FD7FF,.8,2);}
    for(const auto& h:nodes)if(h.kind==2){const auto& s=g_ed.file.landings[std::size_t(h.index)];const auto wires=placeaccess::Merge(s.access,s.wires);
        for(std::size_t n=0;n<wires.size();++n){const auto* r=RouteNode(nodes,wires[n].route);if(!r)continue;const bool sel=g_nodeWireLanding==h.index&&g_nodeWire==int(n);
            if(wires[n].socket==0)wire(r->x,r->y,h.x-26,h.y,sel?0xFFE15A:0x7CFF8A,1,sel?4:2);else wire(h.x+26,h.y,r->x,r->y,sel?0xFFE15A:0xFFD27C,1,sel?4:2);}}
    for(const auto& h:nodes)if(std::hypot(h.x-h.ax,h.y-h.ay)>2){pen.Style(1,0xFFFFFF,.45);pen.Move(h.ax,h.ay);pen.Line(h.x,h.y);pen.NoLine();pen.Fill(0xFFFFFF,.6);pen.Circle(h.ax,h.ay,3);pen.End();}
    for(std::size_t n=0;n<g_nodePicks.size();++n)for(const auto& h:nodes)if(h.key==g_nodePicks[n].key){pen.Style(2,0xFFE15A,1);pen.Circle(h.x,h.y,24);pen.NoLine();Text(movie,std::to_string(n+1),h.x+20,h.y-30,0xFFE15A,20,true);}
    for(const auto& h:nodes){
        if(h.kind==1){const auto& r=g_ed.file.routes[std::size_t(h.index)];pen.Style(1,0x9FD7FF,1);pen.Fill(0x1B2A36,.95);pen.RoundRect(h.x-60,h.y-14,120,28,12);pen.End();pen.NoLine();
            Text(movie,f7layers::Fit(r.label.empty()?r.id:r.label,104,f7layers::kLetterPx),h.x-52,h.y-9,0xDFE7EE,104,true);}
        else if(h.kind==2){const auto& s=g_ed.file.landings[std::size_t(h.index)];const bool sel=g_ed.spotKind==1&&g_ed.spot==h.index;pen.Style(sel?2:1,sel?0x6FE0D0:0xFFFFFF,1);pen.Fill(0x2A2318,.95);pen.RoundRect(h.x-22,h.y-14,44,28,12);pen.End();
            pen.NoLine();pen.Fill(0x7CFF8A,1);pen.Circle(h.x-26,h.y,5);pen.End();pen.Fill(0xFFD27C,1);pen.Circle(h.x+26,h.y,5);pen.End();Text(movie,f7layers::Fit(s.id,140,f7layers::kLetterPx),h.x-22,h.y+16,0xFFD27C,140,true);}
        else if(h.kind==3){pen.NoLine();pen.Fill(0x9FD7FF,1);pen.Circle(h.x,h.y,6);pen.End();}
        else {pen.Style(1,0xF2B04E,.9);pen.Fill(0x2A2216,.85);pen.RoundRect(h.x-14,h.y-10,28,20,8);pen.End();pen.NoLine();}}
    if(g_nodeDragLanding>=0)for(const auto& h:nodes)if(h.kind==2&&h.index==g_nodeDragLanding){pen.Style(2,0xFFE15A,1);pen.Move(h.x+(g_nodeDragSocket?26:-26),h.y);pen.Line(g_nodeDragX,g_nodeDragY);pen.NoLine();}
}
// A draft route through world stops in order, each leg routed in 3D round known obstacles (straight where unknown).
bool MakeRouteThrough(const std::vector<trafficdata::Vec>& stops,const char* why){
    if(stops.size()<2){Status("Pick at least two nodes (Ctrl+click) first.");return false;}
    if(g_ed.draft){Status("Save or Cancel the open route first.");return false;}
    std::vector<trafficdata::Vec> pts{stops.front()};int straight=0;
    for(std::size_t i=0;i+1<stops.size();++i){const auto r=RouteWire(stops[i],stops[i+1],nullptr);
        if(r.ok)for(std::size_t n=1;n<r.points.size();++n)pts.push_back({r.points[n].x,r.points[n].y,r.points[n].z});else{pts.push_back(stops[i+1]);++straight;}}
    NewRoute();if(!g_ed.draft)return false;
    auto t=g_ed.draft->Value();t.points.clear();for(const auto& q:pts)t.points.push_back({q.x,q.y,q.z});t.closed=false;g_ed.draft->Edit(t);g_ed.naming=false;
    logger::Write("OBS system=f7.node event=make_route kind=%s stops=%zu points=%zu straight_legs=%d",why,stops.size(),pts.size(),straight);
    Status("Draft route through "+std::to_string(stops.size())+" nodes"+(straight?" ("+std::to_string(straight)+" leg(s) straight: red where unknown)":"")+". Name it, edit it, Save keeps it.");return true;
}
bool MakeRouteFromPicks(){std::vector<trafficdata::Vec> stops;for(const auto& h:g_nodePicks)stops.push_back(h.at);const bool done=MakeRouteThrough(stops,"picked");if(done)g_nodePicks.clear();return done;}
// Clicks in Node view; true when it used the mouse this frame.
bool NodeViewMouse(const routemap::Camera& c,double lx,double ly,bool click,bool down,bool release,bool ctrl){
    const auto nodes=NodeLayout(c);g_nodeDragX=lx;g_nodeDragY=ly;
    auto body=[&](const NodeHit& h){const double w=h.kind==1?62:h.kind==2?24:16;return std::fabs(lx-h.x)<w&&std::fabs(ly-h.y)<16;};
    if(click){
        for(const auto& h:nodes)if(h.kind==2)for(int socket:{0,1}){const double sx=h.x+(socket?26:-26);if(std::hypot(lx-sx,ly-h.y)<9){g_nodeDragLanding=h.index;g_nodeDragSocket=socket;return true;}}
        for(const auto& h:nodes)if(h.kind==2){const auto& s=g_ed.file.landings[std::size_t(h.index)];const auto wires=placeaccess::Merge(s.access,s.wires);
            for(std::size_t n=0;n<wires.size();++n){const auto* r=RouteNode(nodes,wires[n].route);if(!r)continue;const double ax=wires[n].socket?h.x+26:r->x,bx=wires[n].socket?r->x:h.x-26,mx=(ax+bx)*.5;
                if(std::fabs(lx-mx)<7&&ly>=std::min(h.y,r->y)-4&&ly<=std::max(h.y,r->y)+4){g_nodeWireLanding=h.index;g_nodeWire=int(n);Status("Wire selected: Del removes it.");return true;}}}
        for(const auto& h:nodes)if(body(h)){
            if(ctrl){const auto at=std::find_if(g_nodePicks.begin(),g_nodePicks.end(),[&](const NodeHit& p){return p.key==h.key;});if(at!=g_nodePicks.end())g_nodePicks.erase(at);else g_nodePicks.push_back(h);
                Status(std::to_string(g_nodePicks.size())+" node(s) picked in order. Enter makes a route through them; Esc clears.");return true;}
            const auto now=GetTickCount64();if(g_nodeLastKey==h.key&&now-g_nodeLastClick<400){g_nodeOffset.erase(h.key);SaveF7Layout();g_nodeLastKey.clear();return true;}  // double-click: back to its place
            g_nodeLastKey=h.key;g_nodeLastClick=now;g_nodeMoving=h.key;g_nodeMoveX=lx;g_nodeMoveY=ly;const auto o=g_nodeOffset.find(h.key);g_nodeMoveFrom=o==g_nodeOffset.end()?std::pair<double,double>{0,0}:o->second;
            if(h.kind==2){g_ed.spotKind=1;g_ed.spot=h.index;g_ed.point=-1;}else if(h.kind==1)SelectRow(h.index);g_nodeWire=-1;return true;}
        g_nodeWire=-1;return true;}
    if(!g_nodeMoving.empty()&&down){if(std::hypot(lx-g_nodeMoveX,ly-g_nodeMoveY)>4)g_nodeOffset[g_nodeMoving]={g_nodeMoveFrom.first+lx-g_nodeMoveX,g_nodeMoveFrom.second+ly-g_nodeMoveY};return true;}
    if(!g_nodeMoving.empty()&&(release||!down)){if(g_nodeOffset.count(g_nodeMoving)){SaveF7Layout();logger::Write("OBS system=f7.node event=move key=%s",g_nodeMoving.c_str());}g_nodeMoving.clear();return true;}
    if(release&&g_nodeDragLanding>=0){
        for(const auto& h:nodes){
            if(h.kind==1&&body(h)){const auto route=g_ed.file.routes[std::size_t(h.index)].id;
                logger::Write("OBS system=f7.node event=wire kind=landing_%s route=%s place=%s",g_nodeDragSocket?"out":"in",route.c_str(),g_ed.file.landings[std::size_t(g_nodeDragLanding)].id.c_str());
                BuildWire(g_nodeDragLanding,g_nodeDragSocket,route,-1);break;}
            if(h.kind==2&&h.index!=g_nodeDragLanding&&body(h)){  // landing to landing: a wire between nodes becomes a real draft route
                const auto from=g_ed.file.landings[std::size_t(g_nodeDragLanding)].at,to=g_ed.file.landings[std::size_t(h.index)].at;
                const trafficdata::Vec a{from.x,from.y,from.z},b{to.x,to.y,to.z};MakeRouteThrough(g_nodeDragSocket?std::vector<trafficdata::Vec>{a,b}:std::vector<trafficdata::Vec>{b,a},"landing_to_landing");break;}}
        g_nodeDragLanding=-1;return true;}
    return down&&g_nodeDragLanding>=0;
}
bool NodeViewDelete(){if(g_nodeWireLanding<0||g_nodeWire<0)return false;const bool done=RemoveWire(g_nodeWireLanding,g_nodeWire);logger::Write("OBS system=f7.node event=unwire place=%d wire=%d",g_nodeWireLanding,g_nodeWire);g_nodeWire=-1;return done;}
// Card R2/R3 (notes/f7-restore-20261108): what the F7 dock's Nodes and Wires menus read and press. Same state and actions as above; the
// dock only shows them in the bottom bar. F7WireButton maps a dock row to this file's wire ids (kWireAddIn/Out share 9690/9691 with
// kRoadSnap/kRoadFit, which Press handles first; the dock calls F7WireAction directly, so the two never meet).
int F7WireButton(int wire,int op){return op<0?(wire?kWireAddOut:kWireAddIn):kWireBase+wire*4+op;}
int NodePickCount(){return int(g_nodePicks.size());}
void NodePicksClear(){g_nodePicks.clear();}
std::string NodeWireText(){
    if(g_nodeWireLanding<0||g_nodeWire<0||g_nodeWireLanding>=int(g_ed.file.landings.size()))return {};
    const auto& s=g_ed.file.landings[std::size_t(g_nodeWireLanding)];const auto wires=placeaccess::Merge(s.access,s.wires);if(g_nodeWire>=int(wires.size()))return {};
    const auto& w=wires[std::size_t(g_nodeWire)];return std::string(w.socket?"Out: ":"In: ")+s.id+(w.socket?" to ":" from ")+RouteLabel(w.route)+(w.lane>=0?" lane "+std::to_string(w.lane+1):"");
}
