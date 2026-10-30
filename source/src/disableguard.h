#pragma once
// A guard on the game's actor disable step (2026-10-29: two crashes in a row, 1 s after the traffic pool made new drivers in New
// Atlantis). The game's reference task queue (FUN_140C109C0) runs queued disables through the reference's vtable +0x7E8; for
// actors that is FUN_141958630, which reads actor +0x358 without a check (Starfield.exe+0x1958672, read of 0x28 with +0x358
// null). An actor whose +0x358 is still empty is not disabled (it stays as it is) and the case is logged with its id, base
// and whether it is a traffic-pool driver, so the caller can be found.
namespace disableguard
{
	bool Install();  // startup, main thread
}
