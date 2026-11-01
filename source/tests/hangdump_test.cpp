// Exercise the real watchdog and DbgHelp writer without launching Starfield.
#include "../src/crash.cpp"
#include <cstdarg>
#include <vector>
namespace logger {
void Write(const char*, ...) {}
void WriteUnlocked(const char* fmt, ...) {
    va_list args; va_start(args, fmt); std::vprintf(fmt, args); va_end(args); std::puts("");
}
}
DWORD WINAPI WaitingWorker(void* event) { WaitForSingleObject(event, INFINITE); return 0; }
int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 1;
    const std::wstring path = argv[1];
    const auto help = LoadLibraryW(L"dbghelp.dll");
    crash::g_writeDump = help ? reinterpret_cast<decltype(&MiniDumpWriteDump)>(GetProcAddress(help, "MiniDumpWriteDump")) : nullptr;
    if (!crash::g_writeDump) return 2;
    DWORD error = 0;
    crash::g_dumpBusy.test_and_set();
    const bool busy = !crash::WriteHangDump(path, error) && error == ERROR_BUSY;
    crash::g_dumpBusy.clear();
    if (!busy) return 3;
    if (crash::WriteHangDump(path + L"\\missing\\dump.dmp", error) || !error || crash::g_dumpBusy.test_and_set()) return 4;
    crash::g_dumpBusy.clear();
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD workerId = 0;
    HANDLE worker = CreateThread(nullptr, 0, WaitingWorker, event, 0, &workerId);
    if (!event || !worker) return 5;
    // Exercise the short-hitch writer as well, including busy/failure recovery.
    crash::g_dumpBusy.test_and_set();
    const bool stallBusy=!crash::WriteStallDump(path+L".stall.dmp",error)&&error==ERROR_BUSY;
    crash::g_dumpBusy.clear();if(!stallBusy)return 12;
    if(crash::WriteStallDump(path+L"\\missing\\stall.dmp",error)||!error)return 13;
    const auto stallPath=path+L".stall.dmp";
    if(!crash::WriteStallDump(stallPath,error))return 14;
    {const HANDLE f=CreateFileW(stallPath.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
     if(f==INVALID_HANDLE_VALUE)return 15;const DWORD smallSize=GetFileSize(f,nullptr);std::vector<unsigned char> smallBytes(smallSize);DWORD readBytes=0;
     const bool valid=ReadFile(f,smallBytes.data(),smallSize,&readBytes,nullptr)&&readBytes==smallSize;CloseHandle(f);
     if(!valid||smallSize<sizeof(MINIDUMP_HEADER))return 16;const auto* h=reinterpret_cast<const MINIDUMP_HEADER*>(smallBytes.data());
     if(h->Signature!=MINIDUMP_SIGNATURE||h->StreamDirectoryRva+std::uint64_t(h->NumberOfStreams)*sizeof(MINIDUMP_DIRECTORY)>smallSize)return 17;
     const auto* dirs=reinterpret_cast<const MINIDUMP_DIRECTORY*>(smallBytes.data()+h->StreamDirectoryRva);bool waitingStack=false,mainThreadStack=false,moduleList=false,info=false;
     for(unsigned i=0;i<h->NumberOfStreams;++i){const auto& d=dirs[i];if(std::uint64_t(d.Location.Rva)+d.Location.DataSize>smallSize)return 18;
      if(d.StreamType==ModuleListStream)moduleList=true;if(d.StreamType==ThreadInfoListStream)info=true;
      if(d.StreamType!=ThreadListStream)continue;const auto* list=reinterpret_cast<const MINIDUMP_THREAD_LIST*>(smallBytes.data()+d.Location.Rva);
      for(unsigned j=0;j<list->NumberOfThreads;++j){const auto& t=list->Threads[j];const bool have=t.Stack.Memory.DataSize&&t.ThreadContext.DataSize&&std::uint64_t(t.Stack.Memory.Rva)+t.Stack.Memory.DataSize<=smallSize;
       if(t.ThreadId==workerId)waitingStack=have;if(t.ThreadId==GetCurrentThreadId())mainThreadStack=have;}
     }
     std::printf("Short-stall dump: main stack %d, waiting worker stack %d, modules %d, thread info %d\n",mainThreadStack,waitingStack,moduleList,info);
     if(!waitingStack||!mainThreadStack||!moduleList||!info)return 19;}
    crash::g_hangPath = path;
    crash::g_hangDelay = 1000; // Shortened only in this test.
    crash::Beat();
    HANDLE watch = CreateThread(nullptr, 0, crash::FreezeWatch, nullptr, 0, nullptr);
    if (!watch) return 6;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 150; ++i) {
        Sleep(100);
        file = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
    }
    if (file == INVALID_HANDLE_VALUE) return 7;
    const DWORD size = GetFileSize(file, nullptr);
    std::vector<unsigned char> bytes(size);
    DWORD got = 0;
    const bool read = ReadFile(file, bytes.data(), size, &got, nullptr) && got == size;
    CloseHandle(file);
    SetEvent(event); WaitForSingleObject(worker, 5000); CloseHandle(worker); CloseHandle(event); CloseHandle(watch);
    if (!read || size < sizeof(MINIDUMP_HEADER)) return 8;
    auto* header = reinterpret_cast<const MINIDUMP_HEADER*>(bytes.data());
    if (header->Signature != MINIDUMP_SIGNATURE || header->StreamDirectoryRva + header->NumberOfStreams * sizeof(MINIDUMP_DIRECTORY) > size) return 9;
    auto* directory = reinterpret_cast<const MINIDUMP_DIRECTORY*>(bytes.data() + header->StreamDirectoryRva);
    bool mainStack = false, workerStack = false, threadInfo = false, modules = false;
    unsigned threads = 0;
    for (unsigned i = 0; i < header->NumberOfStreams; ++i) {
        const auto& entry = directory[i];
        if (entry.Location.Rva + entry.Location.DataSize > size) return 10;
        if (entry.StreamType == ThreadInfoListStream) threadInfo = true;
        if (entry.StreamType == ModuleListStream) modules = true;
        if (entry.StreamType != ThreadListStream) continue;
        auto* list = reinterpret_cast<const MINIDUMP_THREAD_LIST*>(bytes.data() + entry.Location.Rva);
        threads = list->NumberOfThreads;
        for (unsigned j = 0; j < threads; ++j) {
            const auto& t = list->Threads[j];
            const bool stack = t.Stack.Memory.DataSize && t.ThreadContext.DataSize && t.Stack.Memory.Rva + t.Stack.Memory.DataSize <= size;
            if (t.ThreadId == GetCurrentThreadId()) mainStack = stack;
            if (t.ThreadId == workerId) workerStack = stack;
        }
    }
    std::printf("Watchdog dump: %u threads, main stack %d, waiting worker stack %d, thread info %d, modules %d\n", threads, mainStack, workerStack, threadInfo, modules);
    return threads >= 3 && mainStack && workerStack && threadInfo && modules ? 0 : 11;
}
