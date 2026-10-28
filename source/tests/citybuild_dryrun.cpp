#include "citystartup.h"
#include <filesystem>
#include <cstdio>
int main(int argc,char** argv){
    if(argc!=3){std::puts("citybuild_dryrun <isolated-settings-folder> <existing-plugin-copy>");return 2;}
    const auto r=citystartup::RebuildAt(std::filesystem::path(argv[2]).wstring(),std::filesystem::path(argv[1]).wstring());
    std::printf("ok=%d changed=%d parked=%zu recorded=%zu removed=%zu identities=%zu error=%s\n",r.ok,r.changed,r.parked,r.recorded,r.removed,r.active.size(),r.error.c_str());
    return r.ok&&r.error.empty()?0:1;
}
