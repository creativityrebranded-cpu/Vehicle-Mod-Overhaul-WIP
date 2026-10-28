#include "framework.h"
#include <array>
#include <cstdio>
#include <vector>

struct Source {
    std::vector<framework::Cell> cells;
    unsigned current=0;
    std::vector<std::pair<std::uintptr_t,unsigned>> visits;
    framework::Cell Describe(unsigned i) { current=i;return cells[i]; }
    void Visit(unsigned i) { visits.emplace_back(cells[current].identity,i); }
};
int main(){
    int checks=0,failed=0;
    auto check=[&](bool ok,const char* name){++checks;if(!ok){++failed;std::printf("FAIL: %s\n",name);}};
    framework::ScanCursor cursor;
    Source dense{{{1,8192}}};
    bool bounded=true,done=false;unsigned frames=0;
    while(!done&&frames<100){const auto before=dense.visits.size();done=framework::ScanStep(cursor,1,dense,[]{return false;});bounded=bounded&&dense.visits.size()-before<=256;++frames;}
    check(done&&bounded&&frames==32&&dense.visits.size()==8192,"dense city cell completes in 32 bounded slices");
    bool unique=true;for(unsigned i=0;i<dense.visits.size();++i)unique=unique&&dense.visits[i]==std::make_pair(std::uintptr_t(1),i);
    check(unique&&cursor.cell==0&&cursor.ref==0&&!cursor.identity,"all dense-cell references are visited exactly once and cursor resets");
    Source many{{{1,250},{0,0},{2,300},{3,1}}};cursor={};frames=0;done=false;
    while(!done&&frames++<10)done=framework::ScanStep(cursor,4,many,[]{return false;});
    check(done&&many.visits.size()==551,"empty cells and partially visited cells do not lose references");
    Source timed{{{1,8192}}};cursor={};
    check(!framework::ScanStep(cursor,1,timed,[]{return true;})&&timed.visits.size()==16&&cursor.ref==16,"expired time budget yields with bounded forward progress");
    timed.cells[0]={2,20};
    check(framework::ScanStep(cursor,1,timed,[]{return false;})&&timed.visits[16]==std::make_pair(std::uintptr_t(2),0u),"replaced cell restarts at its first reference");
    cursor={};Source shrink{{{1,8192}}};framework::ScanStep(cursor,1,shrink,[]{return false;});shrink.cells[0].count=8;
    check(framework::ScanStep(cursor,1,shrink,[]{return false;}),"shrinking reference array cannot stall or read beyond its new bound");
    Source empty{};cursor={4,100,99};
    check(framework::ScanStep(cursor,0,empty,[]{return false;})&&!cursor.identity,"empty grid clears an outstanding cursor");
    framework::Retry retry;std::uint64_t now=1000;
    check(retry.Ready(now),"new pilot can attempt seating immediately");
    bool backoff=true;for(const auto delay:std::array<unsigned,7>{2000,4000,8000,16000,30000,30000,30000}){
        retry.Failed(now);backoff=backoff&&!retry.Ready(now+delay-1)&&retry.Ready(now+delay);now+=delay;
    }
    check(backoff,"persistent failures back off to a bounded 30-second interval");
    retry.Clear();check(retry.Ready(0)&&retry.failures==0,"seating or reload clears the previous failure delay");
    std::printf("framework: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
