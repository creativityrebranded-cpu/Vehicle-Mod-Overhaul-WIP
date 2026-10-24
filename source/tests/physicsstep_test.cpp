#include "physicsstep.h"
#include <cstdio>
#include <limits>
#include <initializer_list>
#include <type_traits>

int main() {
    int checks = 0, failed = 0;
    auto check = [&](bool ok, const char* label) { ++checks; if (!ok) { ++failed; std::printf("FAIL %s\n", label); } };
    using Native = std::uint64_t (*)(std::uintptr_t, std::uintptr_t, float);
    static_assert(std::is_same_v<physicsstep::Callback, Native>);
    for (float dt : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 120.0f, 1.0f / 1000.0f, 0.1f, 0.25f})
        check(physicsstep::ModelSeconds(dt) == double(dt), "valid native intervals are used exactly, with no artificial floor or wall-clock clamp");
    for (float dt : {0.0f, -0.01f, 0.5f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        check(physicsstep::ModelSeconds(dt) == 0, "paused or invalid/long intervals do not advance custom control");
    double slow = 0, fast = 0;
    for (int i = 0; i < 30; ++i) slow += 12.0 * physicsstep::ModelSeconds(1.0f / 30.0f);
    for (int i = 0; i < 120; ++i) fast += 12.0 * physicsstep::ModelSeconds(1.0f / 120.0f);
    check(std::fabs(slow - 12) < 1e-5 && std::fabs(fast - 12) < 1e-5, "equal simulated time gives equal acceleration regardless of callback frequency");
    double batched = 0;
    for (int i = 0; i < 4; ++i) batched += 12.0 * physicsstep::ModelSeconds(1.0f / 60.0f);
    check(std::fabs(batched - 0.8) < 1e-6, "four batched physics callbacks advance four simulation steps");
    std::printf("physicsstep: %d checks, %d failures\n", checks, failed);
    return failed ? 1 : 0;
}
