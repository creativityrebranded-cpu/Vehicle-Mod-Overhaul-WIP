#include "runtimeforms.h"
#include "addresses.h"
#include "game.h"
#include <Windows.h>

namespace runtimeforms {
std::uintptr_t Lookup(std::uint32_t id) {
    if(!id)return 0;
    __try {return reinterpret_cast<std::uintptr_t(*)(std::uint32_t)>(game::Base()+addr::kLookupFormById)(id);}
    __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
std::uint32_t Id(std::uintptr_t form) {
    __try {return form?*reinterpret_cast<const std::uint32_t*>(form+off::kFormId):0;}
    __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
std::uintptr_t LookupChecked(std::uint32_t id) {
    const auto form=Lookup(id);return form&&Id(form)==id?form:0;
}
std::uintptr_t LoadedPlugin(const char* name) {
    if(!name||!name[0])return 0;
    __try {
        const auto data=*reinterpret_cast<const std::uintptr_t*>(game::Base()+addr::kDataHandlerPtr);
        if(!data)return 0;
        const auto file=reinterpret_cast<std::uintptr_t(*)(std::uintptr_t,const char*)>(game::Base()+addr::kLookupPluginByName)(data,name);
        return file&&*reinterpret_cast<const std::uint8_t*>(file+0x218)!=0xFF?file:0;
    } __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
std::uint32_t RuntimeId(std::uintptr_t file,std::uint32_t local) {
    if(!file)return 0;
    __try {reinterpret_cast<void(*)(std::uintptr_t,std::uint32_t*)>(game::Base()+addr::kPluginLocalToRuntime)(file,&local);return local;}
    __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
std::uint32_t Resolve(Key key) {
    return key?RuntimeId(LoadedPlugin(key.plugin),key.local):0;
}
}
