#pragma once

// Addresses for Starfield 1.16.244 (Steam), as RVAs from the image base.
// Generated from the Ghidra project (seemless/ghidra). Every code address carries the first 16 bytes
// of the function so the plugin can refuse to run on any other build.

#include <cstdint>

namespace addr
{
	inline constexpr std::uint32_t kRuntimeVersion = 0x01100F40;  // 1.16.244, MAKE_EXE_VERSION(1, 16, 244)

	struct Signature
	{
		const char*   name;
		std::uint32_t rva;
		std::uint8_t  bytes[16];
	};

	inline constexpr Signature kSignatures[] = {
		{ "PlanetManagerUpdatePlayer", 0x0078C5F0, { 0x40, 0x55, 0x57, 0x48, 0x83, 0xEC, 0x58, 0xC4, 0xC1, 0x7B, 0x10, 0x40, 0x30, 0xC5, 0xFB, 0x11 } },
		{ "PlayerBorderCheck", 0x01A33CB0, { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56 } },
		{ "HavokShiftWorld", 0x03060A50, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x0F, 0x28, 0x02, 0x48, 0x8D, 0x79 } },
		{ "OriginShiftEventSource", 0x00691580, { 0x48, 0x83, 0xEC, 0x28, 0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00, 0xBA, 0xB8, 0x00 } },
		{ "MoveReference", 0x02062F50, { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x4C, 0x89, 0x48, 0x20, 0x48, 0x89, 0x50, 0x10, 0x55 } },
		{ "ForEachRefInCell", 0x00B1E190, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 } },
		{ "EventNotify", 0x022C9300, { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x48, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54 } },
		{ "RWLockWrite", 0x022CC3D0, { 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0xF1 } },
		{ "RWLockWriteRelease", 0x002C45A0, { 0x8B, 0x41, 0x04, 0x48, 0x8B, 0xD1, 0x48, 0x83, 0xC1, 0x04, 0x25, 0x00, 0x00, 0xC0, 0xFF, 0x90 } },
		{ "GetPlanetGrid", 0x0096B8E0, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x57 } },
		{ "OffsetPlanetCoord", 0x0096F5C0, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 } },
		{ "MapInsertKey", 0x00310090, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x4C } },
		{ "HudNotificationSource", 0x014915A0, { 0x48, 0x83, 0xEC, 0x28, 0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00, 0xBA, 0xB8, 0x00 } },
		{ "FixedStringCreate", 0x028CBA80, { 0x44, 0x88, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55 } },
		{ "SetCustomMarker", 0x01A43220, { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC } },
		{ "ShipOfRef", 0x02116840, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0x81, 0xB0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0 } },
		{ "CondGetActorInShipPilotSeat", 0x00C063A0, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x49, 0x8B, 0xF9, 0x48, 0x8B, 0xDA } },
		{ "CondIsInSpace", 0x00C05F70, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xDB, 0x49, 0x8B, 0xF9, 0x41 } },
		{ "PlanetManagerTryLock", 0x0078F480, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA, 0x48, 0x89, 0x11 } },
		{ "PlanetManagerShiftModules", 0x0078C190, { 0x48, 0x89, 0x6C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0xB6, 0x41, 0x20, 0x48, 0x8B } },
		{ "PlanetManagerShiftCommit", 0x0078C290, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0xB6, 0x81, 0x8C, 0x01, 0x00 } },
		{ "GlobePublish", 0x01684A10, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 } },
		{ "WorldToLatLong", 0x0078D1F0, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0xC5, 0xFA, 0x10, 0x0D, 0xEE, 0x6C, 0x77, 0x04, 0xC4, 0xC1 } },
		{ "GetPlayerHomeShip", 0x01A6BD50, { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48 } },
		{ "RefRelease", 0x00384600, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48 } },
		{ "GridUpdate", 0x00576530, { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x48 } },
		{ "ToggleCollision", 0x00C63FF0, { 0x48, 0x83, 0xEC, 0x28, 0x4D, 0x85, 0xC0, 0x74, 0x4C, 0x33, 0xD2, 0x41, 0x80, 0x78, 0x2E, 0x4B } },
		{ "GroundRayCast", 0x00583B70, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 } },
		{ "FixedStringRelease", 0x028CAEA0, { 0x41, 0x54, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x83, 0x39, 0x00, 0x4C, 0x8B, 0xE1, 0x0F, 0x84, 0x96 } },
		{ "ContentLandingBlock", 0x018ABD60, { 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54 } },
		{ "CreateReference", 0x005A0520, { 0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x55 } },
		{ "HandleLookup", 0x028CCC40, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 } },
		{ "MarkerPost1", 0x00B36C40, { 0x40, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x80 } },
		{ "MarkerPost2", 0x00B3F5D0, { 0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x0F, 0xB7, 0x41, 0x2C, 0x48, 0x8B, 0xF1 } },
		{ "MarkerPost3", 0x0187BB60, { 0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x0F, 0xB6, 0xFA, 0x49 } },
		{ "ContentRemoveOverlay", 0x018B40B0, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0x51 } },
		{ "RWLockWriteB", 0x022C8D10, { 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48 } },
		{ "QuestSystemStatus", 0x00ADA760, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x4C } },
		{ "GameSingleton", 0x023FF640, { 0x48, 0x83, 0xEC, 0x28, 0xBA, 0xB8, 0x00, 0x00, 0x00, 0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00 } },
		{ "PlayableAreaTest", 0x003A5C20, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 } },
		{ "PlanetGridFor", 0x0096B8E0, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x57 } },
		{ "CoordFromLatLong", 0x0096EB90, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x49, 0x8B, 0xC0, 0x48 } },
		{ "CoordOffset", 0x0096F940, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48 } },
		{ "SaveLocationWriter", 0x018286F0, { 0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48 } },
		{ "AlignedOrigin", 0x0096EFB0, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x49, 0x8B, 0xC0 } },
		{ "DisableReference", 0x00C10660, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x48, 0x8B, 0xEC } },
		{ "ContentInstanceFor", 0x018AB8D0, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x4C, 0x89, 0x4C, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10, 0x55 } },
		{ "ContentPlaceRecord", 0x018B4050, { 0x48, 0x83, 0xEC, 0x38, 0x44, 0x8B, 0x41, 0x2C, 0x41, 0x8B, 0xD0, 0x83, 0xEA, 0x01, 0x74, 0x05 } },
		{ "MovementControllerOf", 0x012AA710, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 } },
		{ "MovementAgentByName", 0x02734DD0, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48 } },
		{ "GalaxyPositionOf", 0x0090D160, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 } },
		{ "PlayerFlightControlsName", 0x0068E780, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xBA, 0xB8, 0x00, 0x00, 0x00, 0x65, 0x48, 0x8B, 0x04, 0x25 } },
		{ "ShowMenu", 0x025556A0, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x44, 0x89, 0x44, 0x24, 0x18, 0x56 } },
		{ "TakeoffMenuName", 0x012AA320, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xBA, 0xB8, 0x00, 0x00, 0x00, 0x65, 0x48, 0x8B, 0x04, 0x25 } },
		{ "SpaceshipHudMenuName", 0x009FA560, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xBA, 0xB8, 0x00, 0x00, 0x00, 0x65, 0x48, 0x8B, 0x04, 0x25 } },
		{ "ShipSetLanded", 0x02117D00, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x0F, 0xB6, 0xC2, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x15, 0xDD } },
		{ "ShipIsLanded", 0x02119F60, { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x91, 0xB0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF9 } },
		{ "ShipSystemUpdate", 0x02169100, { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x70 } },
		{ "ForceThirdPerson", 0x01E8C840, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x49, 0x10, 0x48, 0x85, 0xC9 } },
		{ "ShipTakeOff", 0x021199B0, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 } },
		{ "ShipPilotActor", 0x021159B0, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x1D, 0x6F, 0xD8, 0xE2 } },
		{ "RefEnableTask", 0x00B4A9C0, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41 } },
		{ "VehicleStep", 0x023CB260, { 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41 } },
		{ "PlayerVehicle", 0x0219BB40, { 0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9, 0x33, 0xF6, 0xB2, 0xA5, 0x48 } },
		{ "LoadedCellDBFlush", 0x003655A0, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x33 } },
		{ "MarkerFindSlot", 0x00B9D650, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41 } },
		{ "MarkerClaimSlot", 0x00B9D040, { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55 } },
		{ "ShipLandAtMarker", 0x021219B0, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48 } },
		{ "RequestSave", 0x0184FF90, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x41 } },
		{ "EnableReference", 0x00C10810, { 0x48, 0x85, 0xC9, 0x0F, 0x84, 0xD0, 0x00, 0x00, 0x00, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x48 } },
		{ "ActorPositionUpdate", 0x01B031F0, { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x83 } },
		{ "PlanetManagerInit", 0x0078B030, { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56 } },
		{ "PlanetCoordDecode", 0x0096FB40, { 0x40, 0x53, 0x55, 0x57, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x58, 0xC6, 0x02, 0x00, 0x45, 0x33, 0xFF } },
		{ "SaveGame", 0x01814220, { 0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48 } },
		{ "ActorSetPosition", 0x0193B410, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 } },
		{ "RefSetPosition", 0x00B4E410, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B } },
		{ "LoadedCellsVisit", 0x005781C0, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x4C } },
		{ "LoadCellRun", 0x0041F8F0, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x89, 0xC0, 0x00, 0x00, 0x00 } },
		{ "CellLoadFinish", 0x0041EFA0, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 } },
		{ "TerrainBlockGenerate", 0x0098A330, { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x4C, 0x89, 0x48, 0x20, 0x48, 0x89, 0x50, 0x10, 0x48 } },
		{ "TerrainBlockEnsure", 0x00978280, { 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48 } },
		{ "TerrainPageStitch", 0x0098E8D0, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 } },
		{ "CellLoadKey", 0x00422600, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 } },
		{ "PlanetCellKey", 0x00424C50, { 0x40, 0x53, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B, 0x9A, 0x10, 0x03, 0x00, 0x00 } },
		{ "ExitPilotSeat", 0x0155C7A0, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x1D, 0x13, 0x16, 0xC8, 0x04, 0xE8, 0xAE, 0xDD } },
	};

	// Engine functions (RVAs; all covered by kSignatures above).
	inline constexpr std::uint32_t kTaskNodeStatsPtr = 0x05E758E8;     // global: task-graph node stats; +0x508 int[priority*7+type] live nodes (type 6 = LoadedCellDB::LoadCell), +0x554 all
	inline constexpr std::uint32_t kActorPositionUpdate = 0x01B031F0;    // (actor, float pos[3]): actor virtual position update (via FUN_141953F60, event path); writes +0x8C directly
	inline constexpr std::uint32_t kPlanetManagerInit = 0x0078B030;      // (mgr, int* desc, flag, u64* packedOrigin): planet manager setup on load/landing
	inline constexpr std::uint32_t kPlanetCoordDecode = 0x0096FB40;      // (grid, out coord 0x2C, u64 packed): decode a packed planet cell id; zeroed output on grid mismatch
	inline constexpr std::uint32_t kSaveGame = 0x01814220;               // BGSSaveLoadGame::SaveGame (all saves; started by the save task FUN_141821B70)
	inline constexpr std::uint32_t kActorSetPosition = 0x0193B410;       // (actor, float pos[4], char warp): position; warp also sets the character controller (+0x228 -> +8 -> +0x458 vtbl +0x240), zeroes its velocity and re-syncs its proxy
	inline constexpr std::uint32_t kRefSetPosition = 0x00B4E410;         // (ref, float pos[3]): writes ref +0x8C..+0x94; the body->actor sync and the player movement update end here
	inline constexpr std::uint32_t kLoadedCellsVisit = 0x005781C0;
	inline constexpr std::uint32_t kLoadedCellsVisitPerFrameReturn = 0x017C8B5B;  // return address of the per-frame call (FUN_1417C8B50); other callers: grid relocation FUN_140666C00, load grid rebuild, FUN_14059BA60, FUN_141844B30      // (unused, char): per-frame visit of the loaded cells in the game update, before the body->actor sync
	inline constexpr std::uint32_t kLoadCellRun = 0x0041F8F0;           // LoadedCellDB::LoadCell vtbl+0x10 run(task): task +0xC0 DB entry (key at +8/+0xC/+0x10), +0xD0 worldspace, +0xD8/+0xDC request x, y
	inline constexpr std::uint32_t kLoadCellRequest = 0x00420410;       // LoadedCellDB task producer (db, priority, worldspace, int16 x, int16 y, ...)
	inline constexpr std::uint32_t kLoadCellBuild = 0x0041F120;         // LoadCell publication path: key, worldspace, coords, result, entry state, flags
	inline constexpr std::uint32_t kPlanetKeyTag = 0x05E7CF24;
	inline constexpr std::uint32_t kBlockSizeStitching = 0x05EA66F0;      // uint32 setting value uBlockSize:Stitching
	inline constexpr std::uint32_t kBlockMergeZoneStitching = 0x05EA6770; // uint32 setting value uBlockMergeZoneSize:Stitching          // uint32 global: LoadedCellDB key type tag of planet cells (key word 1)
	inline constexpr std::uint32_t kCellLoadFinish = 0x0041EFA0;        // (worldspace, cell, int coords[2], arg, char): LoadCell job end step
	inline constexpr std::uint32_t kTerrainBlockGenerate = 0x0098A330;  // (page, coords, page, coords, page, coords, int*, char, char): block build from 3 pages
	inline constexpr std::uint32_t kTerrainCollisionBuild = 0x00990850;  // (terrain manager, int coords[2], cell): LoadCell worldspace step; heightfield collision into the cell's physics systems; nothing when the source rectangle rejects the cell
	inline constexpr std::uint32_t kTerrainSourceAccepts = 0x009A0EA0;   // (terrain manager, int coords[2]) -> bool: 0 <= coords - source origin (+0xE8 -> +8 vtbl +0x88) < dims (vtbl +0x80)
	inline constexpr std::uint32_t kCellSystemsClear = 0x0047E400;      // (FUN_1406659C0(cell)): removes and releases the cell's physics systems (done before the build at load finish)
	inline constexpr std::uint32_t kCellSystems = 0x006659C0;           // (cell) -> the cell's physics system list (+0x10 count, +0x18 array)
	inline constexpr std::uint32_t kWsTerrainFromHandle = 0x00BBD270;   // (ws+0x1F0 handle, 0) -> terrain manager (when ws+0xF2 bit 1), else ws+0xE0
	inline constexpr std::uint32_t kCellDetach = 0x00B03360;        // (cell, char 1, char 0): grid detach (TES progressive detach, FUN_1404227B0); needs state 9/10, leaves 6/7
	inline constexpr std::uint32_t kCellAttach = 0x00B02C80;        // (cell, *(TES+0x88)): grid attach (FUN_1404227D0 / FUN_1405B17B0); sets state 9, dirties the sync box
	inline constexpr std::uint32_t kCellAttachFinish = 0x00AF1560;  // (cell, char): after attach (FUN_1405B17B0 passes 1)
	inline constexpr std::uint32_t kTerrainBlockEnsure = 0x00978280;    // (grid, request (+0x20 level), int coords[2]) -> bool: builds a block once; built bits per block at grid+0x240 [y*width(+0x28)+x], bit 1<<level, 0x80 = building
	inline constexpr std::uint32_t kTerrainPageStitch = 0x0098E8D0;    // (page, coords, neighbour, coords): edge copy of BGSTerrain PageStitchNode jobs
	inline constexpr std::uint32_t kCellLoadKey = 0x00422600;          // (uint32 out[3], wsFormId, int16 x, int16 y): LoadedCellDB key
	inline constexpr std::uint32_t kPlanetCellKey = 0x00424C50;        // (uint64* out, worldspace, int xy[2]) = ws+0x310 origin offset by xy, packed
	inline constexpr std::uint32_t kHavokShiftWorld = 0x03060A50;       // hknpWorld::shiftWorld (vtbl[76] of the +0x18 interface)
	inline constexpr std::uint32_t kOriginShiftEventSource = 0x00691580;  // returns BSTGlobalEvent::EventSource<BSWorldOriginShiftEvent>*
	inline constexpr std::uint32_t kForEachRefInCell = 0x00B1E190;      // (cell, callback, flag) -> int
	inline constexpr std::uint32_t kEventNotify = 0x022C9300;           // (source, callback)
	inline constexpr std::uint32_t kRWLockWrite = 0x022CC3D0;           // (lock)
	inline constexpr std::uint32_t kRWLockWriteRelease = 0x002C45A0;    // (lock)
	inline constexpr std::uint32_t kGetPlanetGrid = 0x0096B8E0;         // (unused, planetId, create) -> grid
	inline constexpr std::uint32_t kOffsetPlanetCoord = 0x0096F5C0;     // (grid, coord in/out, int offset[2])
	inline constexpr std::uint32_t kMapFindKey = 0x003101F0;            // (table, &key) -> slot index, capacity when absent
	inline constexpr std::uint32_t kMapInsertKey = 0x00310090;          // (table, &index in/out, &key) -> inserted
	inline constexpr std::uint32_t kHudNotificationSource = 0x014915A0;  // event source used by Debug.Notification
	inline constexpr std::uint32_t kFixedStringCreate = 0x028CBA80;     // (BSFixedString*, const char*, bool)
	inline constexpr std::uint32_t kFixedStringRelease = 0x028CAEA0;    // (BSFixedString*)
	inline constexpr std::uint32_t kSetCustomMarker = 0x01A43220;       // (float pos[3], worldspace, uint32* markerId, player+0xE48)

	inline constexpr std::uint32_t kShipOfRef = 0x02116840;           // (ref, char includeExteriorShipCell, 0, 0) -> ship ref the ref is inside, or null
	inline constexpr std::uint32_t kCondGetActorInShipPilotSeat = 0x00C063A0;  // (ref** subject, actor, 0, float* result) -> result 1.0 if actor pilots subject's ship
	inline constexpr std::uint32_t kMoveReference = 0x02062F50;       // (ref, uint32* handle, parentCell, worldspace, float pos[3], float rot[3])
	inline constexpr std::uint32_t kCondIsInSpace = 0x00C05F70;       // (ref** subject, 0, 0, float* result) -> 1.0 in space
	// PlayerCamera: current state +0x10, first-person state +0x188, third-person state +0x228.
	inline constexpr std::uint32_t kPlayerCameraPtr = 0x061DD460;   // PlayerCamera*
	inline constexpr std::uint32_t kForceThirdPerson = 0x01E8C840;  // (camera): Papyrus Game.ForceThirdPerson
	inline constexpr std::uint32_t kLookHandlerVtbl = 0x04C41F28;    // PlayerControls::LookHandler::vftable; [6] +0x30 = mouse
	inline constexpr std::uint32_t kLookHandlerMouse = 0x012BC700;   // FUN_1412bc700(handler, event): data(+0x40) +0xC/+0x10 = counts (y flipped)
	inline constexpr std::uint32_t kPlayerControlsPtr = 0x061E23E0;   // PlayerControls* (handlers: flight +0x208, look +0x238, movement +0x240)
	inline constexpr std::uint32_t kInvalidHandle = 0x061FAE90;
	inline constexpr std::uint32_t kUpdateFrameId = 0x05E7A420;     // uint32: current NiUpdateData frame (+0x44), as the move routine stamps it
	inline constexpr std::uint32_t kToggleCollision = 0x00C63FF0;     // console tcl executor (thisObj = 0: player no-clip toggle)
	inline constexpr std::uint32_t kNoClipFlag = 0x05E60B1C;          // uint8: no-clip on (DAT_145e60b1c)
	inline constexpr std::uint32_t kGroundRayCast = 0x00583B70;       // (TES, float pos[3], 0x800, float above, float length) -> ground z or -FLT_MAX
	inline constexpr std::uint32_t kGridUpdate = 0x00576530;          // (TES, float pos[3], char, char, cell): loaded-grid update around pos (hooked: prefetch)
	inline constexpr std::uint32_t kGlobePublish = 0x01684A10;        // (bool*, BodyInspectState*): publishes the globe's POI list to the UI (hooked)
	inline constexpr std::uint32_t kWorldToLatLong = 0x0078D1F0;      // (mgr, double out[2] {longitude, latitude} radians, float pos[3])
	inline constexpr std::uint32_t kGetPlayerHomeShip = 0x01A6BD50;   // (player, NiPointer<ref>* out): the player's home ship (addref'd)
	inline constexpr std::uint32_t kRefRelease = 0x00384600;          // (ref): drop one reference
	inline constexpr std::uint32_t kExitPilotSeat = 0x0155C7A0;       // console ExitPilotSeat executor (no arguments used)
	inline constexpr std::uint32_t kCloneCopyType = 0x05E7A424;       // uint32 global: default copy type for NiCloningProcess (set at runtime)
	inline constexpr std::uint32_t kCloneScale = 0x04F044E8;          // float constant 1.0 used as the cloning scale

	// Planet content (BGSPlanetContentManager, "PCM"): generation around a landing marker, and removal.
	inline constexpr std::uint32_t kContentLandingBlock = 0x018ABD60;  // (PCM, markerRef): generate the virtual landing block around the marker
	inline constexpr std::uint32_t kCreateReference = 0x005A0520;     // (dataHandler, uint32* handle out, NEW_REFR_DATA*)
	inline constexpr std::uint32_t kHandleLookup = 0x028CCC40;        // (handleTable, uint32 handle, functor*)
	inline constexpr std::uint32_t kMarkerPost1 = 0x00B36C40;         // (ref, 0, 1)  } the landing marker's
	inline constexpr std::uint32_t kMarkerPost2 = 0x00B3F5D0;         // (ref, 1)     } tear-down after generation
	inline constexpr std::uint32_t kMarkerPost3 = 0x0187BB60;         // (handle, 0)  } (FUN_14076df30)
	inline constexpr std::uint32_t kContentRemoveOverlay = 0x018B40B0;  // (overlay record) -> removed
	inline constexpr std::uint32_t kRWLockWriteB = 0x022C8D10;        // (lock): write lock used by the PCM (+0x300)
	inline constexpr std::uint32_t kQuestSystemStatus = 0x00ADA760;   // (system, {uint32 value; bool has}*)
	inline constexpr std::uint32_t kGameSingleton = 0x023FF640;       // () -> object whose +0x8B0 is passed to the status call
	inline constexpr std::uint32_t kNewRefrDataVtbl = 0x04B0F6C8;     // NEW_REFR_DATA::vftable
	inline constexpr std::uint32_t kHandleToRefFunctorVtbl = 0x04AF3D50;  // LocalFunctionImpl<lambda_3a268ab6...>: handle -> NiPointer<ref>
	inline constexpr std::uint32_t kContentManagerPtr = 0x061E1D80;   // BGSPlanetContentManager*
	inline constexpr std::uint32_t kDataHandlerPtr = 0x05FD9B28;      // passed to CreateReference
	inline constexpr std::uint32_t kLandingMarkerBasePtr = 0x05E61448;  // base form of the landing marker
	inline constexpr std::uint32_t kRefHandleTablePtr = 0x05E60380;
	// Playable area (content gate): (latlong, planetId, originCoord, buffer) -> inside a square of min(8 blocks,
	// uMaximumPlayableAreaWorldCoord*2/100) cells around originCoord (block aligned), half-width minus buffer.
	inline constexpr std::uint32_t kPlayableAreaTest = 0x003A5C20;  // hooked: playarea
	inline constexpr std::uint32_t kPlanetGridFor = 0x0096B8E0;     // (latlong, planetId, 1) -> planet block grid
	inline constexpr std::uint32_t kCoordFromLatLong = 0x0096EB90;  // (grid, coord out, latlong): face/block/cell struct
	inline constexpr std::uint32_t kCoordOffset = 0x0096F940;       // (grid, int out[2], coordA, coordB): B - A in cells
	inline constexpr std::uint32_t kContentCodeBegin = 0x018A0000;  // BGSPlanetContentManager code (callers of the gate)
	inline constexpr std::uint32_t kContentCodeEnd = 0x018C0000;
	// Content records: +0 lat/long, +0x10 instance (cleared by the remover), +0x18 definition, +0x2C id, +0x30 skip.
	inline constexpr std::uint32_t kContentInstanceFor = 0x018AB8D0;  // (planet, _, def+0x60 form 'V', record, 0, 1, def, 0, 1, record+0x2C, 0) -> instance
	inline constexpr std::uint32_t kContentPlaceRecord = 0x018B4050;  // (record): place the instanced content
	// Ship movement (recon probe).
	inline constexpr std::uint32_t kMovementControllerOf = 0x012AA710;      // (out {controller, handle}, ref, 0, 0): actor +0x240, else registry by form id
	inline constexpr std::uint32_t kMovementAgentByName = 0x02734DD0;       // (controller, BSFixedString* name, out agent) -> found
	inline constexpr std::uint32_t kGalaxyPositionOf = 0x0090D160;          // (out pair, ref, tmp): pair[1] = galaxy position component (doubles +0x40)
	inline constexpr std::uint32_t kPlayerFlightControlsName = 0x0068E780;  // () -> BSFixedString* "Player Flight Controls"
	// Ship piloting (Phase 2 test): menus and the landed state.
	inline constexpr std::uint32_t kShipSystemUpdate = 0x02169100;  // (system, arg): per-frame ship system update from the main loop; hooked: gametick
	inline constexpr std::uint32_t kUIPtr = 0x061DDDC0;              // UI manager (DAT_1461dddc0)
	inline constexpr std::uint32_t kShowMenu = 0x025556A0;           // (ui, BSFixedString* name, mode 0 show / 2 hide)
	inline constexpr std::uint32_t kTakeoffMenuName = 0x012AA320;    // () -> BSFixedString* "TakeoffMenu"
	inline constexpr std::uint32_t kSpaceshipHudMenuName = 0x009FA560;  // () -> BSFixedString* "SpaceshipHudMenu"
	inline constexpr std::uint32_t kShipSetLanded = 0x02117D00;      // (ship, bool landed): landed actor value + event
	inline constexpr std::uint32_t kShipIsLanded = 0x02119F60;       // (ship) -> landed
	inline constexpr std::uint32_t kMinCurrentZoom = 0x05F55EC0;     // fMinCurrentZoom:Camera value (0)
	inline constexpr std::uint32_t kOverShoulderPosZ = 0x05F56080;   // fOverShoulderPosZ:Camera value: third-person height
	inline constexpr std::uint32_t kMaxCurrentZoom = 0x05F55F00;     // fMaxCurrentZoom:Camera value (2.0): third-person zoom cap
	inline constexpr std::uint32_t kMaxSavedOverlays = 0x05E8F250;    // uPlanetContentMaxSavedOverlays value (100)
	// Saves: the location record (player position, landing lat/long mgr+0x98) and the reference disable.
	inline constexpr std::uint32_t kSaveLocationWriter = 0x018286F0;  // (saveBuffer{+8 data, +0x10 capacity, +0x14 offset}); hooked: rebase
	inline constexpr std::uint32_t kAlignedOrigin = 0x0096EFB0;      // (grid, coord out, latlong): block-aligned landing origin
	inline constexpr std::uint32_t kDisableReference = 0x00C10660;   // (ref, fade): what the console's disable calls
	inline constexpr std::uint32_t kEnableReference = 0x00C10810;    // (ref): what the console's enable calls
	// Ship landing with its animation (what Papyrus SpaceshipReference.EnableWithLanding does, via FUN_141f16d20 kind 3):
	// FUN_1421219b0(ship, marker) places the ship at the marker (position, rotation, the marker's cell) and, if the ship
	// has no 3D (disabled), queues the landing sequence FUN_142121cc0; then the ship is enabled (EnableFunctor).
	// Ship take-off with its animation, the NPC way (Papyrus DisableWithTakeOffOrLanding on a ship not in space ->
	// FUN_141f16480 kind 3 -> FUN_1421199b0). Its player branch (pilot == player) runs the player's launch to space:
	// only call it when the player is neither aboard (kShipOfRef) nor the pilot (kShipPilotActor).
	// Console command executor: FUN_141EA1E10(console manager, printf format, ...) formats the line and queues it; the
	// main thread runs the queue (FUN_141EA3130). The game's own input handler runs "ttl" this way (FUN_140FA5D00).
	inline constexpr std::uint32_t kConsoleExecute = 0x01EA1E10;
	inline constexpr std::uint32_t kConsoleManagerPtr = 0x061E26B0;  // console manager*, the executor's first argument
	// The landing setup's lat/long records (FUN_14078b030): +0xB0 = lat/long of the origin coordinate +0xC8, made by
	// FUN_14096f4d0(planet grid = *(mgr+0x120)+0x58, out cube point 0x18 {face, double u, double v}, origin record)
	// then FUN_142332410(double out[2], cube point). FUN_14078d370 converts with +0xB0 (+0x1A8 while shifting).
	inline constexpr std::uint32_t kOriginToCube = 0x0096F4D0;
	inline constexpr std::uint32_t kCubeToLatLong = 0x02332410;
	inline constexpr std::uint32_t kSetLinkedRef = 0x00BA0D40;       // (ref, linkedRef, keyword or null, char promote): Papyrus ObjectReference.SetLinkedRef core
	inline constexpr std::uint32_t kEnableCore = 0x01F16D20;         // (ref, char kind 0 enable/2 grav jump/3 landing, char latent, VM** (only read by error messages), stackId, char* out): Papyrus Enable core; finds a SpaceshipEnabledLandingLink marker and lands a ship there
	inline constexpr std::uint32_t kLandingLinkKeyword = 0x05E9E640; // TESForm* (BGSKeyword, type 4): SpaceshipEnabledLandingLinkKeyword_DO (default object 0x145E9E5F8 + 0x48)
	inline constexpr std::uint32_t kBeaconBaseObject = 0x05F0A088;
	inline constexpr std::uint32_t kLandingMarkerKeyword = 0x05E9E430; // TESForm* (keyword): SpaceshipLandingMarkerKeyword_DO (0x145E9E3E8 + 0x48), required on an EnableWithLanding marker   // TESForm*: OutpostBeaconObject_DO (default object 0x145F0A040 + 0x48)
	inline constexpr std::uint32_t kShipTakeOff = 0x021199B0;        // (ship)
	inline constexpr std::uint32_t kShipPilotActor = 0x021159B0;     // (ship) -> the actor flying it (the player's launch check)
	// The landing slot on a marker, claimed for one ship (the AI's own land-at-marker step FUN_1412b0bf0 does this
	// under the marker's extra-data write lock (marker+0xC8 +0x20) before FUN_1421219b0).
	// The asynchronous cell loader (BSResource2::DB<LoadedCellDB>): (db, 1) processes every queue with wait, finishing
	// all pending cell loads; the engine's own grid relocation (FUN_14057abc0) calls it so. Pending cell-load jobs keyed
	// to old coordinates crash after a world re-centre (null worldspace in FUN_14041efa0), so the re-centre flushes first.
	// Vehicles: the physics step FUN_1423cb260(vehicle, hknpWorld) (hooked: rover hover), the player's vehicle.
	// The enable the game's EnableFunctor uses on a disabled ref (after EnableWithLanding queued the landing).
	inline constexpr std::uint32_t kRefEnableTask = 0x00B4A9C0;  // (ref, 1)
	inline constexpr std::uint32_t kVehicleStep = 0x023CB260;
	inline constexpr std::uint32_t kVehicleBoostUpdate = 0x013211F0;   // (boost component, vehicle context): boost fuel/impulse; sets the graph bools VehicleIsBoosting / VehicleIsVertBoosting on *(context+0x100) + 0x60
	inline constexpr std::uint32_t kSetGraphBool = 0x00F0A370;         // (graph holder = ref + 0x60, BSFixedString* name, char value, 0)
	inline constexpr std::uint32_t kVarVehicleIsBoosting = 0x05F286D8;      // BSFixedString "VehicleIsBoosting" (forward boost)
	inline constexpr std::uint32_t kVarVehicleIsVertBoosting = 0x05F286E0;  // BSFixedString "VehicleIsVertBoosting" (jump boost)
	inline constexpr std::uint32_t kPlayerVehicle = 0x0219BB40;  // (actor) -> vehicle ref being driven, or null
	inline constexpr std::uint32_t kLoadedCellDBPtr = 0x061DD710;    // BSResource2::DB<LoadedCellDB>*
	inline constexpr std::uint32_t kLoadedCellLoadServicePtr = 0x061DD720; // native cell-load service, used by off-grid request path
	inline constexpr std::uint32_t kLoadCellOffGrid = 0x01435C30; // (service, worldspace, x, y, 1): LoadCell without changing the active TES grid
	inline constexpr std::uint32_t kLoadedCellDBFlush = 0x003655A0;  // (db, char all): 1 = every queue, wait
	inline constexpr std::uint32_t kMarkerFindSlot = 0x00B9D650;    // (marker, ship, 0) -> slot or -1
	inline constexpr std::uint32_t kMarkerClaimSlot = 0x00B9D040;   // (marker, slot, ship, 1, 0) -> claimed
	inline constexpr std::uint32_t kShipLandAtMarker = 0x021219B0;   // (ship, markerRef) -> 1 if the landing was queued
	// Saves: request an autosave the way the pause path does (sets saveLoadManager +0x54 = 3, +0x50 |= 1 if allowed).
	inline constexpr std::uint32_t kRequestSave = 0x0184FF90;        // (saveLoadManager, 1, 0)
	inline constexpr std::uint32_t kSaveLoadManagerPtr = 0x05E6D6E8; // BGSSaveLoadManager* (+0x60 tick of the last save)
	// Planet modules (manager module array mgr+0x178, count +0x170). Landing = MoveTo: every module's slot 7 (+0x38),
	// then slot 8 (+0x40, with the manager lock holder), then slot 9 (+0x48).
	inline constexpr std::uint32_t kOverlaysVtbl = 0x04B3E450;            // slot 7 FUN_140797e90 apply overlays, slot 8 FUN_140797d30 refresh
	inline constexpr std::uint32_t kOrderedLandTexturesVtbl = 0x04B3E3C8; // slot 7 FUN_140795110 rebuild texture order (overlays included)
	inline constexpr std::uint32_t kNavMeshGenVtbl = 0x04B3EAA8;          // slot 7 FUN_140791000 flag navmesh regeneration
	inline constexpr std::uint32_t kContentPlacementVtbl = 0x04B3E290;    // slot 7 FUN_14076df30 landing content (we replicate it)
	inline constexpr std::uint32_t kLandscapeVtbl = 0x04B3DF48;          // BGSPlanet::Landscape (planet manager module)
	inline constexpr std::uint32_t kPlanetManagerTryLock = 0x0078F480;      // (holder{mgr, bool owned}*, mgr): try-acquire mgr+0x24/+0x28
	inline constexpr std::uint32_t kPlanetManagerShiftModules = 0x0078C190; // (mgr, holder*): modules' +0x50/+0x58 shift hooks, state 3 -> 4
	inline constexpr std::uint32_t kPlanetManagerShiftCommit = 0x0078C290;  // (mgr, holder*): centre = new centre, clear deltas, state 2, +0x68 hooks       // uint32 global: the "no reference" handle value

	// More globals.
	inline constexpr std::uint32_t kTESPtr = 0x05E716F8;        // TES*
	inline constexpr std::uint32_t kExteriorWorldPtr = 0x061DF6E8;  // ** -> bhkWorld* for exteriors

	// Globals holding object pointers.
	inline constexpr std::uint32_t kPlayerCharacterPtr = 0x05F43230;  // PlayerCharacter*
	inline constexpr std::uint32_t kPlanetManagerPtr = 0x05FD9C30;    // BGSPlanet::Manager*

	// Primary vtables, used to check the pointers above really are those classes.
	inline constexpr std::uint32_t kPlayerCharacterVtbl = 0x04CBC448;
	inline constexpr std::uint32_t kPlanetManagerVtbl = 0x04B3E328;

	// INI setting values (setting object + 8).
	inline constexpr std::uint32_t kBlockSize = 0x05EA66F0;                     // uBlockSize:Stitching
	inline constexpr std::uint32_t kBlockMergeZoneSize = 0x05EA6770;            // uBlockMergeZoneSize:Stitching
	inline constexpr std::uint32_t kMaximumPlayableAreaWorldCoord = 0x05EA6810;  // uMaximumPlayableAreaWorldCoord:General
	inline constexpr std::uint32_t kNumCellWorldExtents = 0x05EAD6B8;           // iNumCellWorldExtents:Havok
	inline constexpr std::uint32_t kBorderRegionsEnabled = 0x05F46D58;          // bBorderRegionsEnabled:General
	inline constexpr std::uint32_t kTerrainShiftThresholdInCells = 0x05F609F0;  // uTerrainShiftThresholdInCells:Terrain
}

// Field offsets (1.16.244).
namespace off
{
	// TESObjectREFR
	inline constexpr std::uint32_t kRefRotation = 0x80;    // NiPoint3, radians
	inline constexpr std::uint32_t kRefPosition = 0x8C;    // NiPoint3, metres
	inline constexpr std::uint32_t kRefParentCell = 0xB0;  // TESObjectCELL*

	inline constexpr std::uint32_t kPlayerCustomMarker = 0xE40;      // uint32 form id of the custom map marker
	inline constexpr std::uint32_t kPlayerCustomMarkerData = 0xE48;
	inline constexpr std::uint32_t kMgrMarkerWorld = 0x78;           // **(mgr+0x78) -> +0x98 = worldspace used for markers
	inline constexpr std::uint32_t kRefLoadedData = 0xB8;  // LOADED_REF_DATA* (+0x08 = root scene node)
	inline constexpr std::uint32_t kLoadedData3D = 0x08;

	// BGSPlanet::Manager
	inline constexpr std::uint32_t kMgrState = 0x20;          // uint8: 2 idle, 3 shifting, 4 shift finished
	inline constexpr std::uint32_t kMgrPlanetId = 0x80;       // uint32 planet form id
	inline constexpr std::uint32_t kMgrOriginCoord = 0xC8;    // planet coordinate (0x2C bytes)
	inline constexpr std::uint32_t kMgrGridCentre = 0x190;    // int32 x, y: terrain grid centre cell
	inline constexpr std::uint32_t kMgrNewCentre = 0x198;     // int32 x, y: centre being shifted to
	inline constexpr std::uint32_t kMgrShiftDelta = 0x1A0;    // int32 x, y: last terrain shift in cells
	inline constexpr std::uint32_t kMgrOriginCoord2 = 0x1C0;  // copy of the planet coordinate used while shifting
	inline constexpr std::uint32_t kMgrLatLong = 0xB0;        // 0x18 bytes, copied to +0x1A8 when a shift starts
	inline constexpr std::uint32_t kMgrLatLong2 = 0x1A8;
	inline constexpr std::uint32_t kMgrModuleCount = 0x170;   // uint32
	inline constexpr std::uint32_t kMgrModules = 0x178;       // IModule** (hooks: +0x50/+0x58 shift, +0x60 begin, +0x68 finish)
	inline constexpr std::uint32_t kMgrShifting = 0x18C;      // uint8: a shift is set up (ShiftToTask pending)
	inline constexpr std::uint32_t kMgrOriginDelta = 0x1EC;   // int32 x, y, z: world origin moved by this many cells (only ever 0 in vanilla)
	inline constexpr std::uint32_t kMgrPlayerPos = 0x200;     // float x, y, z: last player position seen

	// TES
	inline constexpr std::uint32_t kTESGridLock = 0x50;        // BSReadWriteLock: loaded-cell array and grid centre (navmesh update task holds it shared)
	inline constexpr std::uint32_t kTESGridCentre = 0x20;      // int32 x, y (grid at +0x18, centre at grid+0x08)
	inline constexpr std::uint32_t kTESGridCells = 0x30;       // GridCell** (dim * dim); cell at entry + 0x30
	inline constexpr std::uint32_t kTESGridCount = 0x44;       // uint32
	inline constexpr std::uint32_t kTESCurrentCell = 0xC4;     // int32 x at +0xC4, y at +0xC8 (the save writer/reader use them as int32; 0x7FFFFFFF = none)
	inline constexpr std::uint32_t kTESGridUpdatePreviousCell = 0xCC; // int32 x,y: FUN_140576530 compares these with the new cell and builds its edge-load requests from the delta
	inline constexpr std::uint32_t kTESInteriorCell = 0xE8;    // TESObjectCELL*, null outdoors

	// Grid entry (wrapper around a loaded exterior cell)
	inline constexpr std::uint32_t kGridEntryState = 0x14;     // valid when ((v & 0x70000000) + 0xD0000000) & 0xEFFFFFFF == 0
	inline constexpr std::uint32_t kGridEntryCell = 0x30;      // TESObjectCELL*

	// TESObjectCELL (1.16.244)
	inline constexpr std::uint32_t kCellFlags = 0x48;         // bit 0 = interior
	inline constexpr std::uint32_t kCellState = 0x4F;         // uint8, 10 = attached (fully loaded)
	inline constexpr std::uint32_t kFormId = 0x28;
	inline constexpr std::uint32_t kCellExterior = 0x58;      // exterior data*: +0x00 x, +0x04 y, +0x24/+0x28 world corner
	inline constexpr std::uint32_t kCellWorldspace = 0x110;
	// PlayerControls input data (+0x58), written by the movement and look handlers; the flight handler
	// reads its throttle and steering from the same values.
	inline constexpr std::uint32_t kPlayerControlsData = 0x58;  // +0 move x (A/D), +4 move y (W/S), +8 up/down, +0xC look x, +0x10 look y
	inline constexpr std::uint32_t kCellFormFlags = 0x20;     // bit 10 (0x400) = the worldspace's persistent cell
	inline constexpr std::uint32_t kCellInteriorKind = 0x68;  // byte in the interior data (+0x58): 1 = ship interior, 5 = ship exterior cell

	// TESWorldSpace
	inline constexpr std::uint32_t kWsCellMap = 0x90;         // coordinate -> cell handle (not form id); table at +0x98
	inline constexpr std::uint32_t kWsCellMapLock = 0xC0;
	inline constexpr std::uint32_t kWsPlanetOrigin = 0x310;    // packed planet cell id of grid (0, 0); -1 = not a planet worldspace

	// Scatter table (the map's table at map + 8)
	inline constexpr std::uint32_t kTblEntries = 0x08, kTblCapacity = 0x10, kTblFree = 0x18, kTblLastFree = 0x20;

	// Scene node (NiAVObject / NiNode)
	inline constexpr std::uint32_t kNodeParent = 0x38;
	inline constexpr std::uint32_t kNodeLocalT = 0x70;      // local.translate
	inline constexpr std::uint32_t kNodeWorldT = 0xB0;      // world.translate
	inline constexpr std::uint32_t kNodePrevWorldT = 0xF0;  // previousWorld.translate
	inline constexpr std::uint32_t kNodeBoundCentre = 0x100;
	inline constexpr std::uint32_t kNodeChildren = 0x138;   // NiAVObject** ; count uint16 at +0x142
	inline constexpr std::uint32_t kNodeChildCount = 0x142;
	inline constexpr std::uint32_t kVtblIsNode = 0x20 / 8;  // vtable slot returning NiNode* or null

	// Havok
	inline constexpr std::uint32_t kBhkHknpWorld = 0x50;      // bhkWorld -> hknpBSWorld*
	inline constexpr std::uint32_t kHknpShiftIface = 0x18;    // interface whose vtable holds shiftWorld
	inline constexpr std::uint32_t kHknpLock = 0xB00;

	inline constexpr float kCellSize = 100.0f;  // metres
}
