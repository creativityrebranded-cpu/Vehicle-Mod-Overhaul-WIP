#include "modeladmission.h"
#include "vehicleworld.h"
#include <algorithm>
// Consumers receive the same published measurements already used by steering.
// An unobserved model returns unavailable; it never triggers a new native query.
extern "C" __declspec(dllexport) int FVO_ObservedTrafficModelV1(unsigned base,FvoObservedModelV1* out){
    if(!out||out->size!=sizeof(*out)||out->version!=1)return 0;
    static thread_local world::Snapshot snapshot;if(!world::Vehicles(snapshot))return 0;
    FvoObservedModelV1 result;bool found=false;
    for(unsigned i=0;i<snapshot.count;++i){const auto& w=snapshot.v[i];if(w.baseId!=base||!w.baseBoundsValid)continue;
        const double height=std::max(1.0,2.0*double(w.half[2]));const double below=std::clamp(double(w.half[2])-(double(w.center[2])-double(w.pos[2])),.2,2.5);
        result.halfWidth=std::max(result.halfWidth,std::max(.6,double(w.half[0])));result.halfLength=std::max(result.halfLength,std::max(1.0,double(w.half[1])));
        const double above=std::max(result.height-result.originAbove,height-below);result.originAbove=std::max(result.originAbove,below);result.height=above+result.originAbove;found=true;
    }
    if(!found||!modeladmission::Valid(result))return 0;*out=result;return 1;
}
