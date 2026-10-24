#pragma once
#include <algorithm>
#include <cmath>
#include "freeroll.h"
#include "vtolai.h"
#include "vtolphysics.h"

// The player's physical VTOL with Flight assist on (2026-10-24, the user: roll and turn should combine). Before this the
// camera turned the nose flat (no bank; the side thrusters cancelled the skid) and A / D banked without the nose following.
// Now, the AI pilots' coordinated turn (vtolai.h) for the player:
//  - turning with the camera banks the chassis as far as a level turn at this speed needs (vtolai::TurnBank);
//  - banking with A / D or the stick also turns the nose into the curve (the yaw a level turn at that bank makes);
//  - yaw turns about world up, so a banked turn does not drag the nose down.
// Outside kAssistBand of roll (rolled past 60 degrees, aerobatics), with assist off, or with no forward speed to turn
// with, vehicleflight keeps its body-axis controls.
namespace vtolassist {
constexpr double kAssistBand = freeroll::kAssistBand;  // the band freeroll's assist levels a released roll in
constexpr double kTurnSpeed = 8.0;     // m/s: below this the bank's yaw fades out (no curve to follow when hovering)
constexpr double kGravity = vtolai::kGravity;

struct Input {
    vtol::Vec right{1,0,0}, forward{0,1,0}, up{0,0,1};
    double pitch = 0, roll = 0;          // nose up +, right side down +
    double heading = 0;                  // atan2(forward.x, forward.y): the camera's frame
    double speed = 0;                    // horizontal m/s
    bool haveView = false; double view = 0;      // the camera's world heading
    bool haveAim = false; double pitchAim = 0;   // the camera's pitch
    double rollIn = 0;                   // -1 .. 1 (analog): A / D or the stick
    double rollRate = 1, yawLimit = 1, pitchLimit = 1;  // hull rates
    double camGain = 2, levelRate = 1.2;
    bool nearGround = false, landing = false;
};
inline double Wrap(double a) { return std::isfinite(a) ? std::remainder(a, 6.283185307179586) : 0.0; }
// Whether the coordinated controls apply (assist on is the caller's part).
inline bool Applies(double roll) { return std::isfinite(roll) && std::fabs(roll) < kAssistBand; }

// The yaw rate a level turn at this bank and speed makes: v * yaw = g * tan(bank), faded out below kTurnSpeed.
inline double BankYaw(double roll, double speed, double limit)
{
    if (!std::isfinite(roll) || !std::isfinite(speed) || speed <= 0) return 0.0;
    const double fade = std::clamp(speed / kTurnSpeed, 0.0, 1.0);
    return std::clamp(kGravity * std::tan(std::clamp(roll, -vtolai::kMaxBank, vtolai::kMaxBank)) / std::max(speed, kTurnSpeed) * fade, -limit, limit);
}

struct Rates { vtol::Vec world; double yaw = 0, bankWant = 0, rollRate = 0; };
inline Rates Solve(const Input& in)
{
    Rates out;
    // The camera's heading and pitch, measured in the world (yaw about world up, so a bank never mixes into them).
    const double headingError = in.haveView ? Wrap(in.view - in.heading) : 0.0;
    double camYaw = std::clamp(in.camGain * headingError, -in.yawLimit, in.yawLimit);
    double pitch = -in.pitch * in.levelRate;
    if (in.haveAim) pitch = in.camGain * (in.pitchAim - in.pitch);
    pitch = std::clamp(pitch, -in.pitchLimit, in.pitchLimit);
    double roll;
    double yaw;
    if (in.landing || (in.nearGround && in.rollIn == 0.0)) {
        // Close to the ground the chassis levels (freeroll's rule), and the camera alone turns it.
        roll = std::clamp(in.levelRate * Wrap(0.0 - in.roll), -in.rollRate, in.rollRate);
        yaw = camYaw;
    }
    else if (in.rollIn != 0.0) {
        // The player banks: the nose follows the curve that bank makes; the camera's turn adds to it.
        roll = std::clamp(in.rollIn, -1.0, 1.0) * in.rollRate;
        yaw = std::clamp(camYaw + BankYaw(in.roll, in.speed, in.yawLimit), -in.yawLimit, in.yawLimit);
    }
    else {
        // The camera turns: bank as far as a level turn at this speed needs (the AI pilots' rule).
        out.bankWant = vtolai::TurnBank(in.speed, camYaw);
        roll = std::clamp(vtolai::kBankGain * Wrap(out.bankWant - in.roll), -in.rollRate, in.rollRate);
        yaw = camYaw;
    }
    out.yaw = yaw;
    out.rollRate = roll;
    out.world = in.right * pitch + in.forward * roll + vtol::Vec{0, 0, -yaw};
    return out;
}
}
