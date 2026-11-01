// Exercise the real async dump writer with a synthetic allocator exception.
#include "../src/crash.cpp"
#include <cstdarg>
#include <fstream>
#include <filesystem>
#include <vector>
namespace logger {
void Write(const char*,...){}
void WriteUnlocked(const char* fmt,...){va_list args;va_start(args,fmt);std::vprintf(fmt,args);va_end(args);std::puts("");}
}
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 1;
    const std::wstring folder=argv[1];
    crash::g_allocatorDumpPath=folder+L"/allocator-test.dmp";
    crash::g_dumpPath=folder+L"/unhandled-test.dmp";
    const auto help=LoadLibraryW(L"dbghelp.dll");
    crash::g_writeDump=reinterpret_cast<decltype(&MiniDumpWriteDump)>(GetProcAddress(help,"MiniDumpWriteDump"));
    crash::g_dumpAsk=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    crash::g_dumpDone=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!crash::g_writeDump||!crash::g_dumpAsk||!crash::g_dumpDone)return 2;
    const auto writer=CreateThread(nullptr,0,crash::DumpThread,nullptr,0,nullptr);
    if(!writer)return 3;
    int checks=0,failures=0;
    auto check=[&](bool ok,const char* why){++checks;if(!ok){++failures;std::printf("FAIL %s\n",why);}};
    crash::g_gameBase=0x140000000;
    EXCEPTION_RECORD record{};CONTEXT context{};RtlCaptureContext(&context);
    record.ExceptionCode=EXCEPTION_ACCESS_VIOLATION;
    record.ExceptionAddress=reinterpret_cast<void*>(crash::g_gameBase+0x22D3390);
    record.NumberParameters=2;record.ExceptionInformation[0]=0;record.ExceptionInformation[1]=0;
    EXCEPTION_POINTERS info{&record,&context};
    check(crash::AllocatorFault(record),"allocator fault selected");
    record.ExceptionAddress=reinterpret_cast<void*>(crash::g_gameBase+0x500000);
    check(!crash::CaptureAllocatorFault(&info),"ordinary engine handled fault ignored");
    record.ExceptionAddress=reinterpret_cast<void*>(crash::g_gameBase+0x22D3390);
    crash::t_expected=true;check(!crash::CaptureAllocatorFault(&info),"expected probe ignored");crash::t_expected=false;
    crash::g_dumpBusy.test_and_set();check(!crash::CaptureAllocatorFault(&info)&&!crash::g_allocatorCaptured,"busy writer permits later retry");crash::g_dumpBusy.clear();
    check(crash::CaptureAllocatorFault(&info),"first fault captured before unhandled dispatch");
    check(!crash::CaptureAllocatorFault(&info),"one capture per process");
    std::ifstream file(std::filesystem::path(crash::g_allocatorDumpPath),std::ios::binary|std::ios::ate);
    const auto length=file.tellg();std::vector<char> data(static_cast<size_t>(length));file.seekg(0);file.read(data.data(),length);
    PMINIDUMP_DIRECTORY dir=nullptr;void* stream=nullptr;ULONG size=0;
    check(MiniDumpReadDumpStream(data.data(),ExceptionStream,&dir,&stream,&size)!=0,"dump contains exception stream");
    if(stream){const auto& exception=*static_cast<MINIDUMP_EXCEPTION_STREAM*>(stream);
        check(exception.ExceptionRecord.ExceptionCode==EXCEPTION_ACCESS_VIOLATION&&exception.ExceptionRecord.ExceptionAddress==reinterpret_cast<ULONG64>(record.ExceptionAddress),"original fault preserved");
        check(exception.ThreadId==GetCurrentThreadId()&&exception.ThreadContext.DataSize>=sizeof(CONTEXT),"fault thread and context preserved");}
    check(crash::WriteDump(&info),"unhandled dump still writes independently");
    check(std::filesystem::exists(crash::g_allocatorDumpPath)&&std::filesystem::exists(crash::g_dumpPath),"first fault and unhandled files both retained");
    std::printf("allocator recorder: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
