#include "routeeditor.h"
#include <cstdio>
#include <limits>

int main() {
    using namespace routeeditor;
    int checks=0, failures=0;
    auto check=[&](bool ok,const char* name){ ++checks; if(!ok){++failures;std::printf("FAIL %s\n",name);} };
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
    std::printf("routeeditor: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
