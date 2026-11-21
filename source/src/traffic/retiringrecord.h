#pragma once
// SW-1 / SW-2 (2026-11-07): the Census's record of pairs being retired, pure parts (tests/traffic_rules_test.cpp). Owner of the record:
// the Census (generator.cpp CensusRetiringSave / CensusRetiringClear / LoadRetiringSaved); Retirement reads it in CoverPass (SW-2).
#include <cwchar>
#include <string>
#include <vector>
namespace retiringrecord
{
    inline bool RecoveryBaseMatches(unsigned expected,unsigned observed,unsigned legacyRev8) {
        return observed && (expected ? expected==observed : observed==legacyRev8 || (observed>>24)==0xFE);
    }
    struct PairRow { unsigned vehicle=0, driver=0, base=0; bool staged=false, surplus=false; };
    inline PairRow ParsePair(const wchar_t* value) {
        PairRow row;
        unsigned staged=0,surplus=0;
        if(std::swscanf(value,L"%x %x %x %x %x",&row.vehicle,&row.driver,&row.base,&staged,&surplus)<2)return {};
        row.staged=staged!=0;row.surplus=surplus==1;
        return row;
    }
    inline std::wstring FormatPair(const PairRow& row) {
        wchar_t value[64];
        std::swprintf(value,64,L"%08X %08X %08X %X %X",row.vehicle,row.driver,row.base,
                      row.staged?1u:0u,row.surplus?1u:0u);
        return value;
    }

    struct Entry { unsigned vehicle = 0, vbase = 0, driver = 0, dbase = 0; };
    // trafficpool.ini value: "vehicle vbase driver dbase, ..." in hex. Empty entries (both ids cleared) are left out.
    inline std::wstring Format(const std::vector<Entry>& list) {
        std::wstring out;
        for (const auto& e : list) { if (!e.vehicle && !e.driver) continue; wchar_t b[64]; std::swprintf(b, 64, L"%s%08X %08X %08X %08X", out.empty() ? L"" : L", ", e.vehicle, e.vbase, e.driver, e.dbase); out += b; }
        return out;
    }
    inline std::vector<Entry> Parse(const wchar_t* c) {
        std::vector<Entry> list;
        while (c && *c) { Entry e; wchar_t* x = nullptr;
            e.vehicle = unsigned(std::wcstoul(c, &x, 16)); if (x == c) { ++c; continue; } c = x;
            e.vbase = unsigned(std::wcstoul(c, &x, 16)); if (x == c) break; c = x;
            e.driver = unsigned(std::wcstoul(c, &x, 16)); if (x == c) break; c = x;
            e.dbase = unsigned(std::wcstoul(c, &x, 16)); if (x == c) break; c = x;
            list.push_back(e); while (*c == L',' || *c == L' ') ++c; }
        return list;
    }
    // SW-2: ids are reused across saves. A saved vehicle id is touched only if it still exists, is not an actor and has the stored base; a
    // driver id only if it is an actor with the stored base (0 stored: not checked beyond being an actor).
    inline bool VehicleStillOurs(bool exists, bool isActor, unsigned baseNow, unsigned baseSaved) { return exists && !isActor && baseSaved && baseNow == baseSaved; }
    inline bool DriverStillOurs(bool exists, bool isActor, unsigned baseNow, unsigned baseSaved) { return exists && isActor && (!baseSaved || baseNow == baseSaved); }
    // Clear one id from the record; drop entries with nothing left. Returns true when something changed.
    inline bool Clear(std::vector<Entry>& list, unsigned id) {
        if (!id) return false; bool changed = false;
        for (auto& e : list) { if (e.vehicle == id) { e.vehicle = 0; changed = true; } if (e.driver == id) { e.driver = 0; changed = true; } }
        if (changed) { std::vector<Entry> kept; for (const auto& e : list) if (e.vehicle || e.driver) kept.push_back(e); list.swap(kept); }
        return changed;
    }
}
