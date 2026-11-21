#pragma once
// Pair self-reports (card PF-1, 2026-11-07; design: "self-reporting is a fixed contract": every car's pipeline publishes the same facts
// every update; only Health decides what they mean). Owner: the driving pipeline's Ledger (facts only, never a verdict).
// Facts, each held a short time before it is published so one frame never reads as a fault:
//   asked to move and not moving (planrun's notMoving), off its lane (lateral beyond the lane half width + offLaneMargin),
//   tipped (the rail's tipped flag, or the body's up-z under tippedUpZ). "No physics steps" is already published (api pilotLive).
// A record that is not updated for staleMs publishes nothing (the car left the pipeline: its facts are unknown, not bad).
// Pure: tested offline (tests/selfreport_test.cpp).
#include <cstdint>

namespace selfreport
{
    namespace rules {
        inline constexpr std::uint64_t notMovingHoldMs = 5000;   // on top of planrun's own 3 s window
        inline constexpr std::uint64_t offLaneHoldMs = 5000;
        inline constexpr std::uint64_t tippedHoldMs = 3000;
        inline constexpr std::uint64_t staleMs = 2000;
        inline constexpr double offLaneMargin = 2.0;             // m beyond the lane's half width (the arrival check's margin, ST-A3)
        inline constexpr double tippedUpZ = 0.5;                 // as the arrival check
    }
    enum Bits : unsigned { BitNotMoving = 1, BitOffLane = 2, BitTipped = 4 };
    struct Record { std::uint64_t at = 0, notMovingSince = 0, offLaneSince = 0, tippedSince = 0; };

    inline bool OffLane(double lateral, double laneHalfWidth) { return laneHalfWidth > 0 && (lateral < 0 ? -lateral : lateral) > laneHalfWidth + rules::offLaneMargin; }
    inline bool Tipped(bool railTipped, double upZ) { return railTipped || upZ < rules::tippedUpZ; }

    inline void Update(Record& r, std::uint64_t now, bool notMoving, bool offLane, bool tipped)
    {
        if (r.at && now - r.at > rules::staleMs) r = Record{};  // a gap: start the clocks again
        r.at = now;
        r.notMovingSince = notMoving ? (r.notMovingSince ? r.notMovingSince : now) : 0;
        r.offLaneSince = offLane ? (r.offLaneSince ? r.offLaneSince : now) : 0;
        r.tippedSince = tipped ? (r.tippedSince ? r.tippedSince : now) : 0;
    }
    // The published facts now (0: none, or the record is stale).
    inline unsigned Facts(const Record& r, std::uint64_t now)
    {
        if (!r.at || now - r.at > rules::staleMs) return 0;
        unsigned b = 0;
        if (r.notMovingSince && now - r.notMovingSince >= rules::notMovingHoldMs) b |= BitNotMoving;
        if (r.offLaneSince && now - r.offLaneSince >= rules::offLaneHoldMs) b |= BitOffLane;
        if (r.tippedSince && now - r.tippedSince >= rules::tippedHoldMs) b |= BitTipped;
        return b;
    }
    inline const char* Name(unsigned b) { return b & BitTipped ? "tipped" : b & BitOffLane ? "off_its_lane" : b & BitNotMoving ? "asked_to_move_not_moving" : "none"; }
}
