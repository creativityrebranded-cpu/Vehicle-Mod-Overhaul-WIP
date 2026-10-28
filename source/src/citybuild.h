#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <charconv>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "cityblob.h"
#include "ringschedule.h"
#include "cityprofile.h"

// FVO_CityTraffic.esm, rebuilt by the DLL itself (2026-10-24, the user: "make any placement made work automatically and allow
// expansion in game"). The game reads plugins only at start-up, and SFSE loads this DLL before it does, so at every start the
// plugin is rebuilt from the user's recordings when they changed (citystartup.cpp); spots recorded during play are served by a
// pool of spare vehicles at once (citytraffic.cpp) until the next start bakes them in.
//
// Everything is a PERSISTENT reference in New Atlantis' persistent cell (no per-cell records, which are ~700 KB each), so the
// plugin is the two fixed records of cityblob.h plus a list of references:
//   0x800 + k  traffic vehicle k     0x820 + k  its pilot (LvlCitizen_UC)
//   0x900 ..   one parked REV-8 per recorded spot that is not a traffic home; a spot keeps its id for good (citybuild-ids.tsv),
//              a removed spot's id is retired, never given to another spot (a save may remember what happened to it)
//   0xF00 + i  the spare pool: REV-8s, initially disabled, moved and enabled at run time onto spots recorded since the start
// Positions are in the city's own frame: the planet frame the recordings use, shifted (-7, +17) cells and -238.97 m.
// Pure: tests/citybuild_test.cpp (also checks the bytes against scripts/make_traffic_esm.py's plugin).
namespace citybuild {
inline constexpr const auto& kProfile=cityprofile::kNewAtlantis;
constexpr const char* kPluginName = kProfile.plugin;
constexpr std::uint32_t kFirstVehicle = kProfile.vehicles.first, kFirstPilot = kProfile.pilots.first, kFirstSpot = kProfile.firstSpot, kLastSpot = kProfile.lastSpot, kFirstPool = kProfile.pool.first;
constexpr int kPool = int(kProfile.pool.count);
constexpr std::uint32_t kRev8 = kProfile.serializedVehicleBase, kPilot = kProfile.serializedPilotBase, kCityWorld = kProfile.serializedWorld, kPersistentCell = kProfile.serializedCell;
constexpr std::uint32_t kPersistent = 0x400, kInitiallyDisabled = 0x800;
constexpr double kShiftX = kProfile.frame.shiftX, kShiftY = kProfile.frame.shiftY, kZOffset = kProfile.frame.shiftZ;
constexpr const char* kCityWorldPrefix = kProfile.placementWorldPrefix;
static_assert(kProfile.vehicles.count==ringschedule::kVehicles&&kProfile.pilots.count==ringschedule::kVehicles);

struct Spot { std::uint64_t key = 0; double x = 0, y = 0, z = 0, yaw = 0; };  // planet frame
struct Ref { bool actor = false; std::uint32_t local = 0, base = 0, flags = kPersistent; float x = 0, y = 0, z = 0, yaw = 0; std::uint64_t key=0; };

inline void ToCity(double x, double y, double z, double& cx, double& cy, double& cz) { const auto p=kProfile.frame.ToLocal(x,y,z);cx=p[0];cy=p[1];cz=p[2]; }

// ---- The recordings ----
inline std::vector<std::string> Fields(const std::string& line) {
    std::vector<std::string> out; std::size_t at = 0;
    while (true) { const auto t = line.find('\t', at); out.push_back(line.substr(at, t == std::string::npos ? std::string::npos : t - at)); if (t == std::string::npos) break; at = t + 1; }
    return out;
}
inline bool Key(const std::string& s, std::uint64_t& k) {k=0;if(s.empty()||s.size()>16)return false;const auto r=std::from_chars(s.data(),s.data()+s.size(),k,16);return r.ec==std::errc{}&&r.ptr==s.data()+s.size()&&k;}
inline bool Number(const std::string& s,double& out){char* end=nullptr;errno=0;out=std::strtod(s.c_str(),&end);return !s.empty()&&end==s.c_str()+s.size()&&!errno&&std::isfinite(out);}
inline bool CityWorld(const std::string& world){return cityprofile::MatchesPlacement(kProfile,world);}
// placement-markers.tsv (key, world file, local, runtime, world editor id, cell, x, y, z, yaw, label): the New Atlantis ones.
inline std::vector<Spot> Recorded(const std::vector<std::string>& lines,bool* valid=nullptr) {
    if(valid)*valid=true;
    std::vector<Spot> out;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const auto f = Fields(lines[i]); Spot s;
        if(f.size()<5||!CityWorld(f[4]))continue;
        if(f.size()<10||!Key(f[0],s.key)||!Number(f[6],s.x)||!Number(f[7],s.y)||!Number(f[8],s.z)||!Number(f[9],s.yaw)){if(valid)*valid=false;continue;}
        out.push_back(s);
    }
    return out;
}
// placement-removed.tsv (key, world editor id, x, y, z): keys removed, and their places (a key may be re-recorded within 2 m).
struct Removed { std::set<std::uint64_t> keys; std::vector<Spot> at; };
inline Removed RemovedFrom(const std::vector<std::string>& lines,bool* valid=nullptr) {
    if(valid)*valid=true;
    Removed r;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const auto f = Fields(lines[i]); std::uint64_t k = 0;
        if(f.size()<2||!CityWorld(f[1]))continue;
        Spot s;if(f.size()<5||!Key(f[0],k)||!Number(f[2],s.x)||!Number(f[3],s.y)||!Number(f[4],s.z)){if(valid)*valid=false;continue;}
        s.key=k;r.keys.insert(k);r.at.push_back(s);
    }
    return r;
}
inline bool IsRemoved(const Removed& r, const Spot& s) {
    if (r.keys.count(s.key)) return true;
    for (const auto& a : r.at) if (std::hypot(a.x - s.x, a.y - s.y) < 2.0 && std::fabs(a.z - s.z) < 2.0) return true;
    return false;
}
// Every spot: the baked ones (their aligned places win), then recorded ones with new keys; minus the removed.
inline std::vector<Spot> Merge(const std::vector<Spot>& baked, const std::vector<Spot>& recorded, const Removed& removed) {
    std::vector<Spot> out; std::set<std::uint64_t> seen;
    for (const auto& s : baked) if (seen.insert(s.key).second && !IsRemoved(removed, s)) out.push_back(s);
    for (const auto& s : recorded) if (seen.insert(s.key).second && !IsRemoved(removed, s)) out.push_back(s);
    return out;
}

// ---- Stable ids (citybuild-ids.tsv: "key local" or "key local retired") ----
struct Ids { std::map<std::uint64_t, std::uint32_t> live; std::map<std::uint64_t, std::uint32_t> retired; bool valid=true; };
inline Ids IdsFrom(const std::vector<std::string>& lines) {
    Ids ids;
    for (const auto& l : lines) {
        if(l.empty()||l[0]==';'||l[0]=='#')continue;
        std::istringstream in(l);std::string keyText,idText,state,extra;std::uint64_t k=0,local=0;
        if(!(in>>keyText>>idText)||!Key(keyText,k)||!Key(idText,local)){ids.valid=false;continue;}
        in>>state;if(in>>extra||(!state.empty()&&state!="retired")||local<kFirstSpot||local>kLastSpot||ids.live.count(k)||ids.retired.count(k)){ids.valid=false;continue;}
        (state=="retired"?ids.retired:ids.live)[k]=std::uint32_t(local);
    }
    std::set<std::uint32_t> used;for(const auto& entries:{ids.live,ids.retired})for(const auto& [k,v]:entries)if(!used.insert(v).second)ids.valid=false;
    return ids;
}
inline std::vector<std::string> IdsTo(const Ids& ids) {
    std::vector<std::string> out{ "; FVO_CityTraffic.esm: each recorded spot's reference id (0x900..0xEFF). Never edit or reuse an id: saves remember them." };
    char b[64];
    for (const auto& [k, v] : ids.live) { std::snprintf(b, sizeof(b), "%016llX %03X", (unsigned long long)k, v); out.push_back(b); }
    for (const auto& [k, v] : ids.retired) { std::snprintf(b, sizeof(b), "%016llX %03X retired", (unsigned long long)k, v); out.push_back(b); }
    return out;
}
// Gives each spot in `parked` an id (keeping the one it had), retires the ids of spots no longer parked here. False: out of ids.
inline bool Allocate(Ids& ids, const std::vector<Spot>& parked) {
    if(!ids.valid)return false;
    std::set<std::uint64_t> want; for (const auto& s : parked) want.insert(s.key);
    for (auto it = ids.live.begin(); it != ids.live.end();) {
        if (!want.count(it->first)) { ids.retired[it->first] = it->second; it = ids.live.erase(it); }
        else ++it;
    }
    std::set<std::uint32_t> used;
    for (const auto& [k, v] : ids.live) used.insert(v);
    for (const auto& [k, v] : ids.retired) used.insert(v);
    std::uint32_t next = kFirstSpot;
    for (const auto& s : parked) {
        if (ids.live.count(s.key)) continue;
        const auto r = ids.retired.find(s.key);
        if (r != ids.retired.end()) { ids.live[s.key] = r->second; ids.retired.erase(r); continue; }  // the same spot again: its own id back
        while (next <= kLastSpot && used.count(next)) ++next;
        if (next > kLastSpot) return false;
        ids.live[s.key] = next; used.insert(next);
    }
    return true;
}

// ---- The references ----
inline std::vector<Ref> Traffic(const ringschedule::Spot (&homes)[ringschedule::kVehicles]) {
    std::vector<Ref> out;
    const ringschedule::Spot* first = nullptr;
    for (const auto& h : homes) if (h.valid) { first = &h; break; }
    if (!first) return out;
    for (int k = 0; k < ringschedule::kVehicles; ++k) {
        const auto& h = homes[k].valid ? homes[k] : *first;
        double cx, cy, cz; ToCity(h.x, h.y, h.z, cx, cy, cz);
        out.push_back({ false, kFirstVehicle + std::uint32_t(k), kRev8, kPersistent, float(cx), float(cy), float(cz + 0.3), float(h.yaw) });
        // The pilot 2.5 m to the vehicle's left, facing it: inside the seat check's reach.
        out.push_back({ true, kFirstPilot + std::uint32_t(k), kPilot, kPersistent, float(cx - 2.5 * std::cos(h.yaw)), float(cy + 2.5 * std::sin(h.yaw)),
            float(cz + 0.1), float(h.yaw + ringlogic::kPi / 2) });
    }
    return out;
}
inline std::vector<Ref> Parked(const std::vector<Spot>& parked, const Ids& ids) {
    std::vector<Ref> out;
    for (const auto& s : parked) {
        const auto it = ids.live.find(s.key);
        if (it == ids.live.end()) continue;
        double cx, cy, cz; ToCity(s.x, s.y, s.z, cx, cy, cz);
        out.push_back({ false, it->second, kRev8, kPersistent, float(cx), float(cy), float(cz + 0.3), float(s.yaw),s.key });
    }
    std::sort(out.begin(), out.end(), [](const Ref& a, const Ref& b) { return a.local < b.local; });
    return out;
}
// Retain disabled records for removed identities: old saves may still reference these local IDs.
inline std::vector<Ref> Retired(const Ids& ids){std::vector<Ref> out;for(const auto& [key,id]:ids.retired)out.push_back({false,id,kRev8,kPersistent|kInitiallyDisabled,0,0,-2500,0,key});return out;}
// The pool waits disabled (a disabled reference is never loaded), under the city, out of reach.
inline std::vector<Ref> Pool() {
    std::vector<Ref> out;
    for (int i = 0; i < kPool; ++i) out.push_back({ false, kFirstPool + std::uint32_t(i), kRev8, kPersistent | kInitiallyDisabled, 0, 0, -2000.0f - float(i) * 5, 0 });
    return out;
}

// ---- The bytes ----
inline void Put(std::vector<std::uint8_t>& o, const void* p, std::size_t n) { const auto* b = static_cast<const std::uint8_t*>(p); o.insert(o.end(), b, b + n); }
template <class T> void PutT(std::vector<std::uint8_t>& o, T v) { Put(o, &v, sizeof(v)); }
inline void Sub(std::vector<std::uint8_t>& o, const char* tag, const void* p, std::size_t n) { Put(o, tag, 4); PutT<std::uint16_t>(o, std::uint16_t(n)); Put(o, p, n); }
inline std::vector<std::uint8_t> Grup(const void* label, std::int32_t type, const std::vector<std::uint8_t>& content) {
    std::vector<std::uint8_t> o; Put(o, "GRUP", 4); PutT<std::uint32_t>(o, std::uint32_t(24 + content.size())); Put(o, label, 4);
    PutT<std::int32_t>(o, type); PutT<std::uint32_t>(o, 0); PutT<std::uint16_t>(o, 0x245); PutT<std::uint16_t>(o, 0);
    o.insert(o.end(), content.begin(), content.end()); return o;
}
inline std::vector<std::uint8_t> Plugin(const std::vector<Ref>& refs) {
    constexpr std::uint32_t kMasters = 2;
    std::vector<std::uint8_t> body;
    std::uint32_t next = kFirstVehicle;
    for (const auto& r : refs) {
        std::vector<std::uint8_t> b;
        if(r.key){char name[48];std::snprintf(name,sizeof(name),"FVO_CitySpot_%016llX",(unsigned long long)r.key);Sub(b,"EDID",name,std::strlen(name)+1);}
        Sub(b, "NAME", &r.base, 4);
        const float data[6] = { r.x, r.y, r.z, 0.0f, 0.0f, r.yaw };
        Sub(b, "DATA", data, sizeof(data));
        Put(body, r.actor ? "ACHR" : "REFR", 4); PutT<std::uint32_t>(body, std::uint32_t(b.size())); PutT<std::uint32_t>(body, r.flags);
        PutT<std::uint32_t>(body, (kMasters << 24) | r.local); PutT<std::uint32_t>(body, 0); PutT<std::uint16_t>(body, 0x245); PutT<std::uint16_t>(body, 0);
        body.insert(body.end(), b.begin(), b.end());
        next = std::max(next, r.local + 1);
    }
    const std::uint32_t cell = kPersistentCell, world = kCityWorld;
    std::vector<std::uint8_t> children(std::begin(cityblob::kPersistentCell), std::end(cityblob::kPersistentCell));
    const auto inner = Grup(&cell, 6, Grup(&cell, 8, body));
    children.insert(children.end(), inner.begin(), inner.end());
    std::vector<std::uint8_t> top(std::begin(cityblob::kWorld), std::end(cityblob::kWorld));
    const auto worldChildren = Grup(&world, 1, children);
    top.insert(top.end(), worldChildren.begin(), worldChildren.end());
    const auto wrld = Grup("WRLD", 0, top);
    std::vector<std::uint8_t> tes4;
    { const float v = cityblob::kHedrVersion; std::uint8_t h[12]; std::memcpy(h, &v, 4); const std::uint32_t count = std::uint32_t(refs.size() + 2); std::memcpy(h + 4, &count, 4); std::memcpy(h + 8, &next, 4); Sub(tes4, "HEDR", h, 12); }
    Sub(tes4, "CNAM", "Flying Vehicle Overhaul (FVO)", 30);
    const std::uint64_t zero = 0;
    Sub(tes4, "MAST", "Starfield.esm", 14); Sub(tes4, "DATA", &zero, 8);
    Sub(tes4, "MAST", "SFBGS004.esm", 13); Sub(tes4, "DATA", &zero, 8);
    std::vector<std::uint8_t> out; Put(out, "TES4", 4); PutT<std::uint32_t>(out, std::uint32_t(tes4.size())); PutT<std::uint32_t>(out, 0x101);
    Put(out, cityblob::kHeaderTail, sizeof(cityblob::kHeaderTail));
    out.insert(out.end(), tes4.begin(), tes4.end()); out.insert(out.end(), wrld.begin(), wrld.end());
    return out;
}
}
