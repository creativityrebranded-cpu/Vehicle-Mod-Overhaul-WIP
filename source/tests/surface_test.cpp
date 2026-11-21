// SR-1 (2026-11-19): one drivable-surface rule. Can the wheels drive on this hit? Numbers from the project's logs and car spec.
#include "surface.h"
#include "bicycle.h"
#include "planrules.h"
#include <cmath>
#include <cstdio>
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
constexpr double kPi = 3.14159265358979323846;
static double UpDotOfDeg(double deg) { return std::cos(deg * kPi / 180.0); }
int main() {
    {
        surface::FloorSample floor[5] = {{true, true, 20, 8}, {true, true, 19.8, 9}, {true, true, 20.2, 8},
                                        {true, true, 19.97, 8}, {true, true, 20.03, 8}};
        const surface::Car car{};
        const auto ramp = surface::FitProbedFloor(floor, 19.76, 9, false, car);
        Check(ramp.fitted && ramp.clear, "below-road point: a neighbour from the hit's body proves the joined road");
        Check(surface::FitProbedFloor(floor, 19.76, 12, false, car).clear,
              "FC-2: another body under a road all five probes joined clears (a seam under the neighbouring deck piece)");
        floor[4].connected = false;
        Check(!surface::FitProbedFloor(floor, 19.76, 12, false, car).clear, "FC-2: an unmatched body with one probe off the joined road stays");
        floor[4].connected = true;
        Check(!surface::FitProbedFloor(floor, 20.60, 9, false, car).clear, "60 cm wall stays");
        Check(!surface::FitProbedFloor(floor, 20.24, 9, false, car).clear, "low positive obstacle stays");
        Check(!surface::FitProbedFloor(floor, 19.76, 9, true, car).clear, "possible lower road preserves the point");
        floor[2].z += .4;
        Check(!surface::FitProbedFloor(floor, 19.76, 9, false, car).fitted, "uneven stencil lacks a floor fit");
        floor[2].z -= .4;
        floor[3].connected = floor[4].connected = false;
        Check(!surface::FitProbedFloor(floor, 19.76, 9, false, car).fitted, "stencil needs connectivity across both axes");
        floor[3].connected = true;
        floor[0].measured = false;
        Check(!surface::FitProbedFloor(floor, 19.76, 9, false, car).fitted, "missing central sample cannot clear");

        const surface::VerifiedFloorPoint proof{1, 9, -98.98, 1758.13, -79.50, 25};
        Check(proof.Matches(1, 9, -98.98, 1758.13, -79.50, 27), "exact static radar point keeps its proof briefly");
        Check(!proof.Matches(1, 8, -98.98, 1758.13, -79.50, 27), "another body cannot reuse a proof");
        Check(!proof.Matches(1, 9, -99.10, 1758.13, -79.50, 27), "another point on the same body cannot reuse a proof");
        Check(!proof.Matches(1, 9, -98.98, 1758.13, -79.50, 29), "proof expires");
        Check(surface::SweepFloorSupports(.15, car), "near sweep floor keeps unsurveyed pavement continuity");
        Check(!surface::SweepFloorSupports(.64, car) && !surface::SweepFloorSupports(-.64, car),
              "sweep station outside the step band cannot classify the hit");
    }
    using namespace surface;
    const Car car;                                 // gear 3: gripMult 1.0
    Car gear1; gear1.gripMult = 1.6;
    const double maxDeg = std::atan(MaxGrade(car)) * 180.0 / kPi;

    Check(std::fabs(MaxGrade(car) - 0.7) < 1e-9, "default car: MaxGrade is the engine grade 0.7 (friction 1.15 x 0.85 = 0.98 is higher)");
    Check(maxDeg > 34.9 && maxDeg < 35.1, "default car: steepest climbable angle is about 35 deg");
    Check(std::fabs(MinUpDot(car) - 1.0 / std::sqrt(1.49)) < 1e-9, "MinUpDot = 1/sqrt(1+g^2) = 0.819 for grade 0.7");

    // The 23 deg ramp (grade 0.42): its deck normal is 23 deg from up.
    Hit ramp; ramp.haveNormal = true; ramp.upDot = UpDotOfDeg(23); ramp.haveFloor = true; ramp.heightOverFloor = 0.05;
    Check(std::fabs(std::tan(23 * kPi / 180.0) - 0.4245) < 1e-3, "the 23 deg ramp is grade 0.42");
    Check(Judge(ramp, car) == Verdict::Drivable, "the 23 deg ramp is drivable in gear 3 (gripMult 1.0)");
    Check(Judge(ramp, gear1) == Verdict::Drivable, "the 23 deg ramp is drivable in gear 1 (gripMult 1.6)");
    Hit rampNoFloor = ramp; rampNoFloor.haveFloor = false;
    Check(Judge(rampNoFloor, car) == Verdict::Drivable, "the 23 deg ramp face with no floor known is drivable by its trusted normal");

    // A 45 deg rock face.
    Hit rock; rock.haveNormal = true; rock.upDot = UpDotOfDeg(45); rock.haveFloor = true; rock.heightOverFloor = 0.1;
    Check(Judge(rock, car) == Verdict::Solid, "a 45 deg rock face is Solid with the default car (limit about 35 deg)");
    Check(Judge(rock, gear1) == Verdict::Solid, "a 45 deg rock face stays Solid in gear 1: the engine grade 0.7 caps the grip gain");

    // A wall top 1.2 m over the floor with an up-facing normal.
    Hit wallTop; wallTop.haveNormal = true; wallTop.upDot = 1.0; wallTop.haveFloor = true; wallTop.heightOverFloor = 1.2;
    Check(Judge(wallTop, car) == Verdict::Solid, "a wall top 1.2 m over the floor is Solid even with an up-facing normal");

    // A ramp deck hull contact: untrusted normal -0.39 at 0.1 m over the floor.
    Hit deck; deck.haveNormal = true; deck.normalTrusted = false; deck.upDot = -0.39; deck.haveFloor = true; deck.heightOverFloor = 0.1;
    Check(Judge(deck, car) == Verdict::Drivable, "a ramp deck hull contact (untrusted normal -0.39, 0.1 m over the floor) is Drivable");

    // A downward normal: the underside of a deck.
    Hit under; under.haveNormal = true; under.upDot = -0.8; under.haveFloor = true; under.heightOverFloor = 0.1;
    Check(Judge(under, car) == Verdict::Solid, "a trusted downward normal (underside of a deck, upDot -0.8) is Solid");
    Hit underNoFloor = under; underNoFloor.haveFloor = false;
    Check(Judge(underNoFloor, car) == Verdict::Solid, "a trusted downward normal is Solid with no floor known too");

    // Kerb and step.
    Hit kerb; kerb.haveFloor = true; kerb.heightOverFloor = 0.2;
    Check(Judge(kerb, car) == Verdict::Drivable, "a 0.2 m kerb with no normal is Drivable (under the one car's 0.35 m climb, carcap)");
    Hit kerbFlat = kerb; kerbFlat.haveNormal = true; kerbFlat.upDot = 1.0;
    Check(Judge(kerbFlat, car) == Verdict::Drivable, "a 0.2 m kerb top with a flat normal is Drivable");
    Hit step; step.haveFloor = true; step.heightOverFloor = 0.4;
    Check(Judge(step, car) == Verdict::Solid, "a 0.4 m step is Solid (over the one car's 0.35 m climb)");
    Hit lip; lip.haveFloor = true; lip.heightOverFloor = 0.3;
    Check(Judge(lip, car) == Verdict::Drivable, "D2: a 0.3 m lip is Drivable: the one car climbs 0.35 m (was Solid over this file's own 0.25 m step)");
    Check(car.climb == bicycle::Spec{}.climb && car.climb == planbranch::rules::climbHeight && car.climb == carcap::kCar.climb,
          "D2: World's surface rule, the Plan's bicycle and the lane rules read one climb (carcap)");
    Check(MaxGrade(car) == carcap::kCar.MaxGrade(1.0) && MaxGrade(gear1) == carcap::kCar.MaxGrade(1.6), "D2: MaxGrade is the one car record's at the gear's grip");
    Hit stepFlat = step; stepFlat.haveNormal = true; stepFlat.upDot = 1.0;
    Check(Judge(stepFlat, car) == Verdict::Solid, "a 0.4 m step is Solid even with a flat top normal");

    // Doubt.
    Hit nothing;
    Check(Judge(nothing, car) == Verdict::Unknown, "no floor and no normal is Unknown");
    Hit untrustedNoFloor; untrustedNoFloor.haveNormal = true; untrustedNoFloor.normalTrusted = false; untrustedNoFloor.upDot = 0.95;
    Check(Judge(untrustedNoFloor, car) == Verdict::Unknown, "no floor and an untrusted normal (even a flat-looking 0.95) is Unknown");
    Hit deepBelow; deepBelow.haveFloor = true; deepBelow.heightOverFloor = -1.0;
    Check(Judge(deepBelow, car) == Verdict::Below, "SB-1: a hit 1 m below the floor with no normal is Below (dropped), never Drivable");
    Hit deepBelowFlat = deepBelow; deepBelowFlat.haveNormal = true; deepBelowFlat.upDot = 1.0;
    Check(Judge(deepBelowFlat, car) == Verdict::Below, "SB-1: a flat surface 1 m below the floor is Below, never Drivable");

    // SB-1, from the 00:55 log: the ramp pieces' own faces under the measured deck were Solid by their normals before their height was read.
    Hit rampUnderside; rampUnderside.haveNormal = true; rampUnderside.upDot = -0.31; rampUnderside.haveFloor = true; rampUnderside.heightOverFloor = -0.55;
    Check(Judge(rampUnderside, car) == Verdict::Below, "SB-1: the ramp underside 0.55 m under the deck (normal -0.31) is Below, not Solid");
    Hit rampSide = rampUnderside; rampSide.upDot = 0.01; rampSide.heightOverFloor = -2.2;
    Check(Judge(rampSide, car) == Verdict::Below, "SB-1: a vertical ramp side 2.2 m under the deck (normal 0.01) is Below, not Solid");
    Hit lipUnderside = rampUnderside; lipUnderside.heightOverFloor = -0.2;
    Check(Judge(lipUnderside, car) == Verdict::Solid, "SB-1: a downward face within the 0.35 m climb of the floor (carcap) still stops the car");
    Hit wallBelowNoFloor = rampSide; wallBelowNoFloor.haveFloor = false;
    Check(Judge(wallBelowNoFloor, car) == Verdict::Solid, "SB-1: with no floor known a vertical face stays Solid (only a known floor makes it Below)");

    // Grip by gear: more grip raises MaxGrade, never above engineGrade.
    Car slick; slick.mu = 0.5;                     // low friction: friction is the limit, not the engine
    Car slick1 = slick; slick1.gripMult = 1.6;
    Check(std::fabs(MaxGrade(slick) - 0.425) < 1e-9, "friction 0.5 in gear 3: MaxGrade 0.5 x 0.85 = 0.425 (friction limits)");
    Check(MaxGrade(slick1) > MaxGrade(slick), "friction 0.5: gear 1 grip (1.6) raises MaxGrade");
    Check(std::fabs(MaxGrade(slick1) - 0.68) < 1e-9, "friction 0.5: gear 1 MaxGrade is 0.5 x 1.6 x 0.85 = 0.68, under the engine 0.7");
    Check(MaxGrade(gear1) <= car.engineGrade + 1e-12 && std::fabs(MaxGrade(gear1) - 0.7) < 1e-9, "default car gear 1: grip 1.56 is capped at the engine grade 0.7");
    Check(Judge(ramp, slick) == Verdict::Drivable, "the 23 deg ramp (0.4245) is drivable on friction 0.5 in gear 3 (0.425)");
    Hit ramp25 = ramp; ramp25.upDot = UpDotOfDeg(25);
    Check(Judge(ramp25, slick) == Verdict::Solid, "a 25 deg slope (0.466) is Solid on friction 0.5 in gear 3");
    Check(Judge(ramp25, slick1) == Verdict::Drivable, "the same 25 deg slope is Drivable on friction 0.5 in gear 1");

    // The limit is an angle from up: the car spec carries no gravity strength.
    Check(MinUpDot(car) > UpDotOfDeg(36) && MinUpDot(car) < UpDotOfDeg(34), "the limit is an angle from up (between 34 and 36 deg), with no gravity strength in it");

    Check(Name(Verdict::Drivable)[0] == 'D' && Name(Verdict::Solid)[0] == 'S' && Name(Verdict::Unknown)[0] == 'U' && Name(Verdict::Below)[0] == 'B', "Name gives each verdict");

    std::printf(failed ? "FAILED\n" : "ALL PASS\n");
    return failed ? 1 : 0;
}
