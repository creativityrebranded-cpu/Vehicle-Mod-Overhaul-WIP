#pragma once
#include "citybuild.h"
#include <functional>

namespace cityrecords {
struct Inspection {bool valid=false;std::string error;citybuild::Ids ids;std::set<std::uint32_t> parked;std::size_t refs=0;};
inline Inspection Inspect(const std::vector<std::uint8_t>& bytes){
    using namespace citybuild;Inspection result;
    const auto fail=[&](const char* why){result.error=why;return false;};
    const auto u32=[&](std::size_t at){std::uint32_t v=0;std::memcpy(&v,bytes.data()+at,4);return v;};
    std::uint32_t declared=0;std::size_t count=0;std::vector<std::string> masters;std::set<std::uint32_t> seen;
    bool header=false,world=false,cell=false;
    std::function<bool(std::size_t,std::size_t,int)> walk;
    walk=[&](std::size_t from,std::size_t end,int depth){
        if(depth>8)return fail("group nesting");
        while(from<end){
            if(end-from<24)return fail("truncated record");
            const auto size=u32(from+4);const std::string tag(reinterpret_cast<const char*>(bytes.data()+from),4);
            if(tag=="GRUP"){
                if(size<24||size>end-from)return fail("invalid group size");
                if(!walk(from+24,from+size,depth+1))return false;from+=size;continue;
            }
            if(size>end-from-24)return fail("invalid record size");
            const auto recordEnd=from+24+size;const auto id=u32(from+12),flags=u32(from+8);
            if(tag=="WRLD"){
                if(world||id!=kCityWorld||recordEnd-from!=sizeof(cityblob::kWorld)||!std::equal(bytes.begin()+std::ptrdiff_t(from),bytes.begin()+std::ptrdiff_t(recordEnd),std::begin(cityblob::kWorld)))return fail("world override");
                world=true;++count;from=recordEnd;continue;
            }
            else if(tag=="CELL"){
                // This fixed vanilla record is compressed. Validate its exact known bytes rather than parsing compressed data as subrecords.
                if(cell||id!=kPersistentCell||recordEnd-from!=sizeof(cityblob::kPersistentCell)||!std::equal(bytes.begin()+std::ptrdiff_t(from),bytes.begin()+std::ptrdiff_t(recordEnd),std::begin(cityblob::kPersistentCell)))return fail("persistent cell");
                cell=true;++count;from=recordEnd;continue;
            }
            else if(tag!="TES4"&&tag!="REFR"&&tag!="ACHR")return fail("unexpected record");
            std::string editor;std::uint32_t base=0;bool position=false,hedr=false;
            for(std::size_t p=from+24;p<recordEnd;){
                if(recordEnd-p<6)return fail("truncated subrecord");
                std::uint16_t n=0;std::memcpy(&n,bytes.data()+p+4,2);if(n>recordEnd-p-6)return fail("invalid subrecord size");
                const std::string sub(reinterpret_cast<const char*>(bytes.data()+p),4);const auto* data=bytes.data()+p+6;
                if(sub=="EDID"||sub=="MAST"){
                    if(!n||data[n-1]!=0)return fail("unterminated string");
                    const std::string value(reinterpret_cast<const char*>(data),n-1);if(sub=="EDID")editor=value;else masters.push_back(value);
                }
                if(sub=="HEDR"){if(n!=12)return fail("header size");std::memcpy(&declared,data+4,4);hedr=true;}
                if(sub=="NAME"&&n==4)std::memcpy(&base,data,4);
                if(sub=="DATA"&&n==24){float v[6];std::memcpy(v,data,24);for(float x:v)if(!std::isfinite(x))return fail("nonfinite position");position=true;}
                p+=6+n;
            }
            if(tag=="TES4"){if(from!=0||header||!hedr||flags!=0x101)return fail("plugin header");header=true;}
            else ++count;
            if(tag=="REFR"||tag=="ACHR"){
                if((id>>24)!=2||!seen.insert(id).second||!position||!(flags&kPersistent))return fail("reference identity/position");
                const auto local=id&0xFFFFFF;
                const bool pilot=local>=kFirstPilot&&local<kFirstPilot+ringschedule::kVehicles;
                const bool traffic=local>=kFirstVehicle&&local<kFirstVehicle+ringschedule::kVehicles;
                const bool parked=local>=kFirstSpot&&local<=kLastSpot;
                const bool pool=local>=kFirstPool&&local<kFirstPool+kPool;
                if(!(pilot||traffic||parked||pool)||pilot!=(tag=="ACHR")||base!=(pilot?kPilot:kRev8))return fail("reference range/base");
                if(pool&&!(flags&kInitiallyDisabled))return fail("pool must start disabled");
                if(parked){
                    result.parked.insert(local);
                    if(!editor.empty()){
                        constexpr const char* prefix="FVO_CitySpot_";std::uint64_t key=0;
                        if(editor.rfind(prefix,0)!=0||!Key(editor.substr(std::strlen(prefix)),key)||result.ids.live.count(key)||result.ids.retired.count(key))return fail("parked identity");
                        (flags&kInitiallyDisabled?result.ids.retired:result.ids.live)[key]=local;
                    }
                }
                ++result.refs;
            }
            from=recordEnd;
        }
        return true;
    };
    if(bytes.empty()||!walk(0,bytes.size(),0))return result;
    if(!header||!world||!cell||declared!=count||masters!=std::vector<std::string>{"Starfield.esm","SFBGS004.esm"}){result.error="missing header/world/cell/master/count";return result;}
    for(std::uint32_t k=0;k<std::uint32_t(ringschedule::kVehicles);++k)
        if(!seen.count(0x02000000|kFirstVehicle+k)||!seen.count(0x02000000|kFirstPilot+k)){result.error="missing traffic pair";return result;}
    result.valid=true;return result;
}
}
