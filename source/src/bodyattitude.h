#pragma once
#include "hoverlogic.h"

namespace bodyattitude {
// Publish the chassis orientation before choosing a flight model. Physical flight
// returns early from the step, but its planner needs the same live feedback.
template <class State>
void Publish(State& state, const float* right, const float* forward, const float* up) {
    state.bodyHeading = std::atan2(double(forward[0]), double(forward[1]));
    state.bodyPitch = float(hoverlogic::Pitch(forward[0], forward[1], forward[2]));
    state.bodyRoll = float(hoverlogic::Roll(right[2], up[2]));
}
}
