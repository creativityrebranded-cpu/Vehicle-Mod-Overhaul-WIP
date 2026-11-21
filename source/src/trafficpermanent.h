#pragma once
#include "trafficprofilestore.h"
#include <map>
#include <sstream>
#include <iomanip>
// Shared authoring entries are an explicit overlay on a branched working copy.
// Immutable game-save snapshots and vehicle/journey records are never edited.
namespace trafficpermanent {
namespace fs=std::filesystem;
inline constexpr const char* signature="; FVO permanent traffic v1\n";
inline bool Name(const fs::path& path){return trafficprofilestore::Lower(path.filename().string()).starts_with("permanent_")&&trafficprofilestore::Lower(path.extension().string())==".ini";}
inline std::string FileName(const std::string& world){std::uint64_t hash=1469598103934665603ull;for(unsigned char c:trafficprofilestore::Lower(world))hash=(hash^c)*1099511628211ull;std::ostringstream out;out<<"Permanent_"<<std::hex<<hash<<".ini";return out.str();}
inline std::string Read(const fs::path& path){std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("Could not read permanent traffic entry");return {std::istreambuf_iterator<char>(in),{}};}
// A copy whose line endings were converted to CRLF outside FVO (OneDrive, an editor) is still ours.
inline bool Owned(const std::string& text){return text.starts_with(signature)||text.starts_with("; FVO permanent traffic v1\r\n");}
// A file switched off by hand ([file] enabled = 0) is not read back, and writing it would switch it on again. New shared routes go beside it.
inline bool Disabled(const std::string& text){return text.find("\nenabled = 0")!=std::string::npos;}
inline std::string Live(const std::string& name){const auto dot=name.rfind('.');return dot==std::string::npos?name+"_live":name.substr(0,dot)+"_live"+name.substr(dot);}
// Writer atomically replaces one managed file and retains its previous contents.
template<class Writer> inline void Sync(const fs::path& shared,const fs::path& working,Writer&& write){std::map<std::string,std::pair<fs::path,std::string>> current;if(fs::exists(shared)){for(const auto& f:fs::directory_iterator(shared)){if(!f.is_regular_file()||!Name(f.path()))continue;auto text=Read(f.path());if(!Owned(text))throw std::runtime_error("Unrecognized permanent traffic file; preserved without importing");current.emplace(trafficprofilestore::Lower(f.path().filename().string()),std::make_pair(f.path(),std::move(text)));}}
    for(const auto& f:fs::directory_iterator(working)){if(!f.is_regular_file()||!Name(f.path()))continue;const auto key=trafficprofilestore::Lower(f.path().filename().string());const auto text=Read(f.path());if(!Owned(text))throw std::runtime_error("Permanent cache name is occupied by an unowned file");if(!current.count(key)){fs::copy_file(f.path(),fs::path(f.path().wstring()+L".removed.bak"),fs::copy_options::overwrite_existing);fs::remove(f.path());}}
    for(const auto& [name,value]:current){const auto target=working/value.first.filename();if(fs::exists(target)){const auto old=Read(target);if(!Owned(old))throw std::runtime_error("Permanent cache would overwrite an unowned file");if(old==value.second)continue;}write(target,value.second);}
}
}
