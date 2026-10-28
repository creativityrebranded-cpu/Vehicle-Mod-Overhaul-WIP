#pragma once

// C10810 only cancels queued state changes. B4A9C0 is the native enable
// operation that clears Disabled and requests loading the reference's model.
// Keep the ordering shared with tests: cancellation alone is not an enable.
namespace placementenable {
template<class Engine> bool Restore(Engine& engine)
{
    engine.CancelRequests();
    if (engine.Disabled()) engine.Enable();
    return !engine.Disabled();
}
enum class Wait { Pending, Ready, Timeout };
inline Wait BeforeSwap(bool disabled, bool loaded, unsigned long long elapsed)
{
    if (disabled && !loaded) return Wait::Ready;
    return elapsed >= 3000 ? Wait::Timeout : Wait::Pending;
}
}
