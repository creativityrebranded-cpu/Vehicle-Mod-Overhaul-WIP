#pragma once
// Read-only view of a vehicle's plan for the F7 Traffic Manager and the HUD (design section 9 "Ledger", work order step 6).
// One line: what the plan is doing, what holds its speed, how wide Avoid may look, who owns the car, and the last branch that lost.
// Implemented in vehicleflight.cpp (main thread, where the plan lives). Never changes anything.
#include <cstdint>
namespace vehicleflight {
// false: this vehicle has no plan now (not a plan car, or planArbiter off). `out` always ends with a 0 when n > 0.
bool PlanLedgerLine(std::uint32_t vehicleFormId, char* out, int n);
// HV-2 (notes/route-items-20261107/CHANGE_CARDS.md): every branch the plan weighed in its last update, as world points, with why the losers
// lost. Read-only, for F7 to draw; returns how many were written (0: no plan now).
constexpr int kPlanBranchPoints = 6;
struct PlanBranchView { const char* family = ""; bool picked = false, feasible = false, reverse = false; float cost = 0, lost = 0; char blockedBy[40]{}, speedWhy[56]{};
    int points = 0; float x[kPlanBranchPoints]{}, y[kPlanBranchPoints]{}, z[kPlanBranchPoints]{}; };
int PlanBranchesView(std::uint32_t vehicleFormId, PlanBranchView* out, int max);
}
