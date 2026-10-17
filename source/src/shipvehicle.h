#pragma once

// Preview of the home ship exterior on the currently driven vehicle. The actual vehicle body remains the
// physics owner; this visual mode makes no claim that the hull's outer collision has been installed.
namespace shipvehicle
{
	void Tick();
	void Reset();
}
