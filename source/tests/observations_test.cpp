#include "observations.h"
#include <cstdio>
int main(){observations::Gate gate;int failed=0;auto check=[&](bool ok,const char* why){if(!ok){++failed;std::printf("FAIL %s\n",why);}};
    const auto a=observations::Key("physics",1),b=observations::Key("physics",2);
    check(gate.Sample(100,a,0).emit,"initial observation");
    check(!gate.Sample(200,a,0).emit,"unchanged frames do not flood logs");
    check(!gate.Sample(210,a,1).emit,"fast changes coalesce");
    auto changed=gate.Sample(400,a,1);check(changed.emit&&changed.coalesced==1,"coalesced change is reported rather than silently lost");
    check(gate.Sample(400,b,1).emit,"vehicle identities remain independent");
    check(!gate.Sample(5399,a,1).emit&&gate.Sample(5400,a,1).emit,"unchanged health heartbeat");
    check(gate.Sample(10,a,1).emit,"clock/session rewind restarts diagnostics");
    check(observations::Key("seat",1)!=a,"systems do not overwrite each other's identity");
    for(unsigned i=0;i<20000;++i)gate.Sample(10000,observations::Key("fleet",i),i);
    check(gate.Sample(20000,a,2).emit,"bounded storage continues after many retired vehicle ids");
    std::printf("observations: 9 checks, %d failures\n",failed);return failed?1:0;
}
