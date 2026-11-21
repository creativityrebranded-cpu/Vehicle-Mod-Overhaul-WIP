// Everyday NPC trips stripped (2026-11-05 strip; original archived in notes/strip-20261105/archived/traffic/). No NPC takes a vehicle.
#include "everyday.h"
namespace everyday
{
	void Attach(const FvoTrafficApi*, FvoLog_t, FvoTrafficOffer) {}
	int Scan() { return 0; }
	void Reset() {}
}
