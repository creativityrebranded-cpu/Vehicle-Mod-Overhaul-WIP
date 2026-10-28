#include "log.h"
#include "controlindex.h"
#include "refcollect.h"
#include "framework.h"
#include <Windows.h>
#include <cstdio>
#include <io.h>
#include <string>
#include <vector>

namespace plugininfo {
const wchar_t* Name(){return L"PerformanceTest";}
const wchar_t* TelemetryPrefix(){return L"test-";}
}
namespace trace { void Raw(const char*,const char*){} }
namespace logger::testing { void Open(std::FILE*);void Clock(ULONGLONG); }
struct Cells {
    unsigned visits=0;
    framework::Cell Describe(unsigned index){return {index+1,1};}
    void Visit(unsigned){++visits;}
};
int main(){
    int checks=0,failed=0;
    auto check=[&](bool ok,const char* name){++checks;if(!ok){++failed;std::printf("FAIL: %s\n",name);}};
    auto file=std::tmpfile();
    check(file!=nullptr,"temporary output opens");if(!file)return 1;
    logger::testing::Open(file);logger::testing::Clock(1000);
    logger::Write("startup-marker");const auto startup=_filelengthi64(_fileno(file));
    check(startup>0,"startup output reaches disk immediately");
    logger::Service();logger::Write("batched-marker");
    check(_filelengthi64(_fileno(file))==startup,"gameplay output is buffered without a disk flush per line");
    logger::testing::Clock(1249);logger::Service();
    check(_filelengthi64(_fileno(file))==startup,"buffer remains batched before its deadline");
    logger::testing::Clock(1250);logger::Service();const auto serviced=_filelengthi64(_fileno(file));
    check(serviced>startup,"frame service flushes at the 250 ms deadline even with no new messages");
    logger::Write("pending-before-fault");logger::WriteUnlocked("fault-marker");
    check(_filelengthi64(_fileno(file))>serviced,"fault output immediately flushes pending gameplay messages too");
    const auto beforeLarge=_filelengthi64(_fileno(file));
    const std::string payload(1024,'x');for(int i=0;i<40;++i)logger::Write("%s",payload.c_str());
    check(_filelengthi64(_fileno(file))>beforeLarge,"buffer size threshold bounds unflushed output during bursts");
    logger::testing::Clock(1500);logger::Service();
    std::rewind(file);std::string text;char line[4096];while(std::fgets(line,sizeof(line),file))text+=line;
    check(text.find("startup-marker")<text.find("batched-marker")&&text.find("pending-before-fault")<text.find("fault-marker"),"batching preserves message order and fault breadcrumbs");
    logger::testing::Open(nullptr);std::fclose(file);
    controlindex::Index index;
    index.Add("Vehicle","Forward",0);index.Add("Vehicle","Forward",0xFF);index.Add("Vehicle","Forward",87);index.Add("Vehicle","Forward",99);
    check(index.Find("Vehicle","Forward",1)==87,"first valid binding wins as in the original resolver");
    check(index.Find("MainGameplay","Forward",65)==65,"lookup keeps control contexts separate");
    check(index.Find("Vehicle","Missing",42)==42,"missing keyboard binding retains its fallback");
    check(index.Find("Vehicle","Missing")==0,"missing pad binding remains unbound");
    index.Clear();index.Add("Vehicle","Forward",90);
    check(index.Find("Vehicle","Forward")==90,"rebinding replaces the previous lookup index");
    std::vector<std::uintptr_t> refs;unsigned callbacks=0;
    for(std::uintptr_t id=1;id<=100000;++id){++callbacks;if(refcollect::Append(refs,id,30)!=1)break;}
    check(callbacks==30&&refs.size()==30&&refs.front()==1&&refs.back()==30,"native collector stops at its limit while preserving the same reference prefix");
    refs.clear();check(refcollect::Append(refs,std::uintptr_t(0),30)==1&&refs.empty(),"empty reference does not consume the output limit");
    check(refcollect::Append(refs,std::uintptr_t(1),0)==0&&refs.empty(),"zero-limit scan performs no collection");
    framework::ScanCursor cursor;Cells cells;bool done=false;unsigned frames=0;bool bounded=true;
    while(!done&&frames<10){const auto before=cells.visits;done=framework::ScanStep(cursor,4,cells,[]{return false;},1);bounded=bounded&&cells.visits-before<=1;++frames;}
    check(done&&bounded&&frames==4&&cells.visits==4,"registry cell batches finish without processing multiple cells in one frame");
    std::printf("performance: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
