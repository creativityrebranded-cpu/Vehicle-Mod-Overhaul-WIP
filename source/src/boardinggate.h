#pragma once
#include <cstdint>

namespace boardinggate {
enum class PassengerAction { Wait, Start, Complete, Cancel };
struct PassengerAttempt {
    std::uint64_t at = 0;
    bool asked = false;
    PassengerAction Step(std::uint64_t now, bool complete, bool invalid, bool transitioning) {
        if (complete) return PassengerAction::Complete;
        if (invalid || now < at || now - at >= 10000) return PassengerAction::Cancel;
        // Give the native Activate handler a frame to start; never build a second interaction over it.
        if (transitioning) asked = true;
        if (asked || now - at < 150) return PassengerAction::Wait;
        asked = true;
        return PassengerAction::Start;
    }
};
inline std::uintptr_t PassengerKeyword(std::uintptr_t vehicle, bool asDriver,
    std::uintptr_t pinnedVehicle, std::uintptr_t pinnedKeyword,
    std::uintptr_t facingVehicle, std::uintptr_t facingKeyword) {
    if (asDriver || !vehicle) return 0;
    return vehicle == pinnedVehicle ? pinnedKeyword : vehicle == facingVehicle ? facingKeyword : 0;
}
enum class Refusal { None, Reserved, Occupied, Wrecked };
inline Refusal BeforeEntry(bool player, bool vehicle, bool reserved, bool occupied, bool wrecked) {
    if (!player || !vehicle) return Refusal::None;
    if (wrecked) return Refusal::Wrecked;
    if (occupied) return Refusal::Occupied;
    return reserved ? Refusal::Reserved : Refusal::None;
}

struct Context { std::uintptr_t actor=0, vehicle=0; };
// The sit action can nest. Actor identity is local to this callback/thread,
// never inferred from a recent Activate press or an unrelated vehicle's job.
class Scope {
    Context& current_;
    Context previous_;
public:
    Scope(Context& current, Context next): current_(current), previous_(current) { current_=next; }
    ~Scope() { current_=previous_; }
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
};
enum class Completion { Player, Actor, Suppress };
inline Completion Finish(Context context, std::uintptr_t player, std::uintptr_t vehicle, bool restricted) {
    if (context.actor && context.vehicle==vehicle) {
        return context.actor==player ? Completion::Player : Completion::Actor;
    }
    // No late teardown of an already-started player animation. Unknown callbacks
    // into the player-only helper may not commandeer an NPC-reserved vehicle.
    return restricted ? Completion::Suppress : Completion::Player;
}
template<class Native, class Reject>
void Begin(Refusal refusal, Native native, Reject reject) {
    if (refusal==Refusal::None) native();
    else reject(refusal);
}
template<class Native, class Npc>
void Complete(Completion completion, Native native, Npc npc) {
    if (completion==Completion::Player) native();
    else if (completion==Completion::Actor) npc();
}
}
