#include "routeeditor.h"
#include <cmath>
#include <cstdio>
#include <limits>

int main() {
    using namespace routeeditor;
    int checks=0, failures=0;
    auto check=[&](bool ok,const char* name){ ++checks; if(!ok){++failures;std::printf("FAIL %s\n",name);} };
    check(routeeditor::PlacedHeight(167,-248,2)==167&&routeeditor::PlacedHeight(10,20,2)==22&&routeeditor::PlacedHeight(22,20,2)==22,"a placed point is never lowered to the ground, only lifted out of it");
    check(NextLevel(60,1,25)==75&&NextLevel(60,-1,25)==50&&NextLevel(75,1,25)==100&&NextLevel(75,-1,25)==50&&NextLevel(-10,-1,25)==-25,"a controller's height steps land on levels");
    check(F7(Context::SurfaceMap)==F7Action::RouteEditor,"F7 selects route editor in surface map");
    check(F7(Context::Gameplay)==F7Action::HudEditor,"F7 preserves existing HUD editor during gameplay");
    check(F7(Context::OtherMap)==F7Action::None && F7(Context::OtherMenu)==F7Action::None,"other maps and menus are not hijacked");
    Route source; source.name="city"; source.world="jemison"; source.ring=false; source.closed=true; source.points={{0,0,100},{100,0,200},{100,100,300}};
    Draft d(source);
    check(d.Check()==Issue::None,"valid 3D closed route");
    check(d.SetZ(1,250) && d.Value().points[1].z==250 && d.Value().points[0].z==100 && d.Value().points[2].z==300,"Z edit changes only selected point");
    check(source.points[1].z==200,"draft cannot mutate original");
    check(d.Undo() && d.Value().points[1].z==200,"undo height");
    check(d.Redo() && d.Value().points[1].z==250,"redo height");
    check(d.Reverse() && d.Value().points.front().z==300 && d.Value().points.back().z==100,"reverse changes direction with XYZ attached");
    check(d.Undo() && d.Value().points.front().z==100,"undo reverse");
    check(d.Remove(1) && d.Check()==Issue::TooFewPoints,"closed route needs three points");
    check(d.Undo() && d.Check()==Issue::None,"undo removal restores route");
    check(d.Clear() && d.Value().points.empty() && d.Undo() && d.Value().points.size()==3,"clear is undoable");
    check(d.Move(1,{0,0,100}) && d.Check()==Issue::DuplicatePoints,"adjacent coincident points rejected");
    check(d.Undo(),"undo duplicate");
    const auto nan=std::numeric_limits<double>::quiet_NaN();
    check(!d.Add({nan,1,2}) && !d.SetZ(0,nan) && !d.Move(99,{0,0,0}),"invalid position and index rejected without change");
    check(d.SetZ(0,175) && !d.Redo(),"new edit invalidates redo history");
    d.Close(false); check(!d.Value().closed && d.Undo() && d.Value().closed,"open/close is undoable");
    Route broken=source; broken.points.back()=broken.points.front();
    check(Validate(broken)==Issue::DuplicatePoints,"closed loop rejects redundant zero-length closing edge");
    broken=source; broken.speed=nan; check(Validate(broken)==Issue::InvalidSettings,"invalid speed rejected");
    broken=source; broken.world.clear(); check(Validate(broken)==Issue::MissingWorld,"missing world rejected");
    broken=source; broken.points[0].z=nan; check(Validate(broken)==Issue::InvalidPoint,"non-finite point rejected");
    {
        Route r=source;r.closed=false;r.points={{0,0,0},{100,0,10},{100,100,20}};Draft rounded(r);
        check(rounded.Round(1,20)&&rounded.Value().rounding[1]==20,"selected corner radius is an ordinary undoable edit");
        check(rounded.Undo()&&rounded.Value().rounding.empty()&&rounded.Redo(),"undo/redo preserves rounding metadata");
        check(!rounded.Round(0,20)&&!rounded.Round(1,201)&&!rounded.Round(1,nan),"endpoints and invalid radii cannot become broken curves");
        rounded.Reverse();check(rounded.Value().rounding[1]==20,"reverse keeps radius attached to its authored point");
        rounded.Remove(1);check(rounded.Value().rounding.size()==2&&rounded.Undo()&&rounded.Value().rounding[1]==20,"delete/undo keeps radius arrays aligned");
    }
    {   // the whole route at once (stage 2's Route tool)
        auto near=[](double a,double b){return std::fabs(a-b)<1e-6;};
        Route r=source; r.points={{0,0,10},{100,0,20},{100,100,30},{0,100,40}};
        const auto c=Centre(r);
        check(near(c.x,50)&&near(c.y,50)&&near(c.z,25),"the centre is the average of the points");
        const auto moved=Translated(r,10,-5,3);
        check(near(moved.points[0].x,10)&&near(moved.points[0].y,-5)&&near(moved.points[0].z,13)&&near(moved.points[3].z,43),"moving keeps every point's place relative to the others");
        const auto turned=Turned(r,3.14159265358979323846/2);
        check(near(turned.points[0].x,100)&&near(turned.points[0].y,0)&&near(turned.points[1].x,100)&&near(turned.points[1].y,100),"a quarter turn about the centre (counter-clockwise from above)");
        check(near(turned.points[2].z,30)&&near(Centre(turned).x,50)&&near(Centre(turned).y,50),"turning keeps heights and the centre");
        const auto back=Turned(Turned(r,0.7),-0.7);
        check(near(back.points[1].x,100)&&near(back.points[1].y,0),"turning back undoes a turn");
        const auto flipped=Mirrored(r,0,1);
        check(near(flipped.points[0].x,100)&&near(flipped.points[1].x,0)&&near(flipped.points[0].y,0)&&near(flipped.points[0].z,10),"mirroring across a north line swaps east and west, keeps order and height");
        const auto hr=HeightRange(r);
        check(near(hr.first,10)&&near(hr.second,40),"the height range is the lowest and highest point");
        Route ring=source; ring.ring=true; ring.points.clear(); ring.shape.cx=5; ring.shape.cy=6; ring.shape.z=70; ring.shape.radius=100;
        const auto rm=Translated(ring,1,2,3);
        check(near(rm.shape.cx,6)&&near(rm.shape.cy,8)&&near(rm.shape.z,73)&&near(rm.shape.radius,100),"a ring moves and rises by its centre");
        check(near(Turned(ring,1).shape.cx,5)&&near(Mirrored(ring,1,0).shape.cy,6)&&near(HeightRange(ring).first,70),"turning or mirroring a ring leaves it as it was");
        check(Turned(r,std::numeric_limits<double>::quiet_NaN()).points[1].x==100,"a bad angle changes nothing");
    }
    {   // the Shape tool and parallel lanes
        const Point c{100,200,50};
        const auto circle=ShapePoints(Shape::Circle,c,80,0);
        check(circle.size()==12&&std::fabs(Distance(circle[3],c)-80)<1e-6&&circle[5].z==50,"a circle: 12 points at the radius and height");
        const auto ellipse=ShapePoints(Shape::Ellipse,c,100,0);
        check(std::fabs(ellipse[0].x-200)<1e-6&&std::fabs(ellipse[3].y-260)<1e-6,"an ellipse is narrower across");
        std::vector<double> corners;
        const auto rect=ShapePoints(Shape::Rectangle,c,100,0,&corners);
        check(rect.size()==4&&corners.size()==4&&corners[0]>0,"a rectangle: four rounded corners");
        const auto helix=ShapePoints(Shape::Helix,c,50,0);
        check(helix.size()==25&&std::fabs(helix.back().z-110)<1e-6,"a helix rises 30 m a turn for two turns");
        const auto turned=ShapePoints(Shape::Rectangle,c,100,1.5707963267948966);
        check(std::fabs(turned[0].x-(100-60))<1e-6&&std::fabs(turned[0].y-(200+100))<1e-6,"a shape turns about its centre");
        const auto lane=Parallel({{0,0,5},{100,0,5},{200,0,5}},false,20);
        check(std::fabs(lane[1].y+20)<1e-6&&lane[1].x==100&&lane[1].z==5,"a parallel lane sits to the right of travel");
    }
    {   // disconnect and merge
        auto line=[](const char* name,double x0,bool closed=false){Route r; r.ring=false; r.name=name; r.world="w"; r.closed=closed; r.points={{x0,0,5},{x0+10,0,5},{x0+20,0,5},{x0+30,0,5},{x0+40,0,5}}; return r;};
        auto one=CutRoute(line("a",0),{2});
        check(one.pieces.size()==2&&one.pieces[0].points.size()==3&&one.pieces[1].points.size()==3&&one.pieces[1].points[0].x==20,"one point: two routes that share it");
        check(one.map[1]==std::make_pair(0,1)&&one.map[4]==std::make_pair(1,2)&&one.map[2]==std::make_pair(0,2),"each old point knows where it went");
        auto gap=CutRoute(line("a",0),{2,3});
        check(gap.pieces.size()==2&&gap.pieces[0].points.back().x==20&&gap.pieces[1].points.front().x==30,"two points side by side: the stretch between them goes");
        auto twice=CutRoute(line("a",0),{1,3});
        check(twice.pieces.size()==3&&twice.pieces[1].points.size()==3,"cut twice: three routes");
        auto tail=CutRoute(line("a",0),{0,1});
        check(tail.pieces.size()==1&&tail.pieces[0].points.front().x==10&&tail.map[0].first==-1,"the first stretch cut off: its lone point goes");
        auto end=CutRoute(line("a",0),{4});
        check(end.pieces.size()==1&&end.pieces[0].points.size()==5,"a route's own end changes nothing");
        auto loop=CutRoute(line("a",0,true),{2});
        check(loop.pieces.size()==1&&!loop.pieces[0].closed&&loop.pieces[0].points.front().x==20&&loop.pieces[0].points.back().x==20&&loop.pieces[0].points.size()==6,"a loop opens at the point");
        auto looped=CutRoute(line("a",0,true),{1,3});
        check(looped.pieces.size()==2,"a loop cut twice: two routes");
        auto rounded=line("a",0); rounded.rounding={0,9,9,9,0};
        auto kept=CutRoute(rounded,{2});
        check(kept.pieces[0].rounding.size()==3&&kept.pieces[0].rounding[1]==9&&kept.pieces[0].rounding[2]==0,"corners stay, new ends are sharp");
        auto a=line("a",0), b=line("b",100); b.speed=70; a.speed=20;
        auto m=MergeRoutes({a,b});
        check(m.route.points.size()==10&&m.route.name=="a"&&m.route.speed==20&&!m.route.closed,"the first route's settings; every point");
        check(m.route.points[4].x==40&&m.route.points[5].x==100&&m.map[1][0]==5&&m.map[0][4]==4,"nearest ends joined however far apart");
        auto back=b; std::reverse(back.points.begin(),back.points.end());
        auto r2=MergeRoutes({a,back});
        check(r2.route.points[5].x==100&&r2.map[1][4]==5&&r2.map[1][0]==9,"a route running the other way is turned round");
        auto before=line("b",-100);
        auto r3=MergeRoutes({a,before});
        check(r3.route.points.front().x==-100&&r3.route.points.back().x==40&&r3.map[0][0]==5,"a route that belongs in front goes in front");
        auto touching=line("b",40);
        auto r4=MergeRoutes({a,touching});
        check(r4.route.points.size()==9&&r4.map[1][0]==4&&r4.map[0][4]==4,"two ends in one place become one point");
        auto r5=MergeRoutes({a,line("b",100),line("c",200)});
        check(r5.route.points.size()==15&&r5.route.points[10].x==200,"three in a chain");
        auto r6=MergeRoutes({a,line("b",100,true)});
        check(r6.route.points.size()==10&&r6.route.points[5].x==100,"a loop opens at its nearest point");
        auto r7=MergeRoutes({line("a",0,true),line("b",100)});
        check(r7.route.points.size()==10&&r7.route.points[4].x==40&&r7.route.points[5].x==100,"so does the first route if it is a loop");
    }
    {   // several things at once
        GroupMove m; double x=110,y=200,z=5;
        m.dx=1; m.dy=2; m.dz=3; Apply(m,x,y,z);
        check(x==111&&y==202&&z==8,"a group move adds the same to each");
        GroupMove s; s.cx=100; s.cy=200; s.factor=2; x=110; y=200; z=5; Apply(s,x,y,z);
        check(x==120&&y==200&&z==5,"a group sizes about its middle and keeps heights");
        GroupMove t; t.cx=100; t.cy=200; t.radians=1.5707963267948966; x=110; y=200; z=5; Apply(t,x,y,z);
        check(std::fabs(x-100)<1e-9&&std::fabs(y-210)<1e-9&&z==5,"a group turns about its middle");
        Route r; r.ring=false; r.name="a"; r.world="w"; r.closed=false; r.points={{0,0,0},{10,0,0},{20,0,0},{30,0,0}}; r.rounding={0,5,6,0};
        const auto left=WithoutPoints(r,{1,2});
        check(left.points.size()==2&&left.points[1].x==30&&left.rounding.size()==2&&left.rounding[1]==0,"points go with their corners");
        Draft d(r); auto n=d.Value(); const auto base=d.Depth();
        n.speed=30; d.Edit(n); n.speed=35; d.Edit(n); n.speed=40; d.Edit(n);
        d.CollapseTo(base);
        check(d.Depth()==base+1&&d.Value().speed==40&&d.Undo()&&d.Value().speed==r.speed&&d.Depth()==base,"one change to many is one undo step");
    }
    {   // resizing: any route, and a shape's own values read back from its points
        const Point c{100,200,50};
        auto made=[&](Shape s,double radius,double angle,bool closed){Route r; r.ring=false; r.name="a"; r.world="w"; r.closed=closed; r.points=ShapePoints(s,c,radius,angle,&r.rounding); return r;};
        Route any; any.ring=false; any.name="a"; any.world="w"; any.closed=false; any.points={{0,0,5},{100,0,9},{100,40,12}};
        const auto bigger=Scaled(any,2);
        const auto centre=Centre(any);
        check(std::fabs(bigger.points[1].x-(centre.x+(100-centre.x)*2))<1e-6&&bigger.points[1].z==9,"Size scales from the centre and keeps heights");
        check(std::fabs(Across(bigger)-2*Across(any))<1e-6,"the width follows the scale");
        check(std::fabs(Scaled(Scaled(any,1.1),1/1.1).points[0].x-any.points[0].x)<1e-6,"up then down comes back");
        check(!ReadShape(any).known,"a hand-drawn route is not read as a shape");
        Route ring; ring.ring=true; ring.shape.radius=100;
        check(Scaled(ring,1.5).shape.radius==150,"a ring scales by its radius");
        auto helix=made(Shape::Helix,80,0.7,false);
        auto v=ReadShape(helix);
        check(v.known&&v.kind==Shape::Helix&&std::fabs(v.a-80)<1e-6&&std::fabs(v.turns-2)<1e-6&&std::fabs(v.climb-30)<1e-6,"a helix reads back its radius, turns and climb");
        v=ReadShape(Turned(Translated(helix,300,-50,20),1.1));
        check(v.known&&std::fabs(v.a-80)<1e-6&&std::fabs(v.climb-30)<1e-6,"moving, raising and turning keep the values");
        v=ReadShape(helix);
        auto tight=WithHelix(helix,v,40,3.5,10);
        auto w=ReadShape(tight);
        check(tight.points.size()==43&&w.known&&std::fabs(w.a-40)<1e-6&&std::fabs(w.turns-3.5)<1e-6&&std::fabs(w.climb-10)<1e-6,"a helix is laid again from its values");
        check(std::fabs(tight.points[0].x-(v.centre.x+40*std::cos(v.angle)))<1e-6&&tight.points[0].z==helix.points[0].z,"from the same start and height");
        auto back=helix; std::reverse(back.points.begin(),back.points.end());
        w=ReadShape(back);
        check(w.known&&w.climb<0&&w.dir==-v.dir,"a reversed helix runs downhill, the other way round");
        auto bent=helix; bent.points[5].x+=20;
        check(!ReadShape(bent).known,"a point moved by hand: it is just a route");
        check(ReadShape(Scaled(helix,1.5)).known&&std::fabs(ReadShape(Scaled(helix,1.5)).a-120)<1e-3,"Size changes a helix's radius value");
        auto circle=made(Shape::Circle,80,0.3,true);
        v=ReadShape(circle);
        check(v.known&&v.kind==Shape::Circle&&std::fabs(v.a-80)<1e-6,"a circle reads back its radius");
        w=ReadShape(WithSides(circle,v,120,120));
        check(w.known&&w.kind==Shape::Circle&&std::fabs(w.a-120)<1e-6,"a circle takes a new radius");
        auto ellipse=made(Shape::Ellipse,100,1.0,true);
        v=ReadShape(ellipse);
        check(v.known&&v.kind==Shape::Ellipse&&std::fabs(v.a-100)<1e-6&&std::fabs(v.b-60)<1e-6,"an ellipse reads back both radii");
        w=ReadShape(WithSides(ellipse,v,150,40));
        check(w.known&&w.kind==Shape::Ellipse&&std::fabs(w.a-150)<1e-6&&std::fabs(w.b-40)<1e-6,"an ellipse takes new radii");
        auto rev=ellipse; std::reverse(rev.points.begin(),rev.points.end());
        check(ReadShape(rev).known,"a reversed ellipse is still one");
        auto rect=made(Shape::Rectangle,100,0.4,true);
        v=ReadShape(rect);
        check(v.known&&v.kind==Shape::Rectangle&&std::fabs(v.a-100)<1e-6&&std::fabs(v.b-60)<1e-6,"a rectangle reads back its sides");
        auto wide=WithSides(rect,v,200,60);
        w=ReadShape(wide);
        check(w.known&&std::fabs(w.a-200)<1e-6&&std::fabs(w.b-60)<1e-6&&wide.rounding==rect.rounding,"one side changes alone; corners kept while the other side is unchanged");
        auto slanted=rect; slanted.points[0].x+=30;
        check(!ReadShape(slanted).known,"not square: not a rectangle");
    }
    {   // the Alt key hub
        check(HubTap(false)==1&&HubTap(true)==0.25,"a tap is one exact step");
        check(HubSpeed(0.1,false)==0,"no glide until the key is held");
        check(HubSpeed(0.25,false)==8&&HubSpeed(5,false)==40,"a held key speeds up to its limit");
        check(HubSpeed(5,true)==1.5,"Ctrl keeps it slow");
        Route r; r.name="a"; r.world="w"; r.points={{0,0,0},{100,0,0}};
        Draft d(r); auto n=d.Value(); n.points[0].x=1; d.Edit(n); n.points[0].x=2; d.Amend(n); n.points[0].x=3; d.Amend(n);
        check(d.Value().points[0].x==3&&d.Undo()&&d.Value().points[0].x==0,"one held key is one undo step");
    }
    std::printf("routeeditor: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
