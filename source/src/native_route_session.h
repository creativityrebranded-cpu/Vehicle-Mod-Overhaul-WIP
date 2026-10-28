#pragma once

#include <cmath>
#include <cstdint>

// Request ownership independent of any engine ABI. The engine adapter must
// provide the route and the physical vehicle controller consumes it.
namespace native_route_session {
struct Point { double x=0, y=0, z=0; };
struct Request { std::uint32_t vehicle=0; std::uint64_t generation=0; Point destination{}; };
enum class Status { Idle, Waiting, Following, Blocked, Arrived, Unavailable };
// The existing goal contract uses NaN for an unspecified terrain-relative height.
inline bool Valid(Point p) { return std::isfinite(p.x) && std::isfinite(p.y) && !std::isinf(p.z); }

class Session {
public:
    Request Begin(std::uint32_t vehicle, Point destination) {
        ++generation_;
        vehicle_=vehicle;
        status_=(vehicle && Valid(destination)) ? Status::Waiting : Status::Unavailable;
        return {vehicle_,generation_,destination};
    }
    bool Publish(std::uint32_t vehicle, std::uint64_t generation, Status result) {
        if (!vehicle || vehicle!=vehicle_ || generation!=generation_ || status_!=Status::Waiting)
            return false;
        if (result!=Status::Following && result!=Status::Blocked && result!=Status::Arrived && result!=Status::Unavailable)
            return false;
        status_=result;
        return true;
    }
    void Cancel() { ++generation_; vehicle_=0; status_=Status::Idle; }
    Status GetStatus() const { return status_; }
private:
    std::uint32_t vehicle_=0;
    std::uint64_t generation_=0;
    Status status_=Status::Idle;
};
}
