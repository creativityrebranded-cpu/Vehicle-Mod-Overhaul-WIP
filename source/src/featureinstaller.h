#pragma once
#include <Windows.h>
#include <array>
#include <string>

// Data-local installer fragments are imported once when a selected value changes.
// This keeps the existing Documents INI as the single source used by all systems.
namespace featureinstaller {
enum class Status { Disabled, Unchanged, Applied, Invalid, Failed };
struct Result { Status status=Status::Disabled; unsigned changed=0; };
inline std::wstring Filename(const wchar_t* section,const wchar_t* key){return std::wstring(section)+L"."+key+L".ini";}
inline std::wstring Read(const std::wstring& file,const wchar_t* section,const wchar_t* key){
    wchar_t value[256]{};GetPrivateProfileStringW(section,key,L"",value,DWORD(std::size(value)),file.c_str());return value;
}
inline bool Exists(const std::wstring& path){return GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES;}
inline bool Missing(const std::wstring& path){
    if(Exists(path))return false;const auto e=GetLastError();return e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND;
}
inline void Flush(const std::wstring& path){WritePrivateProfileStringW(nullptr,nullptr,nullptr,path.c_str());}
inline bool Replace(const std::wstring& staged,const std::wstring& target){
    Flush(staged);Flush(target);
    return MoveFileExW(staged.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
}
template<class Feature,std::size_t N>
Result Apply(const std::wstring& directory,const std::wstring& settings,const Feature (&features)[N]){
    if(directory.empty()||settings.empty())return {};
    const auto mode=directory+L"\\mode.ini";
    if(Missing(mode))return {};
    const auto enabled=Read(mode,L"installer",L"enabled");
    if(enabled==L"0")return {};
    if(enabled!=L"1")return {Status::Invalid};
    std::array<std::wstring,N> values,keys;
    std::array<bool,N> changed{};
    const auto ledger=settings+L".fomod-applied";
    unsigned count=0;
    for(std::size_t i=0;i<N;++i){
        keys[i]=std::wstring(features[i].section)+L"."+features[i].key;
        const auto file=directory+L"\\"+Filename(features[i].section,features[i].key);
        if(Read(file,L"feature",L"section")!=features[i].section||Read(file,L"feature",L"key")!=features[i].key)
            return {Status::Invalid};
        values[i]=Read(file,L"feature",L"value");
        if(values[i]!=L"0"&&values[i]!=L"1")return {Status::Invalid};
        changed[i]=Read(ledger,L"applied",keys[i].c_str())!=values[i];
        if(changed[i])++count;
    }
    if(!count)return {Status::Unchanged};
    const auto temp=settings+L".fomod-new",stateTemp=ledger+L".new";
    struct Cleanup {std::wstring a,b;~Cleanup(){DeleteFileW(a.c_str());DeleteFileW(b.c_str());}} cleanup{temp,stateTemp};
    Flush(settings);Flush(ledger);
    const bool existing=Exists(settings);
    if(!existing&&!Missing(settings))return {Status::Failed};
    if(existing){
        // Keep the pre-import file even if a later ledger commit needs a retry.
        const auto backup=settings+L".before-fomod";
        if(!Exists(backup)&&!CopyFileW(settings.c_str(),backup.c_str(),TRUE))return {Status::Failed};
        if(!CopyFileW(settings.c_str(),temp.c_str(),FALSE))return {Status::Failed};
    }else{
        HANDLE file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)return {Status::Failed};CloseHandle(file);
    }
    if(Exists(ledger)){if(!CopyFileW(ledger.c_str(),stateTemp.c_str(),FALSE))return {Status::Failed};}
    else {
        if(!Missing(ledger))return {Status::Failed};
        HANDLE file=CreateFileW(stateTemp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)return {Status::Failed};CloseHandle(file);
    }
    for(std::size_t i=0;i<N;++i){
        if(changed[i]&&!WritePrivateProfileStringW(features[i].section,features[i].key,values[i].c_str(),temp.c_str()))return {Status::Failed};
        if(!WritePrivateProfileStringW(L"applied",keys[i].c_str(),values[i].c_str(),stateTemp.c_str()))return {Status::Failed};
    }
    // Publish settings first; a ledger failure safely retries the same choices.
    if(!Replace(temp,settings)||!Replace(stateTemp,ledger))return {Status::Failed};
    return {Status::Applied,count};
}
}
