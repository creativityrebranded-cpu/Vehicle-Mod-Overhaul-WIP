#pragma once
#include <cstddef>
#include <cstdint>
namespace inputownership {
// 0 unknown, 1 native game owns this press, 2 mod owns it. Ownership lasts
// through release, even if menus, scanner, editor or a vehicle change meanwhile.
inline bool Own(std::uint8_t& owner,bool down,bool fresh,bool requested) {
    if(down){if(fresh||!owner)owner=requested?2:1;return owner==2;}
    const bool block=owner==2;owner=0;return block;
}
inline bool Gameplay(const std::uint8_t* contexts,std::size_t count) {
    if(count>128)return false;
    for(std::size_t i=0;i<count;++i)if(contexts[i]==0x4E||contexts[i]==0x11)return false;
    return true;
}
// 2026-11-15 hotfix (Nexus reports: rebinding froze the Controls page; G no longer added modules in the ship builder or
// worked on the local map): the mod's keys were taken from the game in every menu but the console, so the remap capture
// never saw an FVO-owned key and menus lost theirs. Input contexts are the game's names table (Starfield.exe 1.16.244
// +0x4A554E0, 78 entries). Any menu context on the stack gives every key and button back to the game.
// 2026-11-17, from the decomp (push FUN_1422F2480(cm, id, owner); entry +0 id, +8 owner name; a menu pushes its own list
// IMenu+0xD8..0xE0 with owner IMenu+0xB0 in FUN_1425417A0, plus 0x4E when its flags +0xC0 & 0x10):
// - NOT menus, on the stack in ordinary play: 0x01 ConsoleOpening (owner "TES", pushed by FUN_14057F430 as the loading
//   screen clears), 0x06 QuickContainer (HUDRolloverDataModel, look-at loot prompt), 0x23 Ship_QuickContainer
//   (ShipHUDData), 0x3A LevelUp (HUDExperienceMeterDataModel). Counting 0x01 took every key from the mod in play
//   (in game 2026-11-17: HUD Hover / Change gear "UNBOUND").
// - Menus: the ids below; 0x4C OpenAlmanac is only in the Star Map menu's list (FUN_1416BEF40).
inline bool MenuContext(std::uint8_t id) {
    switch(id){
    // 0x03 VirtualController and 0x0C RightThumbstick stay out: every menu list holding them also holds 0x02, and one
    // list (FUN_141EC8A80) holds 0x03 alone with its owner not identified.
    case 0x02: /*BasicMenuNav*/ case 0x04: case 0x05: /*Menu3DModel*/
    case 0x0D: case 0x0E: /*Cursor*/ case 0x0F: /*Book*/ case 0x10: /*DataSlateButtons*/ case 0x11: /*Console*/
    case 0x16: case 0x17: /*Workshop*/ case 0x18: /*Terminal*/ case 0x19: /*PhotoMode*/ case 0x1A: /*Security*/
    case 0x1D: case 0x1E: /*DataMenu*/
    case 0x28: case 0x29: case 0x2A: /*StarMap*/ case 0x2B: /*SurfaceMap*/
    case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: /*ShipBuilder*/
    case 0x35: case 0x36: case 0x37: /*ShipInfo*/ case 0x3D: /*TextInput*/ case 0x3E: /*MissionFromStarMap*/
    case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: /*ShipBuilder simple*/ case 0x45: /*BasicMenuNav_JustCancel*/
    case 0x46: /*StarMap_QuickSelect*/ case 0x47: /*Creations*/ case 0x48: /*Login*/ case 0x4A: /*Vehicle_Hangar*/
    case 0x4B: case 0x4C: /*Almanac, OpenAlmanac*/ case 0x4E: /*StopContextProcessing*/
        return true;
    default: return false;
    }
}
inline bool ModKeys(const std::uint8_t* contexts,std::size_t count) {
    if(!Gameplay(contexts,count))return false;
    for(std::size_t i=0;i<count;++i)if(MenuContext(contexts[i]))return false;
    return true;
}
}
