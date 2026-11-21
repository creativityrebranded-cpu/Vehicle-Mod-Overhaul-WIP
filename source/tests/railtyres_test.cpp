// TY-1 tyre mode pedals (src/railtyres.h) against a simple car: throttle pushes, brake slows while moving forward and backs a stopped
// car up (the game's rule), drag, and an optional slope. Offline logic only: never proof of behaviour in game.
#include "railtyres.h"
#include <cstdio>
#include <cmath>
static int failed = 0, checks = 0;
static void Check(bool ok, const char* what) { ++checks; std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }

struct Car { double v = 0; bool backedUp = false; };
// One step of the toy car: 3.5 m/s^2 full throttle, 6 m/s^2 full brake while moving forward, brake at rest = reverse at 2 m/s^2.
static void Physics(Car& c, double accel, double brake, double slope, double dt) {
    double a = 3.5 * accel - 0.06 * c.v * std::fabs(c.v) * 0.1 - 0.15 * c.v - 9.8 * slope;
    if (brake > 0) { if (c.v > 0.2) a -= 6.0 * brake; else { a -= 2.0 * brake; } }
    c.v += a * dt;
    if (c.v < -0.2) c.backedUp = true;
}
static Car Run(double want, double seconds, double slope = 0, double start = 0, double steer = 0, railtyres::Out* last = nullptr) {
    Car c; c.v = start; railtyres::State s; const double dt = 1.0 / 60;
    for (double t = 0; t < seconds; t += dt) { const auto o = railtyres::Step(s, want, c.v, false, steer, dt); Physics(c, o.accel, o.brake, slope, dt); if (last) *last = o; }
    return c;
}

int main() {
    Check(std::fabs(Run(10, 20).v - 10) < 1.0, "flat road: reaches and holds 10 m/s within 1 m/s");
    Check(std::fabs(Run(5, 20).v - 5) < 1.0, "flat road: holds 5 m/s");
    Check(std::fabs(Run(10, 30, 0.08).v - 10) < 1.0, "8 % climb: the held throttle keeps 10 m/s");
    {   const auto c = Run(0, 10, 0, 12); Check(std::fabs(c.v) < 0.4 && !c.backedUp, "stop from 12 m/s: stops and never backs up"); }
    {   railtyres::Out o; Run(0, 5, 0, 0, 0, &o); Check(o.brake == 0 && o.accel == 0, "at rest told to stop: no pedal (a held brake would reverse it)"); }
    {   railtyres::State s; const auto o = railtyres::Step(s, 0, -1.0, false, 0, 0.016); Check(o.accel > 0 && o.brake == 0, "rolling back while stopped: a little throttle, never brake"); }
    {   railtyres::State s; const auto o = railtyres::Step(s, 0, 2.0, false, 0.9, 0.016); Check(o.brake <= railtyres::rules::lockBrakeMax + 1e-9, "near full lock at a crawl: brake capped (no side-on flip-flop)"); }
    {   railtyres::State s; const auto o = railtyres::Step(s, 3, 0, true, 0, 0.016); Check(o.brake == 1 && o.accel == 0, "reverse: the brake pedal (the game backs up once stopped)"); }
    {   const auto c = Run(4, 15, 0, 14); Check(std::fabs(c.v - 4) < 1.0 && !c.backedUp, "slow from 14 to 4 m/s without backing up"); }
    {   // Stop then pull away again.
        Car c; railtyres::State s; const double dt = 1.0 / 60;
        for (double t = 0; t < 10; t += dt) { const auto o = railtyres::Step(s, 8, c.v, false, 0, dt); Physics(c, o.accel, o.brake, 0, dt); }
        for (double t = 0; t < 8; t += dt) { const auto o = railtyres::Step(s, 0, c.v, false, 0, dt); Physics(c, o.accel, o.brake, 0, dt); }
        const bool stopped = std::fabs(c.v) < 0.4;
        for (double t = 0; t < 10; t += dt) { const auto o = railtyres::Step(s, 8, c.v, false, 0, dt); Physics(c, o.accel, o.brake, 0, dt); }
        Check(stopped && std::fabs(c.v - 8) < 1.0 && !c.backedUp, "stops without backing up, then pulls away to 8 m/s"); }
    {   railtyres::State s; const auto o = railtyres::Step(s, NAN, 1, false, 0, 0.016); Check(o.accel == 0 && o.brake == 0, "no numbers: no pedal"); }
    // TY-3 signed pedal (one value for both directions). Toy car: pedal > 0 pushes forward; pedal < 0 brakes forward motion, reverses otherwise.
    {   auto runS = [](double want, double seconds, double start, double slope, bool* wrongWay) {
            Car c; c.v = start; railtyres::State s; const double dt = 1.0 / 60; double prev = start;
            for (double t = 0; t < seconds; t += dt) { const auto o = railtyres::StepSigned(s, want, c.v, 0, dt);
                c.backedUp = false; Physics(c, o.accel, o.brake, slope, dt);
                if (wrongWay && want > 0 && prev > 0.5 && c.v < -0.3) *wrongWay = true; prev = c.v; }
            return c; };
        bool w = false;
        Check(std::fabs(runS(10, 20, 0, 0, &w).v - 10) < 1.0 && !w, "signed: holds 10 m/s forward");
        Check(std::fabs(runS(-3, 20, 0, 0, nullptr).v + 3) < 0.7, "signed: backs up at the wanted 3 m/s");
        Check(std::fabs(runS(-3, 20, -6, 0, nullptr).v + 3) < 0.7, "signed: backing too fast (6 m/s) slows to 3 with throttle");
        Check(std::fabs(runS(-3, 20, 8, 0, nullptr).v + 3) < 0.7, "signed: moving forward at 8, told to back up: brakes, then reverses at 3");
        Check(std::fabs(runS(0, 10, 12, 0, nullptr).v) < 0.4, "signed: stops from 12 m/s forward");
        Check(std::fabs(runS(0, 10, -4, 0, nullptr).v) < 0.4, "signed: stops from 4 m/s backward");
        Check(std::fabs(runS(10, 30, 0, 0.08, nullptr).v - 10) < 1.0, "signed: holds 10 m/s on an 8 % climb");
        { railtyres::State s; const auto o = railtyres::StepSigned(s, 0, 0.1, 0, 0.016); Check(o.pedal == 0, "signed: at rest told to stop: no pedal"); }
        {   railtyres::State a, b, c;
            const auto flat = railtyres::StepSigned(a, 2.0, 0.3, 0, 0.016, 0.0);
            const auto ramp = railtyres::StepSigned(b, 2.0, 0.3, 0, 0.016, 0.42);
            const auto down = railtyres::StepSigned(c, 2.0, 0.3, 0, 0.016, -0.42);
            Check(flat.pedal < .6 && ramp.pedal >= .99, "SP-1: on the 23 deg ramp the push is full at once (flat: the slow build-up)");
            Check(std::fabs(down.pedal - flat.pedal) < 1e-9, "SP-1: downhill adds no push");
        }
        { railtyres::State s; const auto o = railtyres::StepSigned(s, 0, 2.0, 0.9, 0.016); Check(o.brake <= railtyres::rules::lockBrakeMax + 1e-9, "signed: brake capped near full lock at a crawl"); }
        { railtyres::State s; for (int k = 0; k < 120; ++k) railtyres::StepSigned(s, 10, 5, 0, 0.016); railtyres::StepSigned(s, -3, 5, 0, 0.016);
          Check(s.i <= 0, "signed: the held part does not carry over into reverse"); }
        { railtyres::State s; const auto o = railtyres::StepSigned(s, 10, 10.5, 0, 0.016); Check(o.brake == 0, "signed: a little over the wanted speed: no brake (dead band)"); }
        { railtyres::State s; const auto o = railtyres::StepSigned(s, NAN, 1, 0, 0.016); Check(o.pedal == 0, "signed: no numbers: no pedal"); }
        {   // NaN held values (the wrapped-gap fade, 2026-11-19): the pedal stays a number and the ramp push still works.
            railtyres::State s; s.i = NAN; s.fwdHeld = INFINITY; s.revHeld = -NAN;
            const auto back = railtyres::StepSigned(s, -2.0, 0.0, 0, 0.016);
            railtyres::State r; r.i = INFINITY; r.fwdHeld = NAN; r.revHeld = -INFINITY;
            const auto ramp = railtyres::StepSigned(r, 2.0, 0.0, 0, 0.016, 0.42);
            Check(std::isfinite(back.pedal) && back.pedal < 0 && std::isfinite(s.i) && std::isfinite(s.fwdHeld) && std::isfinite(s.revHeld),
                  "NaN/inf held values: backing up still gives a finite brake pedal and clean state");
            Check(std::isfinite(ramp.pedal) && ramp.pedal >= .99, "NaN/inf held values: 0.42 grade, 2 m/s short: full throttle");
        }
        {   // TM-1: on a hill the held throttle builds up; after a short stop (and a short back-up) the push comes back at once.
            railtyres::State s; for (int k = 0; k < 600; ++k) railtyres::StepSigned(s, 3, 1.0, 0, 0.016);  // 9.6 s held 2 m/s short: i builds
            const double built = s.i;
            for (int k = 0; k < 30; ++k) railtyres::StepSigned(s, 0, 0, 0, 0.016);   // 0.5 s stop
            for (int k = 0; k < 30; ++k) railtyres::StepSigned(s, -1.8, -0.5, 0, 0.016);  // 0.5 s backing
            const auto o = railtyres::StepSigned(s, 3, 0, 0, 0.016);
            Check(built > .5 && o.pedal > built * .8, "TM-1: the built-up throttle comes back after a stop and a back-up (not from zero)");
            railtyres::State t = s; for (int k = 0; k < 1500; ++k) railtyres::StepSigned(t, 0, 0, 0, 0.016);  // 24 s standing
            { railtyres::State fresh; const double f0 = railtyres::StepSigned(fresh, 3, 0, 0, 0.016).pedal; Check(railtyres::StepSigned(t, 3, 0, 0, 0.016).pedal < f0 + built * .05, "TM-1: it fades after standing a long time (back to a fresh start)"); }
            Check(railtyres::StepSigned(s, -1.8, 0, 0, 0.016).pedal <= 0, "TM-1: the forward push never carries into reverse"); } }
    std::printf("railtyres_test: %d checks, %d failed\n%s\n", checks, failed, failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
