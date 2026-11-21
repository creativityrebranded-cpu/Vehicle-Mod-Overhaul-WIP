#include "stockfloat.h"
#include <cstdio>
#include <cmath>
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
using namespace stockfloat;

static Sample At(std::uint64_t now, double z, bool onWheels = false) {
    Sample s; s.vehicle = 0xFF000001; s.lease = 7; s.now = now; s.z = z; s.onWheels = onWheels; return s;
}

int main() {
    {   // capture: the car's own height is the commanded height, no lift, no ground reference needed
        Hold h;
        Check(Update(h, At(1000, -80.0, true)) == Event::Captured, "first frame with a lease captures");
        Check(h.z == -80.0 && h.engagedAt == 0, "the commanded height is the car's own height (absolute, nothing added)");
        Check(!Held(h, 1000, -80.0, 0, true), "on the game's wheels the float is not held");
        Check(Update(h, At(1100, -80.0, true)) == Event::None && h.engagedAt == 0, "wheels still on: flight not engaged");
        Update(h, At(1200, -80.0, false));
        Check(h.engagedAt == 1200, "flight mode seen: engaged time starts");
        Check(!Held(h, 1900, -80.0, 0, false), "not held before the settle time");
        Update(h, At(2300, -80.0, false));
        Check(Held(h, 2300, -80.0, 0, false), "held after the settle time at the commanded height");
        Check(!Held(h, 2300, -79.5, 0, false), "not held 0.5 m off the commanded height");
        Check(!Held(h, 2300, -80.0, 0.8, false), "not held while still climbing");
        Check(!Held(h, 2300, -80.0, 0, true), "not held once the wheels are on again");
        Check(!Held(h, 2300, std::nan(""), 0, false) && !Held(h, 2300, -80.0, std::nan(""), false), "non-finite readings are never held");
    }
    {   // the hold lapses when nobody renews it (lease ended / car promoted): the ordinary hover-or-drive rules take over
        Hold h; Update(h, At(1000, 5.0)); Update(h, At(1200, 5.0));
        Check(Active(h, 1500), "renewed within freshMs: active");
        Check(!Active(h, 1200 + rules::freshMs + 1), "not renewed: lapsed");
        Check(!Held(h, 1200 + rules::freshMs + 1, 5.0, 0, false), "a lapsed hold is never held");
        Hold none; Check(!Active(none, 1000), "empty hold is not active");
    }
    {   // relocation in progress: the height follows the car; applied receipt: destination height, once
        Hold h; Update(h, At(1000, -80.0)); Update(h, At(3000, -80.0));
        Check(Held(h, 3000, -80.0, 0, false), "held before the move");
        Sample s = At(3100, -70.0); s.relocating = true;
        Check(Update(h, s) == Event::Following, "move starts: follows the car");
        Check(h.z == -70.0 && !Held(h, 3100, -70.0, 0, false), "following: height tracks the car, not held");
        s = At(3200, -60.0); s.relocating = true;
        Check(Update(h, s) == Event::None && h.z == -60.0, "mid-move height keeps following (no pull back to the lab)");
        s = At(3300, 2.0); s.relocEpoch = 1; s.relocSerial = 5; s.relocToZ = 2.0;
        Check(Update(h, s) == Event::Rebased && h.z == 2.0, "receipt applied: commanded height is the road destination");
        Check(h.engagedAt == 3300 && !Held(h, 3300, 2.0, 0, false), "settle restarts after a move");
        s = At(3400, 2.0); s.relocEpoch = 1; s.relocSerial = 5; s.relocToZ = 2.0;
        Check(Update(h, s) == Event::None, "the same receipt is not applied twice");
        s = At(3500, 2.0); s.relocEpoch = 1; s.relocSerial = 4; s.relocToZ = 99.0;
        Check(Update(h, s) == Event::None && h.z == 2.0, "an older serial changes nothing");
        s = At(3600, 2.0); s.relocEpoch = 0; s.relocSerial = 9; s.relocToZ = 99.0;
        Check(Update(h, s) == Event::None && h.z == 2.0, "an older epoch changes nothing");
        s = At(3700, 2.0); s.relocEpoch = 2; s.relocSerial = 1; s.relocToZ = 3.0;
        Check(Update(h, s) == Event::Rebased && h.z == 3.0, "a newer epoch (serials restart) is applied");
        s = At(3800, 3.0); s.relocEpoch = 2; s.relocSerial = 1; s.relocToZ = 3.0;
        Update(h, s); Update(h, At(5000, 3.0));
        Check(Held(h, 5000, 3.0, 0, false), "held again at the destination after settling");
    }
    {   // a failed move (no applied receipt): the hold stays where the car was restored
        Hold h; Update(h, At(1000, -80.0)); Update(h, At(3000, -80.0));
        Sample s = At(3100, -75.0); s.relocating = true; Update(h, s);
        s = At(3200, -80.0); s.relocating = true; Update(h, s);
        Update(h, At(3300, -80.0));
        Check(!h.following && h.z == -80.0, "restored car: hold is the restored height");
    }
    {   // a receipt that was already applied before the capture is history
        Hold h; Sample s = At(1000, 1.0); s.relocEpoch = 1; s.relocSerial = 3; s.relocToZ = 50.0;
        Update(h, s);
        Check(h.z == 1.0, "capture keeps the car's own height, not an old receipt's");
        Check(Update(h, s) == Event::None && h.z == 1.0, "the old receipt is not applied after the capture");
    }
    {   // a new lease or another car re-captures; no lease clears
        Hold h; Update(h, At(1000, -80.0));
        Sample s = At(1100, -50.0); s.lease = 8;
        Check(Update(h, s) == Event::Captured && h.z == -50.0, "a new lease captures again");
        s = At(1200, -40.0); s.vehicle = 0xFF000002;
        Check(Update(h, s) == Event::Captured && h.z == -40.0, "another vehicle captures again");
        s = At(1300, -40.0); s.lease = 0;
        Check(Update(h, s) == Event::None && !Active(h, 1300), "no lease: the hold is cleared");
        s = At(1400, std::nan(""));
        Check(Update(h, s) == Event::None && !Active(h, 1400), "no finite height: no hold");
    }
    {
        Check(rules::pulseThrottle > 0 && rules::pulseThrottle <= .5, "the pulse is a gentle fraction of thrust");
        Check(rules::pulseHoldMs >= 50 && rules::pulseHoldMs < 500 && rules::pulseHoldMs < rules::freshMs, "the pulse command lapses by itself within half a second");
        Check(rules::pulseGear == 1, "the pulse uses the gentlest gear");
    }
    std::printf(failed ? "FAILED %d\n" : "ALL PASS\n", failed);
    return failed ? 1 : 0;
}
