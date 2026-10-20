#pragma once
#include <cstdint>
#include <span>
namespace bodybinding {
struct Binding {std::uint32_t body=0;bool enabled=false;};
inline bool Valid(std::uint32_t id){return id!=0&&(id&0xFFFFFFu)!=0xFFFFFFu;}
inline bool Matches(std::uint32_t body,std::uint32_t expected){return Valid(body)&&body==expected;}
inline bool Contains(std::span<const std::uint32_t> ids,std::uint32_t body){
 if(!Valid(body))return false;for(const auto id:ids)if(id==body)return true;return false;
}
// Full IDs include the generation. Neither nearby coordinates nor a cached pointer prove ownership.
inline int Owner(std::uint32_t body,std::span<const Binding> bindings){
 if(!Valid(body))return -1;int found=-1;
 for(std::size_t i=0;i<bindings.size();++i)if(bindings[i].enabled&&Matches(body,bindings[i].body)){
  if(found>=0)return -1;found=int(i);
 }
 return found;
}
}
