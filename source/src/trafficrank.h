#pragma once
// One right-of-way ladder shared by the autopilot, the lane planner and the field gate (user, 2026-11-03):
// Player > Combat/pursuit > Escorts/followers > Scripted trips > Traffic > Civil > Parked.
// A lower rank yields to a higher one. Collision safety ignores rank (autodrive keeps clear of everyone).
// Combat's own take-over of a vehicle stays where it is (npcvehicles, frozen); this ladder only ranks movement.
namespace trafficrank {
enum class Rank : int { Parked=0, Civil=1, Traffic=2, Scripted=3, Escort=4, Combat=5, Player=6 };
// Existing numbers are kept (player 120, scripted 100, traffic 60, civil 10); combat and escorts move up from 60.
inline int Priority(Rank r) {constexpr int p[]{0,10,60,100,105,110,120};return p[int(r)];}
inline const char* Name(Rank r) {constexpr const char* n[]{"parked","civil","traffic","scripted","escort","combat","player"};return n[int(r)];}
struct Facts {bool player=false,pursuit=false,following=false,parked=false;int navPriority=0;};
inline Rank Of(const Facts& f) {
    if(f.player)return Rank::Player;
    if(f.pursuit)return Rank::Combat;
    if(f.following)return Rank::Escort;
    if(f.navPriority>=100)return Rank::Scripted;
    if(f.navPriority>0&&f.navPriority<=10)return Rank::Civil;
    if(f.parked)return Rank::Parked;
    return Rank::Traffic;
}
// The number the planners compare. A job's own explicit priority stays unless the ladder ranks it higher.
inline int PriorityOf(const Facts& f) {
    const int ladder=Priority(Of(f));
    return f.navPriority>0&&!f.player&&!f.pursuit&&!f.following?f.navPriority:ladder;
}
// Combat outranks every authored field: an engaged vehicle is not held to a road field.
inline bool FieldReleased(Rank r) {return r>=Rank::Combat&&r!=Rank::Player;}
}
