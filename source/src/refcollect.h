#pragma once
#include <cstddef>
namespace refcollect {
// Native ForEachRefInCell continues only for return value 1; 0 ends the visit
// and releases its cell lock through the normal exit path (FUN_140B1E190).
template<class Refs,class Ref>
int Append(Refs& refs,Ref ref,std::size_t limit) {
    if(ref&&refs.size()<limit)refs.push_back(ref);
    return refs.size()<limit?1:0;
}
}
