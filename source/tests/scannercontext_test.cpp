#include "scannercontext.h"
#include <cstdio>
#include <cmath>
int main() {
 int failures=0;
 auto check=[&](bool ok,const char* what){std::printf("%s %s\n",ok?"PASS":"FAIL",what); if(!ok) ++failures;};
 using scannercontext::PoiScore;
 check(std::isfinite(PoiScore("Outpost", "Outpost",1000,10,100,1000,1000)),"selected named destination wins over near collision");
 check(!std::isfinite(PoiScore("Other", "Outpost",1000,0,100,1000,1000)),"different name cannot steal selection");
 check(!std::isfinite(PoiScore("Outpost", "Outpost",600,0,-1,600,1000)),"repeated name at wrong distance rejected");
 check(!std::isfinite(PoiScore("Outpost", "",1000,0,100,1000,0)),"ordinary aiming respects foreground obstacle");
 check(std::isfinite(PoiScore("Outpost", "",1000,0,-1,1000,0)),"ordinary aiming still snaps to visible map point");
 check(!std::isfinite(PoiScore("Outpost", "Outpost",1000,500,-1,1000,1000)),"off-axis named point cannot steal selection");
 check(scannercontext::RangeMeters("1.25 km")==1250,"native km range converted to metres");
 check(scannercontext::RangeMeters("1,250 m")==1250,"native thousands separator preserved");
 check(scannercontext::RangeMeters(" 325M")==325,"native compact uppercase metres");
 check(scannercontext::RangeMeters("--")==0,"unavailable scanner range rejected");
 check(scannercontext::RangeMeters("NaN m")==0,"invalid range rejected");
 check(scannercontext::Range3D(30,0,40)==50,"target range includes height like scanner");
 return failures ? 1 : 0;
}
