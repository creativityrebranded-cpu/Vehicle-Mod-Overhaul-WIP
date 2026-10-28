#include "runtimecells.h"
#include "addresses.h"
#include "game.h"
#include "log.h"
#include <Windows.h>
#include <algorithm>

namespace runtimecells {
namespace {
int g_refsOffset=-1;
template<class T>T& At(std::uintptr_t p,unsigned offset){return *reinterpret_cast<T*>(p+offset);}
std::uintptr_t Tes(){return *reinterpret_cast<const std::uintptr_t*>(game::Base()+addr::kTESPtr);}
bool TryReferences(std::uintptr_t cell,int offset,std::uintptr_t*& data,std::uint32_t& count){
    __try {
        const auto size=At<std::uint32_t>(cell,unsigned(offset)),cap=At<std::uint32_t>(cell,unsigned(offset+4));
        const auto refs=At<std::uintptr_t*>(cell,unsigned(offset+8));
        if(!refs||!size||size>cap||cap>200000)return false;
        const auto first=refs[0];
        if(!first||At<std::uintptr_t>(first,off::kRefParentCell)!=cell)return false;
        data=refs;count=size;return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
}
unsigned GridSize(unsigned cap) {
    __try {const auto tes=Tes();return tes?std::min(At<unsigned>(tes,off::kTESGridCount),cap):0;}
    __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
std::uintptr_t Cell(unsigned index,Policy policy) {
    __try {
        const auto tes=Tes();if(!tes||index>=At<unsigned>(tes,off::kTESGridCount))return 0;
        const auto entries=At<std::uintptr_t*>(tes,off::kTESGridCells);const auto entry=entries?entries[index]:0;
        if(!entry)return 0;
        const auto state=At<std::uint32_t>(entry,off::kGridEntryState);
        if(policy==Policy::ReadyGridEntry&&(((state&0x70000000u)+0xD0000000u)&0xEFFFFFFFu)!=0)return 0;
        const auto cell=At<std::uintptr_t>(entry,off::kGridEntryCell);
        return cell&&At<std::uint8_t>(cell,off::kCellState)==10?cell:0;
    } __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
std::uintptr_t Attached(unsigned index){return Cell(index,Policy::ReadyGridEntry);}
bool References(std::uintptr_t cell,std::uintptr_t*& data,std::uint32_t& count) {
    data=nullptr;count=0;if(!cell)return false;
    const int offsets[]{0x80,0x88};
    for(const auto offset:offsets){
            if(g_refsOffset>=0&&g_refsOffset!=offset)continue;
            if(!TryReferences(cell,offset,data,count))continue;
            if(g_refsOffset<0){g_refsOffset=offset;logger::Write("Runtime cells: reference array at cell+0x%X",offset);}
            return true;
    }
    return false;
}
void Reset(){g_refsOffset=-1;}
}
