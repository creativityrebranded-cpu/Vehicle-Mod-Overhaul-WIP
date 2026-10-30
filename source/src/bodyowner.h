#pragma once
#include <cstdint>
namespace bodyowner {
struct Result {std::uintptr_t object=0;bool invalid=false;};
// The LoadedAreaBound query accepts a null owner. Validate both virtual
// pointers before invoking; a freed object's all-bits-set vtable is not code.
template<class Read,class Native,class Invoke>
Result Lookup(std::uintptr_t user,Read read,Native native,Invoke invoke){
    if(!user)return {};
    const auto vtable=read(user);
    if(!native(vtable)||!native(vtable+0x138+sizeof(std::uintptr_t)-1))return {0,true};
    const auto fn=read(vtable+0x138);
    if(!native(fn))return {0,true};
    return {invoke(fn,user),false};
}
}
