#include <Windows.h>
#include <DbgHelp.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#pragma comment(lib, "dbghelp")
struct Range { DWORD64 address; size_t size, offset; };
struct Module { DWORD64 base; DWORD size; std::wstring path; std::vector<char> image; };
static std::vector<char> dump;
static std::vector<Range> ranges;
static std::vector<Module> modules;
static bool Load(const std::wstring& path, std::vector<char>& bytes) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto size = file.tellg(); if (size < 0) return false;
    bytes.resize(size_t(size)); file.seekg(0); return bool(file.read(bytes.data(), size));
}
static BOOL CALLBACK Read(HANDLE, DWORD64 address, PVOID out, DWORD count, LPDWORD got) {
    *got = 0;
    for (const auto& r : ranges) {
        if (address >= r.address && address - r.address <= r.size && count <= r.size - size_t(address-r.address)) {
            const auto at = r.offset + size_t(address-r.address);
            if (at > dump.size() || count > dump.size()-at) continue;
            std::memcpy(out, dump.data()+at, count); *got=count; return TRUE;
        }
    }
    for (auto& m : modules) {
        if (address < m.base || address-m.base >= m.size) continue;
        if (m.image.empty() && !Load(m.path,m.image)) break;
        if (m.image.size()<sizeof(IMAGE_DOS_HEADER)) break;
        const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(m.image.data());
        if (dos->e_magic!=IMAGE_DOS_SIGNATURE || size_t(dos->e_lfanew)+sizeof(IMAGE_NT_HEADERS64)>m.image.size()) break;
        const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.image.data()+dos->e_lfanew);
        const auto* section=IMAGE_FIRST_SECTION(nt);
        const auto rva=address-m.base;
        for (unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
            if (rva < section[i].VirtualAddress || rva-section[i].VirtualAddress+count>section[i].SizeOfRawData) continue;
            const size_t at=section[i].PointerToRawData+size_t(rva-section[i].VirtualAddress);
            if(at+count>m.image.size()) break;
            std::memcpy(out,m.image.data()+at,count); *got=count; return TRUE;
        }
    }
    return FALSE;
}
static void* Stream(ULONG type) {
    PMINIDUMP_DIRECTORY directory=nullptr; PVOID stream=nullptr; ULONG size=0;
    return MiniDumpReadDumpStream(dump.data(),type,&directory,&stream,&size)?stream:nullptr;
}
static const Module* Find(DWORD64 address) {
    for(const auto& m:modules) if(address>=m.base && address-m.base<m.size) return &m;
    return nullptr;
}
static void* ImageRva(Module& m,DWORD rva,size_t count) {
    if(m.image.empty() && !Load(m.path,m.image)) return nullptr;
    if(m.image.size()<sizeof(IMAGE_DOS_HEADER)) return nullptr;
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(m.image.data());
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE || size_t(dos->e_lfanew)+sizeof(IMAGE_NT_HEADERS64)>m.image.size()) return nullptr;
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(m.image.data()+dos->e_lfanew);
    auto* sections=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i){
        const auto& s=sections[i];
        if(rva<s.VirtualAddress || size_t(rva-s.VirtualAddress)+count>s.SizeOfRawData)continue;
        const size_t at=s.PointerToRawData+size_t(rva-s.VirtualAddress);
        return at+count<=m.image.size()?m.image.data()+at:nullptr;
    }
    return nullptr;
}
static PVOID CALLBACK Functions(HANDLE,DWORD64 address) {
    for(auto& m:modules) {
        if(address<m.base || address-m.base>=m.size)continue;
        if(m.image.empty() && !Load(m.path,m.image))return nullptr;
        auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(m.image.data());
        auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(m.image.data()+dos->e_lfanew);
        const auto& dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        auto* functions=static_cast<RUNTIME_FUNCTION*>(ImageRva(m,dir.VirtualAddress,dir.Size));
        if(!functions)return nullptr;
        const auto count=dir.Size/sizeof(RUNTIME_FUNCTION);
        const auto rva=DWORD(address-m.base);
        auto* end=functions+count;
        auto* found=std::upper_bound(functions,end,rva,[](DWORD value,const RUNTIME_FUNCTION& f){return value<f.BeginAddress;});
        if(found==functions)return nullptr;
        --found;return rva<found->EndAddress?found:nullptr;
    }
    return nullptr;
}
static DWORD64 CALLBACK Base(HANDLE,DWORD64 address){const auto* m=Find(address);return m?m->base:0;}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2 || !Load(argv[1],dump)) return 1;
    auto* list=static_cast<MINIDUMP_MODULE_LIST*>(Stream(ModuleListStream));
    auto* threads=static_cast<MINIDUMP_THREAD_LIST*>(Stream(ThreadListStream));
    if(!list || !threads) return 2;
    HANDLE process=GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS|SYMOPT_FAIL_CRITICAL_ERRORS|SYMOPT_UNDNAME|SYMOPT_IGNORE_NT_SYMPATH);
    if(!SymInitializeW(process,L".",FALSE)) return 3;
    for(unsigned i=0;i<list->NumberOfModules;++i) {
        const auto& item=list->Modules[i];
        auto* name=reinterpret_cast<const MINIDUMP_STRING*>(dump.data()+item.ModuleNameRva);
        Module m{item.BaseOfImage,item.SizeOfImage,std::wstring(name->Buffer,name->Length/sizeof(wchar_t)),{}};
        SymLoadModuleExW(process,nullptr,m.path.c_str(),nullptr,m.base,m.size,nullptr,0);
        modules.push_back(std::move(m));
    }
    if(auto* memory=static_cast<MINIDUMP_MEMORY_LIST*>(Stream(MemoryListStream)))
        for(unsigned i=0;i<memory->NumberOfMemoryRanges;++i) { const auto& r=memory->MemoryRanges[i]; ranges.push_back({r.StartOfMemoryRange,r.Memory.DataSize,r.Memory.Rva}); }
    if(auto* memory=static_cast<MINIDUMP_MEMORY64_LIST*>(Stream(Memory64ListStream))) {
        size_t offset=size_t(memory->BaseRva);
        for(ULONG64 i=0;i<memory->NumberOfMemoryRanges;++i) { const auto& r=memory->MemoryRanges[i]; ranges.push_back({r.StartOfMemoryRange,size_t(r.DataSize),offset}); offset+=size_t(r.DataSize); }
    }
    for(unsigned i=0;i<threads->NumberOfThreads;++i) {const auto& t=threads->Threads[i];ranges.push_back({t.Stack.StartOfMemoryRange,t.Stack.Memory.DataSize,t.Stack.Memory.Rva});}
    std::printf("%lu threads; %zu modules; %zu memory ranges\n",threads->NumberOfThreads,modules.size(),ranges.size());
    for(unsigned i=0;i<threads->NumberOfThreads;++i) {
        const auto& t=threads->Threads[i]; CONTEXT context{};
        if(t.ThreadContext.Rva+t.ThreadContext.DataSize>dump.size()) continue;
        std::memcpy(&context,dump.data()+t.ThreadContext.Rva,std::min(size_t(t.ThreadContext.DataSize),sizeof context));
        std::printf("\nTHREAD %lu RIP=%llX RSP=%llX RCX=%llX RDX=%llX RBX=%llX RSI=%llX RDI=%llX R12=%llX R13=%llX R14=%llX R15=%llX\n",t.ThreadId,context.Rip,context.Rsp,context.Rcx,context.Rdx,context.Rbx,context.Rsi,context.Rdi,context.R12,context.R13,context.R14,context.R15);
        STACKFRAME64 frame{}; frame.AddrPC={context.Rip,0,AddrModeFlat};frame.AddrStack={context.Rsp,0,AddrModeFlat};frame.AddrFrame={context.Rbp,0,AddrModeFlat};
        DWORD64 prior=0;
        for(unsigned depth=0;depth<64;++depth) {
            const auto pc=frame.AddrPC.Offset; if(!pc || (pc==prior && depth)) break; prior=pc;
            const auto* m=Find(pc);
            if(m) std::printf("  #%02u %ls+%llX rsp=%llX\n",depth,std::filesystem::path(m->path).filename().c_str(),pc-m->base,frame.AddrStack.Offset);
            else std::printf("  #%02u %llX rsp=%llX\n",depth,pc,frame.AddrStack.Offset);
            if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64,process,reinterpret_cast<HANDLE>(ULONG_PTR(t.ThreadId)),&frame,&context,Read,Functions,Base,nullptr)) break;
            if(frame.AddrPC.Offset==pc) { // First StackWalk call can describe the initial frame again.
                if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64,process,reinterpret_cast<HANDLE>(ULONG_PTR(t.ThreadId)),&frame,&context,Read,Functions,Base,nullptr)) break;
            }
        }
    }
    SymCleanup(process); return 0;
}
