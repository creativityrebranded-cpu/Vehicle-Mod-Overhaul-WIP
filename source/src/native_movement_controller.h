#pragma once

#include "native_movement_output.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace native_movement {

// The factory's R9 is a borrowed component DB view, not a BGSPathingData*.
// 140689180 reads **view and then stored-record+0x18 (component+8).
// Its caller must keep the
// native DB read handle alive throughout Open; this class never fabricates it.
struct PathingDataLink { const void* object = nullptr; };
struct PathingDataView { const PathingDataLink* link = nullptr; };

struct Bounds {
    Vector min{}, max{};
    bool measured = false;
    bool Valid() const;
};

enum class ControllerStatus { Closed, Ready, Waiting, Following, Arrived, Blocked, Unavailable };

// All functions below act on the private component or its private interfaces.
// There is deliberately no lookup of the driver's controller, actor+0x240,
// global component registration, or 1412ab5a0 reference-based submission here.
struct ControllerApi {
    bool (*isNonActorReference)(std::uint32_t) = nullptr;
    void (*construct)(void*, void*, std::uint32_t, const PathingDataView*) = nullptr;
    void (*destroy)(void*) = nullptr;
    void (*activate)(void*) = nullptr;
    void (*selectNormal)(void*) = nullptr;
    void* (*lookup)(void*, const void*) = nullptr;
    void (*update)(void*, float, const std::uint8_t*, void*, void*, bool) = nullptr;
    const void* setGoalKey = nullptr;
    const void* storedOutputKey = nullptr;
    // Optional destination bridge. Returns one retained native request, or null.
    void* (*makeRequest)(void* movementData, Vector position, float stopDistance, bool flight) = nullptr;
    void (*releaseRequest)(void*) = nullptr;
    bool Complete() const;
};

struct ControllerFrame {
    ControllerStatus status = ControllerStatus::Closed;
    std::uint64_t generation = 0;
    Motion motion{};
};

class OwnedController {
public:
    explicit OwnedController(ControllerApi api) : api_(api) {}
    ~OwnedController();
    OwnedController(const OwnedController&) = delete;
    OwnedController& operator=(const OwnedController&) = delete;
    OwnedController(OwnedController&&) = delete;
    OwnedController& operator=(OwnedController&&) = delete;

    // Serial main-thread use only, with a live reference and native DB view.
    // Bounds are in the native mover's local coordinate system, not world AABB.
    bool Open(std::uint32_t formId, const PathingDataView&, Bounds);
    // request is an initialized, caller-retained native PathingRequest pointer.
    // The native arbiter retains it on success; this call borrows the local slot.
    bool Submit(void* request);
    // Position is in the mover's current reference frame. Terrain-relative NaN
    // heights must be resolved before this boundary; raw XYZ is never passed
    // as a native BSPathingLocation.
    bool SubmitDestination(Vector position, float stopDistance, bool flight);
    ControllerFrame Update(float seconds, bool flight);
    void Cancel();
    void Close();
    ControllerStatus Status() const { return status_; }
    std::uint64_t Generation() const { return generation_; }
    std::uint32_t FormId() const { return formId_; }

private:
    // MovementControllerComponent is moved into a 0xa0 stored-component record
    // at record+0x20 by 14068ad50: the complete component itself is 0x80 bytes.
    alignas(16) std::array<std::byte, 0x80> component_{};
    // Native data's primary table has methods through +0x180. The final entry
    // is private context for our geometry overrides, never an engine method.
    std::array<std::uintptr_t, 50> dataTable_{};
    ControllerApi api_;
    Bounds bounds_{};
    void* data_ = nullptr;
    std::uintptr_t originalDataTable_ = 0;
    std::uint32_t formId_ = 0;
    std::uint64_t generation_ = 0;
    ControllerStatus status_ = ControllerStatus::Closed;
    bool constructed_ = false;
    void* Interface(const void* key) const;
    static OwnedController& Owner(void* data);
    static float Radius(void* data);
    static float Width(void* data);
    static float Length(void* data);
    static float Height(void* data);
    static void* GetBounds(void* data, void* out);
};

// Resolves only the pinned 1.16.244 ABI and checks code/table fingerprints.
// Returns an empty API on mismatch; constructing the API does not call engine code.
ControllerApi ResolveControllerApi(std::uintptr_t imageBase);

} // namespace native_movement
