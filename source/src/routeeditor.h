#pragma once
// Engine-independent route-editing model. This module does not install hooks,
// read map positions, write files, or claim that a route is safe to fly.
#include "trafficdata.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace routeeditor {
using Route = trafficdata::Route;
using Point = trafficdata::Vec;
enum class Context { Gameplay, SurfaceMap, OtherMap, OtherMenu };
enum class F7Action { HudEditor, RouteEditor, None };
inline F7Action F7(Context context) {
    if (context == Context::SurfaceMap) return F7Action::RouteEditor;
    return context == Context::Gameplay ? F7Action::HudEditor : F7Action::None;
}
inline bool Finite(Point p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
inline double Distance(Point a, Point b) { return std::hypot(a.x-b.x,a.y-b.y,a.z-b.z); }
enum class Issue { None, WrongType, MissingName, MissingWorld, InvalidSettings, TooFewPoints, InvalidPoint, DuplicatePoints };
inline Issue Validate(const Route& route) {
    if (route.name.empty()) return Issue::MissingName;
    if (route.world.empty()) return Issue::MissingWorld;
    if (!std::isfinite(route.speed) || route.speed <= 0 || !std::isfinite(route.width) || route.width < 0 || route.lanes < 1 || route.lanes > 2) return Issue::InvalidSettings;
    if (route.ring)  // an edited original ring: its shape, not points
        return std::isfinite(route.shape.cx) && std::isfinite(route.shape.cy) && std::isfinite(route.shape.z) && std::isfinite(route.shape.radius) &&
               route.shape.radius >= 20 ? Issue::None : Issue::InvalidPoint;
    if (route.points.size() < (route.closed ? 3u : 2u)) return Issue::TooFewPoints;
    if(!route.rounding.empty()&&(route.rounding.size()!=route.points.size()||std::any_of(route.rounding.begin(),route.rounding.end(),[](double r){return !std::isfinite(r)||r<0||r>200;})))return Issue::InvalidSettings;
    for (const auto& p : route.points) if (!Finite(p)) return Issue::InvalidPoint;
    for (std::size_t i=1; i<route.points.size(); ++i) if (Distance(route.points[i-1],route.points[i]) < .01) return Issue::DuplicatePoints;
    if (route.closed && Distance(route.points.front(),route.points.back()) < .01) return Issue::DuplicatePoints;
    return Issue::None; // obstacle and landing-site safety require live evidence
}

// Snapshot-based local undo. The owner supplies a draft copy; no methods mutate
// the original route or write to disk. Pointer drags should call Move on release.
class Draft {
    Route value_;
    std::vector<Route> undo_, redo_;
    static constexpr std::size_t kHistory = 64;
    void Checkpoint() {
        if (undo_.size() == kHistory) undo_.erase(undo_.begin());
        undo_.push_back(value_); redo_.clear();
    }
public:
    explicit Draft(Route source): value_(std::move(source)) {}
    const Route& Value() const { return value_; }
    Issue Check() const { return Validate(value_); }
    bool Add(Point p) {
        if (!Finite(p)) return false;
        Checkpoint(); if(!value_.rounding.empty())value_.rounding.push_back(0);value_.points.push_back(p); return true;
    }
    bool Move(std::size_t i, Point p) {
        if (i>=value_.points.size() || !Finite(p)) return false;
        const auto old=value_.points[i];
        if (old.x==p.x && old.y==p.y && old.z==p.z) return true;
        Checkpoint(); value_.points[i]=p; return true;
    }
    bool SetZ(std::size_t i, double z) {
        if (i>=value_.points.size() || !std::isfinite(z)) return false;
        auto p=value_.points[i]; p.z=z; return Move(i,p);
    }
    bool Remove(std::size_t i) {
        if (i>=value_.points.size()) return false;
        Checkpoint();if(i<value_.rounding.size())value_.rounding.erase(value_.rounding.begin()+i);value_.points.erase(value_.points.begin()+i); return true;
    }
    bool Clear() {
        if (value_.points.empty()) return false;
        Checkpoint(); value_.rounding.clear();value_.points.clear(); return true;
    }
    bool Reverse() {
        if (value_.points.size()<2) return false;
        Checkpoint();std::reverse(value_.rounding.begin(),value_.rounding.end()); std::reverse(value_.points.begin(),value_.points.end()); return true;
    }
    bool Round(std::size_t i,double radius){
        if(i>=value_.points.size()||!std::isfinite(radius)||radius<0||radius>200||value_.ring)return false;
        if(!value_.closed&&(i==0||i+1==value_.points.size())&&radius>0)return false;
        Checkpoint();value_.rounding.resize(value_.points.size(),0);value_.rounding[i]=radius;return true;
    }
    void Close(bool closed) { if (value_.closed!=closed) { Checkpoint(); value_.closed=closed; } }
    // Any other change (speed, lanes, schedule, label): one undo step.
    void Edit(const Route& next) { Checkpoint(); value_=next; }
    bool Undo() {
        if (undo_.empty()) return false;
        redo_.push_back(value_); value_=std::move(undo_.back()); undo_.pop_back(); return true;
    }
    bool Redo() {
        if (redo_.empty()) return false;
        undo_.push_back(value_); value_=std::move(redo_.back()); redo_.pop_back(); return true;
    }
};
}
