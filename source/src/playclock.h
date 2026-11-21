#pragma once
// W1 (spawn one-place cards, 2026-11-07): the one answer to "is play time running?". Owner: the spawn flow's Window (step 1) reads it;
// every clock in the core and the addon that must not count paused time reads this, never its own mix of presentation, menus and photo
// mode. Bits: kCovered (loading screen or arrival cover), kMenu (a menu holds gameplay), kPhoto (photo mode: physics stopped).
// Play time runs only when no bit is set. Main or any thread: each part is an atomic read.
#include "inputgate.h"
#include "routeedit.h"
#include "trafficarrival.h"

namespace playclock
{
    enum Bits : unsigned { kCovered = 1, kMenu = 2, kPhoto = 4 };
    inline unsigned Now()
    {
        return (trafficarrival::Covered() ? kCovered : 0u) | (!inputgate::GameplayAllowed() ? kMenu : 0u) | (routeedit::PhotoOpen() ? kPhoto : 0u);
    }
    inline bool Running() { return Now() == 0; }
}
