#pragma once
#include <cstdint>
namespace environment {
enum class Verdict { Pending, Safe, Unsafe };
struct Site {Verdict verdict=Verdict::Pending;double ground=0,x=0,y=0,z=0;bool supported=false;const char* reason="Checking ground";};
Site CheckSite(std::uint32_t vehicle,double x,double y,double z,double yaw);
Site BoardingSite(std::uintptr_t vehicle);
bool ClearShot(std::uint32_t self,std::uint32_t target,std::uint32_t targetVehicle,const float from[3],const float to[3]);
bool CombatPosition(std::uint32_t self,const float enemy[3],double&x,double&y,double&z,bool defensive);
void Tick();void Reset();
}
