#pragma once

// Name of the DLL this code is built into (each plugin's main defines it): log file <Name>.log, telemetry files
// <prefix>telemetry-<time>.jsonl. The core keeps the historical names (SeamlessPlanets.log, telemetry-*.jsonl).
namespace plugininfo
{
	const wchar_t* Name();             // e.g. L"SeamlessPlanets", L"SeamlessShip"
	const wchar_t* TelemetryPrefix();  // L"" for the core, L"ship-" etc. for the features
}
