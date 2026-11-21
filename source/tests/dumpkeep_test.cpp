// PF-2: kept dump names, which old copies go, and the allocator retry ranges (dumpkeep.h).
#include "dumpkeep.h"
#include <cstdio>
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }

int main() {
    Check(dumpkeep::Name(L"hang", 2026, 11, 12, 13, 11, 48, true) == L"hang-20261112-131148-fg.dmp", "a dated name, window in front");
    Check(dumpkeep::Name(L"hang", 2026, 11, 12, 13, 59, 2, false) == L"hang-20261112-135902-bg.dmp", "a dated name, window behind");

    std::vector<std::wstring> names = { L"hang-20261112-135902-bg.dmp", L"hang-20261110-172001-fg.dmp", L"hang-20261112-131148-fg.dmp",
                                        L"hang-20261111-060156-fg.dmp", L"stall-20261112-131145-fg.dmp", L"last-hang.dmp", L"hang-notes.txt" };
    const auto del = dumpkeep::ToDelete(names, L"hang", 3);
    Check(del.size() == 1 && del[0] == L"hang-20261110-172001-fg.dmp", "four hang copies, keep 3: only the oldest goes");
    Check(dumpkeep::ToDelete(names, L"stall", 6).empty(), "other kinds and other files are never touched");
    Check(dumpkeep::ToDelete({ L"hang-20261112-131148-fg.dmp" }, L"hang", 3).empty(), "fewer than kept: nothing deleted");

    Check(dumpkeep::AllocatorSpinAt(0x023F6E55) != nullptr, "the 13:11 spin address (+23F6E55) is in the allocator");
    Check(dumpkeep::AllocatorSpinAt(0x022C8300) != nullptr, "inside the out-of-memory handler");
    Check(dumpkeep::AllocatorSpinAt(0x023F74D4) != nullptr && dumpkeep::kRetryReturn == 0x023F74D4, "the retry loop's return address");
    Check(dumpkeep::AllocatorSpinAt(0x028D409C) == nullptr && dumpkeep::AllocatorSpinAt(0x03730CC0) == nullptr, "an idle worker or the ray walk is not the allocator");

    std::printf(failed ? "FAILED\n" : "ALL PASS\n");
    return failed ? 1 : 0;
}
