#include "flightsavelogic.h"
#include <cmath>
#include <cstdio>
#include <string>

int main() {
    using namespace flightsavelogic;
    int checks = 0, failures = 0;
    auto check = [&](bool good, const char* what) { ++checks; if (!good) { ++failures; std::printf("FAIL %s\n", what); } };
    // One key for the save routine's name and the load routine's name of the same save.
    check(Key("Save12_ABCD1234_Player_Akila") == "save12_abcd1234_player_akila", "plain save name");
    check(Key("C:\\Users\\x\\Documents\\My Games\\Starfield\\Saves\\Save12_ABCD1234_Player_Akila.sfs") == Key("Save12_ABCD1234_Player_Akila"), "path and .sfs give the same key");
    check(Key("Quicksave0.SFS") == "quicksave0", "extension case");
    check(Key("a b:c*?") == "a_b_c__", "unsafe characters become _");
    check(Key(nullptr).empty() && Key("").empty(), "no name, no key");
    check(Key(std::string(300, 'x').c_str()).size() == 120, "long names are cut");
    // Record round trip and rejection.
    Record r{ 0xFF00123Au, 412.25f, 3 }, back;
    check(Parse(Format(r), back) && back.vehicle == r.vehicle && back.gear == 3 && back.z > 412.2f && back.z < 412.3f, "record round trip");
    check(!Parse("", back) && !Parse("FVO saved hover v2\nvehicle=1\nz=1\ngear=1\n", back), "other formats are ignored");
    check(!Parse("FVO saved hover v1\nvehicle=00000000\nz=1\ngear=1\n", back), "no vehicle");
    check(!Parse("FVO saved hover v1\nvehicle=00000001\nz=1\ngear=9\n", back), "bad gear");
    check(!Parse("FVO saved hover v1\nvehicle=00000001\n", back), "cut record");
    // The load's answer.
    check(Decide(r, 0, 0, 0) == Verdict::Wait, "no seat yet: wait");
    check(Decide(r, 0, 0, kWaitMs) == Verdict::Expired, "no seat in time: end");
    check(Decide(r, 0x1234, 412, 10) == Verdict::OtherVehicle, "another vehicle: no hover");
    check(Decide(r, r.vehicle, 380, 10) == Verdict::Moved, "the vehicle already fell or moved: no hover");
    check(Decide(r, r.vehicle, std::nan(""), 10) == Verdict::Moved, "unreadable height: no hover");
    check(Decide(r, r.vehicle, 414, 10) == Verdict::Restore, "same vehicle, same spot: hover");
    std::printf("Saved hover: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
