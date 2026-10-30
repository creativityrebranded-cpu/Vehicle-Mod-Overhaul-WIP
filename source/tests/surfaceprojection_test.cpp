#include "surfaceprojection.h"
#include <cmath>
#include <cstdio>
#include <limits>
int main() {
    using namespace surfaceprojection;
    int checks=0,failures=0;
    auto check=[&](bool ok,const char* name){++checks;if(!ok){++failures;std::printf("FAIL %s\n",name);}};
    const std::array<double,16> identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    Vec2 screen; double depth=0;
    check(Project(identity,{0,1920,0,1080},{0,0,.5},screen,depth)&&screen.x==960&&screen.y==540&&depth==.5,"centre and depth");
    check(Project(identity,{0,1920,0,1080},{-1,1,0},screen,depth)&&screen.x==0&&screen.y==0,"top left viewport mapping");
    check(Project(identity,{100,500,200,400},{1,-1,0},screen,depth)&&screen.x==500&&screen.y==400,"offset viewport mapping");
    auto perspective=identity;perspective[14]=1;perspective[15]=1;
    check(Project(perspective,{0,2,0,2},{1,0,1},screen,depth)&&std::fabs(screen.x-1.5)<1e-9,"homogeneous divide");
    check(!Project(perspective,{0,2,0,2},{1,0,-2},screen,depth),"behind-camera point rejected");
    check(!Project(identity,{1,1,0,2},{0,0,0},screen,depth),"empty viewport rejected");
    const auto nan=std::numeric_limits<double>::quiet_NaN();
    check(!Project(identity,{0,2,0,2},{nan,0,0},screen,depth),"invalid world point rejected");
    Vec3 hit{99,99,99};
    check(IntersectHeight({0,0,10},{1,2,-1},5,hit)&&hit.x==5&&hit.y==10&&hit.z==5,"ray intersects selected Z plane");
    check(!IntersectHeight({0,0,10},{1,2,0},5,hit),"parallel ray rejected");
    check(!IntersectHeight({0,0,10},{1,2,1},5,hit),"intersection behind camera rejected");
    check(!IntersectHeight({0,0,10},{1,2,-1},nan,hit),"invalid target height rejected");
    std::printf("surfaceprojection: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
