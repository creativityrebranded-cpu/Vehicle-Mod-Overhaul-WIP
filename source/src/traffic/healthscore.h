#pragma once
// Continuous health (strip build 2, cards CH-3 and CH-5, notes/spawn-one-place-20261107/CHANGE_CARDS.md). Owner: Health (spawn flow
// step 7), the only judge. Pure: tests/healthscore_test.cpp.
//
// Two layers, both fed every judgement from facts (the core's raw sample, CH-1, and what the addon sees itself):
//  - The account: a leaky score. Each second of play adds what the pair failed to do (asked to move and did not, asked and no physics
//    steps, off its lane, leaning, driver aboard but not sitting) and leaks slowly when it does well. Faults that come and go add up;
//    one bad reading never breaks a pair. Broken at brokenAt; doubtful from doubtAt (Health then asks for a spot test, CH-4). A pair
//    broken by score is healthy again only after clearForMs of real driving that moved it clearMoved metres.
//  - Decisive faults: measured proof a pair is completely broken (off its path, off its level, on its side, fell, no body, driver out
//    or apart, spot test dead). Each must hold confirmMs of play; then the pair is broken and stays broken (only Retirement ends it).
// Paused play (the one play clock, W1) adds and leaks nothing, and confirms nothing. A pair's own "no steps" is evidence, never a pause.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace healthscore
{
    namespace rules {
        inline constexpr double brokenAt = 10.0, doubtAt = 4.0, maxScore = 20.0;
        inline constexpr double leakPerS = 0.5;
        inline constexpr double askedMove = 1.0;        // m/s asked: the plan wants it to move
        inline constexpr double notMovingSpeed = 0.3;   // m/s actual under that while asked: not moving
        inline constexpr double wNotMoving = 1.0, wNoSteps = 1.0, wOffLane = 0.5, wLeaning = 1.0, wNotSitting = 1.0;  // per second
        inline constexpr double offLaneSoft = 0.5;      // m beyond the lane's half width: soft evidence
        inline constexpr double leaningUpZ = 0.85;      // up axis z under this: leaning (soft)
        inline constexpr unsigned long long clearForMs = 8000; inline constexpr double clearMoved = 8.0;
        // Decisive faults (user: "something that drops it extensively").
        inline constexpr double offPathMargin = 2.0;    // m beyond the lane's half width, with no part of the body in its field
        inline constexpr double offLevelGap = 3.0;      // m above or below the wanted floor, on its rail
        inline constexpr double tippedUpZ = 0.5;        // on its side or upside down
        inline constexpr unsigned long long confirmPathMs = 1000, confirmLevelMs = 2000, confirmTippedMs = 2000, confirmDriverMs = 3000;
        inline constexpr unsigned long long confirmSpotMs = 2000; // a newer live result can supersede a failed pulse before retirement
        // Strip build 4 (M1, D-S2): the in-play fixes are gone, so what they patched is Health's now.
        inline constexpr unsigned long long confirmNoJobMs = 60000;       // driving with no trip (the trip restarts every 10 s had their chance)
        inline constexpr unsigned long long confirmDriverModelMs = 10000; // vehicle loaded, its seated driver has no model, near the player
        inline constexpr unsigned long long maxStepMs = 1500;   // longest gap one judgement may count
    }

    // What Health knows about one pair at one judgement. All facts, no thresholds.
    struct Input {
        bool play = true;          // W1: play time runs
        bool driving = false;      // handed to the driving pipeline (Census Driving / Recovering)
        bool sample = false;       // the core's sample (CH-1) is fresh; the fields below it are meaningful
        double asked = 0, actual = 0; bool steps = true;
        double lateral = 0, halfWidth = 0, fieldShare = -1, floorGap = 0, upZ = 1; bool floorKnown = false, onRail = false;
        bool askedKnown = false;   // asked/actual known (a sample); without one, no steps counts whenever driving
        bool aboard = true, sitting = true; bool driverOut = false, driverFar = false;  // from the pair's record (JudgePair facts)
        bool fell = false, noBody = false, spotDead = false, misplaced = false;          // facts seen by their own steps
        bool noJob = false, driverNoModel = false;                                       // strip build 4: no trip; driver without a model near the player
        double moved = 0;          // m the body moved since the last judgement
    };
    enum class Kind : int { None, OffPath, OffLevel, Tipped, DriverOut, DriverFar, Fell, NoBody, SpotDead, Misplaced, NoJob, DriverNoModel, Count };
    inline const char* Name(Kind k) {
        constexpr const char* n[]{ "none", "off_its_path", "off_its_level", "on_its_side", "driver_out", "driver_apart", "fell_through_world", "no_physics_body", "spot_test_dead", "misplaced_at_arrival", "no_trip", "driver_no_model" };
        return n[int(k)];
    }
    enum class State : int { Healthy, Doubtful, Broken };
    inline const char* Name(State s) { constexpr const char* n[]{ "healthy", "doubtful", "broken" }; return n[int(s)]; }

    struct Account {
        double score = 0;
        bool brokenByScore = false;
        bool spotWasFailed = false;
        Kind decisive = Kind::None;                                  // latched: never clears
        unsigned long long since[int(Kind::Count)]{};                // play ms each decisive fact has held
        unsigned long long goodMs = 0; double goodMoved = 0;         // real driving since broken by score
        unsigned long long at = 0;                                   // last judgement
        const char* top = "";                                        // the largest contributor of the last judgement
        State state = State::Healthy;
        const char* why = "healthy";
    };

    // One judgement. now: ms clock; the account remembers the last one.
    inline State Step(Account& a, const Input& in, unsigned long long now)
    {
        const unsigned long long dtMs = a.at && now > a.at ? std::min<unsigned long long>(now - a.at, rules::maxStepMs) : 0;
        a.at = now;
        if (a.decisive != Kind::None) { a.state = State::Broken; a.why = Name(a.decisive); return a.state; }
        const double dt = in.play ? dtMs / 1000.0 : 0.0;
        // Confirm a failed spot result across live judgements. Fell, no body and misplaced already arrive confirmed.
        auto hold = [&](Kind k, bool on, unsigned long long confirm) {
            auto& s = a.since[int(k)];
            if (!on) { s = 0; return; }
            if (in.play) s += dtMs;
            if (confirm == 0 || s >= confirm) a.decisive = k;
        };
        hold(Kind::Fell, in.fell, 0); hold(Kind::NoBody, in.noBody, 0); hold(Kind::Misplaced, in.misplaced, 0);
        hold(Kind::SpotDead, in.spotDead&&a.spotWasFailed, rules::confirmSpotMs);
        a.spotWasFailed=in.spotDead&&in.play;
        if (in.driving) {
            hold(Kind::DriverOut, in.driverOut, rules::confirmDriverMs);
            hold(Kind::DriverFar, in.driverFar, rules::confirmDriverMs);
            hold(Kind::NoJob, in.noJob, rules::confirmNoJobMs);
            hold(Kind::DriverNoModel, in.driverNoModel, rules::confirmDriverModelMs);
            if (in.sample) {
                const double out = std::fabs(in.lateral) - in.halfWidth;
                const bool offPath = out > rules::offPathMargin && (in.fieldShare < 0 || in.fieldShare <= 0.0);
                hold(Kind::OffPath, offPath, rules::confirmPathMs);
                hold(Kind::OffLevel, in.onRail && in.floorKnown && std::fabs(in.floorGap) > rules::offLevelGap, rules::confirmLevelMs);
                hold(Kind::Tipped, in.upZ < rules::tippedUpZ, rules::confirmTippedMs);
            }
        }
        if (a.decisive != Kind::None) { a.state = State::Broken; a.why = Name(a.decisive); return a.state; }
        // The account (driving pairs only: a pair still being proved is Birth's).
        double add = 0, best = 0; a.top = "";
        auto put = [&](bool on, double w, const char* name) { if (!on) return; add += w; if (w > best) { best = w; a.top = name; } };
        if (in.driving) {
            const bool askedMove = in.askedKnown ? in.asked > rules::askedMove : true;
            put(in.askedKnown && askedMove && in.actual < rules::notMovingSpeed && in.steps, rules::wNotMoving, "asked_not_moving");
            put(askedMove && !in.steps, rules::wNoSteps, "no_physics_steps");
            if (in.sample) {
                put(std::fabs(in.lateral) - in.halfWidth > rules::offLaneSoft, rules::wOffLane, "off_its_lane");
                put(in.upZ < rules::leaningUpZ, rules::wLeaning, "leaning");
            }
            put(in.aboard && !in.sitting, rules::wNotSitting, "driver_not_sitting");
        }
        a.score = std::clamp(a.score + (add > 0 ? add : -rules::leakPerS) * dt, 0.0, rules::maxScore);
        if (a.score >= rules::brokenAt && !a.brokenByScore) { a.brokenByScore = true; a.goodMs = 0; a.goodMoved = 0; }
        if (a.brokenByScore) {
            // Healthy again only after real driving: moving, stepping, nothing adding.
            if (in.play && add == 0 && in.driving && in.steps && in.moved > 0.05) { a.goodMs += dtMs; a.goodMoved += in.moved; }
            else if (in.play && add > 0) { a.goodMs = 0; a.goodMoved = 0; }
            if (a.goodMs >= rules::clearForMs && a.goodMoved >= rules::clearMoved) { a.brokenByScore = false; a.score = 0; }
        }
        a.state = a.brokenByScore ? State::Broken : a.score >= rules::doubtAt ? State::Doubtful : State::Healthy;
        a.why = a.state == State::Healthy ? "healthy" : (*a.top ? a.top : (a.brokenByScore ? "did_not_recover" : "doubtful"));
        return a.state;
    }
}
