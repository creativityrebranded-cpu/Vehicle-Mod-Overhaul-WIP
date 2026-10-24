#include "cameraroll.h"
#include <cstdio>
#include <cstring>
#include <limits>

int main() {
    int checks=0,failed=0;
    auto check=[&](bool ok,const char* label){++checks;if(!ok){++failed;std::printf("FAIL %s\n",label);}};
    auto near=[](double a,double b){return std::fabs(a-b)<1e-5;};
    const double pi=cameraroll::kPi;
    const float basis[12]={1,0,0,101, 0,1,0,102, 0,0,1,103};
    for (double angle : {0.0,pi/6,pi/2,pi,pi*1.5,2*pi,-pi/3}) {
        float rows[12];std::memcpy(rows,basis,sizeof(rows));cameraroll::Apply(rows,angle);
        check(near(rows[0],std::cos(angle))&&near(rows[2],-std::sin(angle)),"right wing drops with positive bank including inverted attitude");
        check(std::memcmp(rows+4,basis+4,4*sizeof(float))==0,"camera forward is bitwise unchanged: no steering/aim feedback");
        check(rows[3]==101&&rows[7]==102&&rows[11]==103,"native row padding preserved");
        double rr=0,uu=0,ru=0;for(int i=0;i<3;++i){rr+=rows[i]*rows[i];uu+=rows[8+i]*rows[8+i];ru+=rows[i]*rows[8+i];}
        check(near(rr,1)&&near(uu,1)&&near(ru,0),"rotation remains orthonormal");
    }
    // Test a pitched/yawed native matrix: roll must leave that exact viewing direction alone too.
    float pitched[12]={0,1,0,0, -0.8f,0,0.6f,0, 0.6f,0,0.8f,0};
    float before[12];std::memcpy(before,pitched,sizeof(before));cameraroll::Apply(pitched,1.1);
    check(std::memcmp(before+4,pitched+4,4*sizeof(float))==0,"arbitrary pitched/yawed camera forward preserved");
    // First-person native E * O^T * M cancels chassis attitude when M=O; injecting in E banks once.
    float chassis[12];std::memcpy(chassis,basis,sizeof(chassis));cameraroll::Apply(chassis,pi/3);
    float e[12];std::memcpy(e,basis,sizeof(e));cameraroll::Apply(e,pi/3);
    bool composition=true;
    for(int row=0;row<3;++row)for(int col=0;col<3;++col){
        double value=0;for(int j=0;j<3;++j)for(int k=0;k<3;++k)value+=e[row*4+k]*chassis[j*4+k]*chassis[j*4+col];
        composition=composition&&near(value,e[row*4+col]);
    }
    check(composition,"first-person chassis cancellation does not double the inserted bank");
    cameraroll::Filter wrap;wrap.angle=179*pi/180;
    const double next=wrap.Step(-179*pi/180,1.0/60);
    check(std::fabs(std::remainder(next-179*pi/180,2*pi))<pi/180,"wrap boundary follows short arc");
    cameraroll::Filter a,b;
    for(int i=0;i<30;++i)a.Step(1,1.0/30);for(int i=0;i<120;++i)b.Step(1,1.0/120);
    check(std::fabs(a.angle-b.angle)<1e-12,"visual smoothing independent of refresh frequency");
    const double saved=a.angle;
    a.Step(0,0);a.Step(0,-1);a.Step(std::numeric_limits<double>::quiet_NaN(),0.1);
    check(a.angle==saved,"invalid or paused samples do not poison filter");
    auto h=cameraroll::Indicator(10*pi/180,0);
    check(near(h.y,20)&&near(h.x,0)&&near(h.rotation,0),"nose up moves horizon down");
    h=cameraroll::Indicator(10*pi/180,pi/2);
    check(near(h.rotation,-90)&&near(h.x,20)&&near(h.y,0),"bank rotates horizon opposite aircraft and rotates pitch displacement");
    h=cameraroll::Indicator(0,pi);
    check(near(h.rotation,-180)&&near(h.x,0)&&near(h.y,0),"inverted horizon retains full 180 degree bank");
    h=cameraroll::Indicator(pi/2,0);
    check(near(h.y,150),"extreme pitch displacement bounded to instrument");
    std::printf("cameraroll: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
