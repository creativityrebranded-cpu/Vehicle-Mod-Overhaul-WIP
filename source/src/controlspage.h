#pragma once

// The mod's controls on the game's own Settings > Controls page (2026-10-23, the user: "the keybindings at the native side ...
// with a header for flying vehicle ... so if we use the selecting of a button it doesn't overlap ... it'll move there like
// native does").
//
// The page (settingspanel.swf ControlsPanel_mc.ControlsList_mc) lists ControlBindingsData.aInputSettingsList; hoverui.cpp
// appends a divider row and one row per action and device to it. Selecting one of our rows runs the game's own capture
// (SettingsPanel_RemapMode -> RemapHandler DAT_145F2F430: +0x40 event name, +0x48 context, +0x49 slot, +0x4A device,
// +0x4C/+0x50 codes). The capture ends in two control-map calls, both hooked here for our event names only:
//   FUN_1422EEA90 (map, BSFixedString* event, device, codes*, slot, context) -> 0 free / 2 in use / else refused
//   FUN_1422EF4C0 (same arguments) -> nonzero applied; the game then rebuilds the list and leaves remap mode.
// Ours answer "free" and apply through bindings::Assign, which moves the input off any other of our actions.
// Row labels are translation keys ("$" + sContextName [+ "_" + sInputName + "_KBM"]); the game loads one translation file,
// so the Scaleform translator (BSScaleformTranslator::ScaleformImpl vtable slot 2) answers our keys.
namespace controlspage
{
	constexpr const char* kContextName = "FlyingVehicleOverhaul";  // our rows' sContextName (the header's key)
	constexpr unsigned kContextId = 0x49;  // our rows' uContextID: the game's Vehicle context, which has no conflict group
	void Install();
	bool Installed();
}
