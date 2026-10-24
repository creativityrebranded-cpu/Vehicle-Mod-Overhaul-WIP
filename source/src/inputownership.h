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
}
