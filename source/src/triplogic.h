#pragma once
#include <cmath>
namespace triplogic {
enum class Phase { Idle, Travelling, WaitingTerrain, Blocked, FindingWay, AdjustingHeight, Arrived };
inline bool Reached(double distance, double stop, double heightError, double speed, bool ready) {
 return ready && std::isfinite(distance) && std::isfinite(heightError) && std::isfinite(speed) &&
  distance <= stop + 2.0 && std::fabs(heightError) <= 2.5 && speed <= 1.5;
}
inline Phase Status(bool ready, bool reached, bool near, double heightError, bool escaping, bool blocked) {
 if(!ready) return Phase::WaitingTerrain;
 if(reached) return Phase::Arrived;
 if(near && std::fabs(heightError)>2.5) return Phase::AdjustingHeight;
 if(escaping) return Phase::FindingWay;
 if(blocked) return Phase::Blocked;
 return Phase::Travelling;
}
template<class Queue, class Point, class Place>
bool Advance(Queue& queue, Point& current, bool& haveCurrent, bool patrol, bool skipped, Place place) {
 if(queue.empty()) return false;
 const Point next=queue.front(), previous=current;
 const bool hadCurrent=haveCurrent;
 if(!place(next)) return false;
 queue.erase(queue.begin());
 if(patrol && !skipped && hadCurrent) queue.push_back(previous);
 current=next; haveCurrent=true;
 return true;
}
inline const char* Text(Phase p) {
 switch(p) {
 case Phase::Travelling:return "Travelling";
 case Phase::WaitingTerrain:return "Waiting for terrain";
 case Phase::Blocked:return "Blocked";
 case Phase::FindingWay:return "Finding another way";
 case Phase::AdjustingHeight:return "Adjusting height";
 case Phase::Arrived:return "Arrived";
 default:return "Waiting for driver";
 }
}
}
