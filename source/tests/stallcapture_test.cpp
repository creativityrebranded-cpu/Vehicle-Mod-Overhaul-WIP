#include "stallcapture.h"
#include <cstdio>
int main(){int n=0,fail=0;auto check=[&](bool ok,const char* why){++n;if(!ok){++fail;std::printf("FAIL %s\n",why);}};using namespace stallcapture;Policy p;
    check(p.Check(3999,1000,true,true)==Gate::Healthy,"normal callback gaps below three seconds do not capture");
    check(p.Check(4000,1000,true,false)==Gate::Background,"alt-tab gaps remain excluded");
    check(p.Check(4000,1000,false,true)==Gate::NotGameplay,"loading and paused-world state is reported rather than captured as gameplay");
    check(p.Check(4000,1000,true,true)==Gate::Capture,"a foreground gameplay stall is captured at three seconds");
    p.Result(4000,1000,false);
    check(p.successes==0&&p.capturedBeat==0,"a failed writer does not spend a successful capture or latch this stall");
    check(p.Check(4500,1000,true,true)==Gate::RetryDelay,"failed writes are paced");
    check(p.Check(5000,1000,true,true)==Gate::Capture,"the same ongoing stall is retried after a failed write");
    p.Result(5000,1000,true);check(p.successes==1&&p.Check(6000,1000,true,true)==Gate::Captured,"a successful dump captures only once per hitch");
    check(p.Check(6500,6400,true,true)==Gate::Healthy,"callback recovery disarms the old hitch");
    p.Result(10000,7000,true);p.Result(14000,11000,true);
    check(p.Check(18000,15000,true,true)==Gate::Quota,"three successful dumps cap per-run capture cost");
    Policy denied;for(Time t=4000;t<=6000;t+=1000)denied.Result(t,1000,false);
    check(denied.Check(7000,1000,true,true)==Gate::AttemptsExhausted,"a broken writer cannot spin on a single stall");
    check(denied.Check(12000,9000,true,true)==Gate::Capture,"a later independent hitch still gets an attempt after earlier failures");
    check(p.Check(1,1000,true,true)==Gate::NoBeat,"clock reset cannot masquerade as an enormous stall");
    std::printf("stall capture: %d checks, %d failures\n",n,fail);return fail?1:0;
}
