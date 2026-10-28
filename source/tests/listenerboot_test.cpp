#include "listenerboot.h"
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

int main() {
    constexpr std::uintptr_t base = 0x140000000;
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* name) {
        ++checks;
        if (!ok) { ++failures; std::printf("FAIL: %s\n", name); }
    };
    auto code = listenerboot::kPublishBytes;
    std::array<std::uintptr_t,4> tables{};
    for (unsigned i = 0; i < tables.size(); ++i) tables[i] = base + listenerboot::kVtables[i];
    bool readable = true;
    std::atomic<std::uintptr_t> pointer{0};
    std::atomic<unsigned> writes{0};
    auto read = [&](std::uintptr_t at, void* out, std::size_t size) {
        if (!readable) return false;
        if (at == base + listenerboot::kAssignment && size == code.size()) {
            std::memcpy(out, code.data(), size); return true;
        }
        if (at == base + listenerboot::kObject && size == sizeof(tables)) {
            std::memcpy(out, tables.data(), size); return true;
        }
        return false;
    };
    auto publish = [&](std::uintptr_t at, std::uintptr_t value) {
        if (at != base + listenerboot::kPointer || value != base + listenerboot::kObject) return std::uintptr_t(1);
        ++writes;
        std::uintptr_t expected = 0;
        pointer.compare_exchange_strong(expected, value);
        return expected;
    };
    auto prepare = [&] { return listenerboot::Prepare(base, read, publish); };
    using R = listenerboot::Result;
    check(listenerboot::Prepare(0, read, publish) == R::Invalid && writes == 0, "zero module base cannot publish");
    readable = false;
    check(prepare() == R::Invalid && writes == 0, "unreadable game memory cannot publish");
    readable = true;
    code[3] ^= 1;
    check(prepare() == R::Invalid && writes == 0, "changed native initializer refuses without writing");
    code = listenerboot::kPublishBytes;
    for (unsigned i = 0; i < tables.size(); ++i) {
        tables[i] ^= 8;
        check(prepare() == R::Invalid && writes == 0, "each listener interface must match before writing");
        tables[i] ^= 8;
    }
    check(prepare() == R::Published && pointer == base + listenerboot::kObject, "cold vehicle load publishes native static listener");
    check(prepare() == R::Existing && pointer == base + listenerboot::kObject, "repeated preparation preserves existing listener");
    pointer = 0x12345678;
    check(prepare() == R::Existing && pointer == 0x12345678, "existing non-null native pointer is preserved");
    pointer = 0;
    std::atomic<unsigned> published{0}, existing{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 16; ++i) workers.emplace_back([&] {
        const auto result = prepare();
        if (result == R::Published) ++published;
        if (result == R::Existing) ++existing;
    });
    for (auto& worker : workers) worker.join();
    check(published == 1 && existing == 15 && pointer == base + listenerboot::kObject,
          "simultaneous preparations publish once and preserve the winning pointer");
    std::printf("listenerboot: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
