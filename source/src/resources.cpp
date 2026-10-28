#include "resources.h"
#include "log.h"
#include <Windows.h>
#include <Psapi.h>
#include <dxgi1_4.h>
#include <array>

namespace resources {
namespace {
// SFSE retains this DLL for the process lifetime. Keep observation interfaces
// for that lifetime; do not release DXGI objects under DLL-detach loader lock.
std::array<IDXGIAdapter3*,8> adapters{};
bool initialized=false,gpuSlow=false;
void Init() {
    initialized=true;IDXGIFactory1* factory=nullptr;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return;
    for(UINT i=0;i<adapters.size();++i) {
        IDXGIAdapter1* adapter=nullptr;
        if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND)break;
        if(!adapter)continue;
        DXGI_ADAPTER_DESC1 desc{};
        if(SUCCEEDED(adapter->GetDesc1(&desc))&&!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) {
            adapter->QueryInterface(IID_PPV_ARGS(&adapters[i]));
            logger::Write("Vehicle GPU adapter: %ls, dedicated %.0f MiB",desc.Description,double(desc.DedicatedVideoMemory)/1048576);
        }
        adapter->Release();
    }
    factory->Release();
}
}
void Tick() {
    static ULONGLONG previousAt=0;const auto now=GetTickCount64();
    if(previousAt&&now-previousAt<2000)return;
    if(!logger::Verbose())return;
    const auto elapsed=previousAt?now-previousAt:0;previousAt=now;
    LARGE_INTEGER start{},end{},frequency{};QueryPerformanceCounter(&start);QueryPerformanceFrequency(&frequency);
    const bool first=!initialized;if(first)Init();
    PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
    const bool memoryOk=K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory))!=0;
    MEMORYSTATUSEX system{};system.dwLength=sizeof(system);const bool systemOk=GlobalMemoryStatusEx(&system)!=0;
    IO_COUNTERS io{};const bool ioOk=GetProcessIoCounters(GetCurrentProcess(),&io)!=0;
    static IO_COUNTERS previousIo{};static DWORD previousFaults=0;static bool haveIo=false,haveMemory=false;
    const double readRate=ioOk&&haveIo&&elapsed&&io.ReadTransferCount>=previousIo.ReadTransferCount?
        double(io.ReadTransferCount-previousIo.ReadTransferCount)*1000/double(elapsed)/1048576:-1;
    const auto faults=memoryOk&&haveMemory?DWORD(memory.PageFaultCount-previousFaults):0;
    if(ioOk){previousIo=io;haveIo=true;}if(memoryOk){previousFaults=memory.PageFaultCount;haveMemory=true;}
    DXGI_QUERY_VIDEO_MEMORY_INFO chosen{},shared{};int selected=-1;
    if(!gpuSlow)for(std::size_t i=0;i<adapters.size();++i)if(adapters[i]) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if(SUCCEEDED(adapters[i]->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&info))&&info.Budget&&
            (selected<0||info.CurrentUsage>chosen.CurrentUsage)){chosen=info;selected=int(i);}
    }
    if(selected>=0)adapters[std::size_t(selected)]->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,&shared);
    QueryPerformanceCounter(&end);const double diagnosticMs=1000.0*double(end.QuadPart-start.QuadPart)/frequency.QuadPart;
    logger::Write("Vehicle resources: pid %lu, RAM working %.0f/private %.0f MiB, system available %.0f MiB, faults +%lu (includes soft), process reads %.2f MiB/s (not disk-only); GPU adapter %d local %.0f/budget %.0f MiB, nonlocal %.0f MiB%s; diagnostic %.3f ms",
        GetCurrentProcessId(),memoryOk?double(memory.WorkingSetSize)/1048576:-1,memoryOk?double(memory.PrivateUsage)/1048576:-1,
        systemOk?double(system.ullAvailPhys)/1048576:-1,faults,readRate,selected,double(chosen.CurrentUsage)/1048576,
        double(chosen.Budget)/1048576,double(shared.CurrentUsage)/1048576,selected<0?" (GPU unavailable)":chosen.CurrentUsage>chosen.Budget?" OVER BUDGET":"",diagnosticMs);
    if(!first&&diagnosticMs>2&&!gpuSlow){gpuSlow=true;logger::Write("Vehicle resources: GPU queries suspended after a slow diagnostic; RAM/IO observation retained");}
}
}
