#pragma once
// Ray priority across cars (design section 9 "Rays", work order step 6). The game answers about 12 rays a frame across everything
// (ENGINE_HANDBOOK 8). Each plan car's path sweep asks 3 a frame, so with many cars the sweeps must share: nearest cars first, and a car
// that has waited moves up (kWaitWorth metres per frame waited), so a far car is measured less often but never starved.
// A gate in our own code only: prefetch, slots and the engine queries are unchanged. With no more asking cars than kCars, every car
// is served every frame, exactly as before. Decisions for frame N come from who asked in frame N-1 (the pilots run one after another).
// Pure; tests: tests/planrays_test.cpp.
#include <algorithm>
#include <cstdint>

namespace planrays {
constexpr int kMaxPilots = 512, kCars = 4;     // 4 cars x 3 rays = the ~12 a frame
constexpr double kWaitWorth = 15.0;            // m of distance one frame of waiting is worth

struct Budget {
    std::uint64_t frame = ~0ull;
    double askDist[kMaxPilots] = {}; bool asked[kMaxPilots] = {}; int askCount = 0;
    bool allowed[kMaxPilots] = {}; bool open = true;  // open: last frame had room for everyone
    int waited[kMaxPilots] = {};
    int served = 0, skipped = 0;                       // last frame, for traces
    // Called by a pilot that wants its sweep this frame. True: cast it now.
    bool Ask(std::uint64_t f, int pilot, double distance) {
        if (pilot < 0 || pilot >= kMaxPilots) return true;
        if (f != frame) Decide(f);
        if (!asked[pilot]) { asked[pilot] = true; askDist[pilot] = distance; ++askCount; }
        const bool go = open || allowed[pilot];
        if (go) { waited[pilot] = 0; ++served; } else { ++waited[pilot]; ++skipped; }
        return go;
    }
    void Decide(std::uint64_t f) {
        frame = f; served = skipped = 0;
        std::fill(std::begin(allowed), std::end(allowed), false);
        open = askCount <= kCars;
        if (!open) {
            int pick[kCars]; double score[kCars]; int n = 0;
            for (int i = 0; i < kMaxPilots; ++i) if (asked[i]) {
                const double s = askDist[i] - kWaitWorth * waited[i];
                if (n < kCars) { pick[n] = i; score[n] = s; ++n; continue; }
                int worst = 0; for (int k = 1; k < n; ++k) if (score[k] > score[worst]) worst = k;
                if (s < score[worst]) { pick[worst] = i; score[worst] = s; }
            }
            for (int k = 0; k < n; ++k) allowed[pick[k]] = true;
        }
        std::fill(std::begin(asked), std::end(asked), false); askCount = 0;
    }
};
}
