#include <cstdlib>
#include <string>
#include "../src/placementlogic.h"
#include <cstdio>
#include <set>
#include <vector>

static int failures = 0;
static void check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; }

int main()
{
    using namespace placementlogic;
    const std::uint64_t seed = 0x5EA3B1E5C0FFEE21ull;
    std::vector<std::uint64_t> types;
    for (int i = 0; i < 20; ++i) types.push_back(Fnv("vehicle" + std::to_string(i)) ^ Mix(0x800 + i));

    // The same spot always gets the same type.
    bool same = true;
    for (std::uint64_t spot = 1; spot < 300; ++spot)
        same = same && Pick(seed, 7, spot, types.data(), 20) == Pick(seed, 7, spot, types.data(), 20);
    check(same, "a spot always gets the same type");

    // The load order of the types does not matter.
    std::vector<std::uint64_t> reversed(types.rbegin(), types.rend());
    bool order = true;
    for (std::uint64_t spot = 1; spot < 300; ++spot)
        order = order && types[Pick(seed, 7, spot, types.data(), 20)] == reversed[Pick(seed, 7, spot, reversed.data(), 20)];
    check(order, "the load order of the vehicle types does not change any pick");

    // A city of 15 spots is a mix, not one type.
    std::set<int> city;
    for (std::uint64_t spot = 1000; spot < 1015; ++spot) city.insert(Pick(seed, 11, spot, types.data(), 20));
    check(city.size() >= 3, "a location's spots are a mix of several types");

    // Locations differ in their mix.
    auto mix = [&](std::uint64_t location) {
        std::vector<int> n(20, 0);
        for (std::uint64_t spot = 0; spot < 2000; ++spot) ++n[Pick(seed, location, spot * 7919, types.data(), 20)];
        return n;
    };
    const auto a = mix(1), b = mix(2);
    int differ = 0;
    for (int i = 0; i < 20; ++i) differ += std::abs(a[i] - b[i]);
    check(differ > 400, "two locations have different mixes");

    // Installing one more vehicle type changes only the spots that type wins.
    auto more = types;
    more.push_back(Fnv("new vehicle") ^ Mix(0x900));
    bool stable = true;
    int moved = 0;
    for (std::uint64_t spot = 1; spot < 2000; ++spot) {
        const int before = Pick(seed, 5, spot, types.data(), 20), after = Pick(seed, 5, spot, more.data(), 21);
        if (after != before) { ++moved; stable = stable && after == 20; }
    }
    check(stable && moved < 1000, "a new vehicle type only takes spots, it never reshuffles the others");

    // Another seed gives another arrangement.
    int changed = 0;
    for (std::uint64_t spot = 1; spot < 200; ++spot) changed += Pick(seed, 5, spot, types.data(), 20) != Pick(seed + 1, 5, spot, types.data(), 20);
    check(changed > 100, "the seed decides the arrangement");

    check(Pick(seed, 5, 1, nullptr, 0) == -1, "an empty pool picks nothing");
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
