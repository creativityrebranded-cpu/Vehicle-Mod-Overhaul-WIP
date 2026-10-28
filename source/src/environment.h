#pragma once
#include <cstdint>
namespace environment {
enum class Verdict { Pending, Safe, Unsafe };
struct Site {Verdict verdict=Verdict::Pending;double ground=0,x=0,y=0,z=0;bool supported=false;const char* reason="Checking ground";};
Site CheckSite(std::uint32_t vehicle,double x,double y,double z,double yaw);
Site BoardingSite(std::uintptr_t vehicle);
// Entry only: Safe once the vehicle is at rest on or just above the ground (BoardingSite is for exits and drop-offs).
Site EntrySite(std::uintptr_t vehicle);
bool ClearShot(std::uint32_t self,std::uint32_t target,std::uint32_t targetVehicle,const float from[3],const float to[3]);
bool CombatPosition(std::uint32_t self,const float enemy[3],double&x,double&y,double&z,bool defensive);
// The nav map's line of sight (terrain, mapped solids, object boxes), answered at once (vehicle manoeuvres hide behind terrain with it).
bool LineBlocked(const float from[3],const float to[3]);
// Along the way ahead (downward rays, up to `reach` m, cached): water, or rough ground (steep, or a big step). Answers fill in
// over a few frames; known = at least two points answered.
struct Surface {bool known=false,water=false,rough=false;double at=0;};
Surface SurfaceAhead(std::uint32_t self,double x,double y,double z,double bearing,double reach);
// One downward ray at (x, y) between top and bottom (layer mask as SurfaceAhead): false while it is pending. The answer is
// cached 30 s; ask again each frame until it comes (an unasked query is dropped after 2 s). Ground planner sensing (groundnav).
struct Ground {bool hit=false,water=false;double z=0,nz=1;bool normalOk=false;};
bool GroundAt(std::uint32_t self,double x,double y,double top,double bottom,Ground& out);
unsigned Pending(); // queries waiting for a ray slot (the ground sensing keeps its asks below a share of the queue)
void Tick();void Reset();
}
