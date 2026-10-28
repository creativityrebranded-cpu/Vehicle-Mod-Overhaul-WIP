#pragma once
#include <cstdint>

namespace boardinggate {
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
