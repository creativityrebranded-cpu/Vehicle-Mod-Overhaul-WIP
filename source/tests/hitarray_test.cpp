// PF-1: reading a Havok all-hits collector where the hits really are (hitarray.h).
#include "hitarray.h"
#include <cstdio>
#include <vector>
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }

struct Collector {
    alignas(16) std::uint8_t c[0x50] = {};
    void Set(std::uint8_t* array, std::uint32_t count, std::uint32_t size, std::uint32_t capacity) {
        std::memcpy(c + 0x30, &array, sizeof array); std::memcpy(c + 0xC, &count, 4); std::memcpy(c + 0x38, &size, 4); std::memcpy(c + 0x3C, &capacity, 4);
    }
};

int main() {
    alignas(16) static std::uint8_t inlineHits[0x490];
    std::vector<std::uint8_t> grown(40 * hitarray::kHitBytes);
    Collector col;

    col.Set(inlineHits, 7, 7, 0x8000000A);
    auto v = hitarray::Read(col.c, inlineHits);
    Check(v.data == inlineHits && v.count == 7 && !v.owned, "up to 10 hits: the inline buffer, nothing to free");

    col.Set(inlineHits, 25, 25, 0x8000000A);
    v = hitarray::Read(col.c, inlineHits);
    Check(v.count == 10, "a size past the inline room never reads past the 10 slots");

    col.Set(grown.data(), 23, 23, 40);
    v = hitarray::Read(col.c, inlineHits);
    Check(v.data == grown.data() && v.count == 23 && v.owned, "grown by Havok: the heap array, all 23 hits, owned (to free)");

    col.Set(grown.data(), 23, 23, 0x80000028);
    v = hitarray::Read(col.c, inlineHits);
    Check(v.data == grown.data() && !v.owned, "an array marked not owned is read but never freed");

    col.Set(grown.data(), 0x7FFFFFFF, 0x7FFFFFFF, 0x3FFFFFFF);
    v = hitarray::Read(col.c, inlineHits);
    Check(v.count == hitarray::kMaxRead, "a garbage size is capped");

    col.Set(nullptr, 3, 3, 40);
    v = hitarray::Read(col.c, inlineHits);
    Check(!v.data && v.count == 0 && !v.owned, "no array: nothing read, nothing freed");

    hitarray::Stats st;
    col.Set(inlineHits, 4, 4, 0x8000000A); st.Note(hitarray::Read(col.c, inlineHits));
    col.Set(grown.data(), 23, 23, 40); st.Note(hitarray::Read(col.c, inlineHits));
    Check(st.casts == 2 && st.grown == 1 && st.largest == 23 && st.freedBytes == 40ull * hitarray::kHitBytes, "the trace counts casts, grown arrays, the largest count and the bytes given back");

    std::printf(failed ? "FAILED\n" : "ALL PASS\n");
    return failed ? 1 : 0;
}
