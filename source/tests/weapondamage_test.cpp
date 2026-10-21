#include "weapondamage.h"
#include <cstdio>
#include <limits>
#include <initializer_list>

int main()
{
    int failed = 0, checks = 0;
    auto check = [&](bool ok, const char* why) { ++checks; if (!ok) { ++failed; std::printf("FAIL: %s\n", why); } };
    auto hit = weapondamage::Resolve(true, 84.0f, 0.25f, 20.0f);
    check(hit.native && hit.damage == 21.0f, "native weapon value and fractional ini scale");
    hit = weapondamage::Resolve(true, 0.0f, 1.0f, 20.0f);
    check(hit.native && hit.damage == 0.0f, "valid zero never becomes fallback damage");
    hit = weapondamage::Resolve(true, 80.0f, 0.0f, 20.0f);
    check(hit.native && hit.damage == 0.0f, "zero scale explicitly disables weapon damage");
    hit = weapondamage::Resolve(false, 80.0f, 0.25f, 31.0f);
    check(!hit.native && hit.damage == 31.0f, "unavailable data uses unscaled configured fallback");
    for (float bad : { -1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 1000001.0f }) {
        hit = weapondamage::Resolve(true, bad, 1.0f, 20.0f);
        check(!hit.native && hit.damage == 20.0f, "invalid native data uses fallback");
    }
    hit = weapondamage::Resolve(true, 1000000.0f, 1000.0f, 20.0f);
    check(!hit.native && hit.damage == 20.0f, "scaled overflow rejected");
    check(weapondamage::Scale(-1) == 1 && weapondamage::Scale(NAN) == 1, "bad ini scale defaults to one");
    hit = weapondamage::Resolve(true, 12.0f * 2.0f, 1.0f, 20.0f);
    check(hit.native && hit.damage == 24.0f, "already native difficulty-scaled result is not multiplied twice");
    std::printf("Weapon damage: %d/%d checks passed\n", checks - failed, checks);
    return failed ? 1 : 0;
}
