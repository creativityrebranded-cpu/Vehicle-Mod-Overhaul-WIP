// spawnhealth.h (card HR1, HR2): one rule for every stage, blame by spread, aging, trials, text round trip.
#include "traffic/spawnhealth.h"
#include <cstdio>
#include <cstring>

static int g_fail = 0, g_pass = 0;
static void Check(bool ok, const char* what) { if (ok) ++g_pass; else { ++g_fail; std::printf("FAIL: %s\n", what); } }

int main() {
    using namespace spawnhealth;
    Check(JudgeSeatEvidence(false, false, true) == SeatResult::Pending, "FI-4: a tested seat may still recover");
    Check(JudgeSeatEvidence(true, false, true) == SeatResult::Pass, "FI-4: a late seated driver gets a pass");
    Check(JudgeSeatEvidence(false, true, true) == SeatResult::Fail, "FI-4: a terminal tested seat gets a fail");
    Check(JudgeSeatEvidence(false, true, false) == SeatResult::Pending, "FI-4: an untested seat adds no evidence");
    Check(!DriveProved(0, 0) && DriveProved(30, 40), "FI-4: vertical-only motion cannot prove driving");
    // The rule: "never in 2" and "1 in 10" both reject; "1 in 4", "2 in 5" and a single fail do not.
    Check(Upper(0, 2) < rules::rejectBelow, "rule: 0 of 2 is low");
    Check(Upper(1, 10) < rules::rejectBelow, "rule: 1 of 10 is low");
    Check(Upper(1, 4) >= rules::rejectBelow && Upper(2, 5) >= rules::rejectBelow && Upper(0, 1) >= rules::rejectBelow, "rule: 1 of 4, 2 of 5, 0 of 1 are not");

    {   Store s; s.build = "b1";
        // A model failing to seat at two different spots is blamed.
        Check(!s.Report(Stage::Seat, 0x07000809, "r/0/1_1", false).modelRejected, "blame: one fail is no verdict");
        const auto c = s.Report(Stage::Seat, 0x07000809, "r/0/5_5", false);
        Check(c.modelRejected && s.ModelRejected(0x07000809), "blame: never sits at two spots -> model rejected");
        Check(!s.SpotRejected("r/0/1_1") && !s.SpotRejected("r/0/5_5"), "blame: the spots are not blamed for one model");
        // A model that sits sometimes is kept until it mostly fails.
        s.Report(Stage::Seat, 0xB, "a", true);
        for (int k = 0; k < 3; ++k) s.Report(Stage::Seat, 0xB, k % 2 ? "a" : "b", false);
        Check(!s.ModelRejected(0xB), "rule: 1 of 4 is kept");
        for (int k = 0; k < 6; ++k) s.Report(Stage::Seat, 0xB, k % 2 ? "a" : "b", false);
        Check(s.ModelRejected(0xB), "rule: 1 of 10 is rejected (mostly fails)");
    }
    {   Store s;
        // Same model, same spot, over and over: nothing to tell them apart -> unexplained, no verdict.
        s.Report(Stage::Seat, 0xC, "x", false); const auto c = s.Report(Stage::Seat, 0xC, "x", false);
        Check(!s.ModelRejected(0xC) && !s.SpotRejected("x") && c.unexplained && s.unexplained == 1, "blame: concentrated fails are unexplained, never a verdict");
        // A spot failing for two models is the spot.
        const auto d = s.Report(Stage::Body, 0xD, "y", false); (void)d; s.Report(Stage::Body, 0xE, "y", false);
        Check(s.SpotRejected("y") && !s.ModelRejected(0xD) && !s.ModelRejected(0xE), "blame: two models fail at one spot -> spot rejected");
    }
    {   Store s;
        // Any stage, same rule: a model whose cars never drive is rejected at the drive stage.
        s.Report(Stage::Seat, 0xF, "p", true); s.Report(Stage::Seat, 0xF, "q", true);
        s.Report(Stage::Drive, 0xF, "p", false); s.Report(Stage::Drive, 0xF, "q", false);
        const auto* r = s.Model(0xF); Check(r && r->rejected && r->why == int(Stage::Drive), "stages: drive fails reject like seat fails");
        Check(s.Odds(0xF) < rules::preferOdds && s.Odds(0x123) == .5, "odds: weakest stage; unknown .5");
        // The user's allow wins.
        s.Allow(0xF, true); Check(!s.ModelRejected(0xF), "allow: the user's call wins");
        // Trial: one in retrialEvery asks.
        s.Allow(0xF, false); int trials = 0; for (int k = 0; k < 2 * rules::retrialEvery; ++k) trials += s.ModelTrial(0xF);
        Check(trials == 2, "trial: one in retrialEvery asks");
    }
    {   Store s; s.build = "b1";
        s.Report(Stage::Seat, 0x10, "a", false); s.Report(Stage::Seat, 0x10, "b", false); s.Allow(0x11, true);
        s.Report(Stage::Body, 0x12, "z|bad;name", false);
        const auto text = s.Save();
        Check(text.find('\r') == std::string::npos, "file: LF only");
        Store same; same.build = "b1"; Check(same.Load(text) && same.ModelRejected(0x10) && same.Model(0x11) && same.Model(0x11)->allowed, "file: same build keeps verdicts and allows");
        Check(same.spots.count("z_bad_name") == 1, "file: separators in keys are cleaned");
        Store next; next.build = "b2"; Check(next.Load(text) && !next.ModelRejected(0x10), "file: a new build ages the evidence (fresh evidence needed)");
        next.Report(Stage::Seat, 0x10, "c", false); Check(next.ModelRejected(0x10), "file: aged evidence plus one new fail rejects again");
        Store bad; Check(!bad.Load("something else\nm|x"), "file: another format reads nothing");
    }
    {   Store s;
        // A reused id with other bounds starts again; the user's allow is kept.
        Check(!s.Fingerprint(0x20, 2.4, 1.1), "fingerprint: first seen");
        s.Report(Stage::Seat, 0x20, "a", false); s.Report(Stage::Seat, 0x20, "b", false); s.Allow(0x20, true);
        Check(!s.Fingerprint(0x20, 2.4, 1.1) && s.Model(0x20)->stage[0].fail == 2, "fingerprint: same bounds keep the record");
        Check(s.Fingerprint(0x20, 5.0, 2.0) && s.Model(0x20)->stage[0].fail == 0 && s.Model(0x20)->allowed, "fingerprint: other bounds start again, allow kept");
    }
    Check(SpotKey("ring road", 1, 15, -3) == "ring_road/1/1_-1", "spot key: route, lane, 10 m cell");
    std::printf("spawnhealth_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
