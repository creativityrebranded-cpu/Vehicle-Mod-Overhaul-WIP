#include "native_movement_controller.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace native_movement {
namespace {
template<class T> T Read(const void* object, std::size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}
template<class T> void Write(void* object, std::size_t offset, T value) {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof value);
}
template<class T> T Method(void* object, std::size_t offset) {
    return reinterpret_cast<T>(Read<std::uintptr_t>(Read<void*>(object, 0), offset));
}
}

bool Bounds::Valid() const {
    return measured && Finite(min) && Finite(max) &&
        min.x < max.x && min.y < max.y && min.z < max.z &&
        std::isfinite(max.x - min.x) && std::isfinite(max.y - min.y) &&
        std::isfinite(max.z - min.z);
}

bool ControllerApi::Complete() const {
    return isNonActorReference && construct && destroy && activate &&
        selectNormal && lookup && update && setGoalKey && storedOutputKey;
}

OwnedController::~OwnedController() { Close(); }

void* OwnedController::Interface(const void* key) const {
    auto* registry = Read<void*>(component_.data(), 0x48);
    return registry ? api_.lookup(registry, key) : nullptr;
}

OwnedController& OwnedController::Owner(void* data) {
    const auto* table = Read<const std::uintptr_t*>(data, 0);
    return *reinterpret_cast<OwnedController*>(table[49]);
}
float OwnedController::Radius(void* data) {
    const auto& b = Owner(data).bounds_;
    // A circumscribed XY radius includes both long/offset hull corners.
    return std::hypot(std::max(std::abs(b.min.x), std::abs(b.max.x)),
                      std::max(std::abs(b.min.y), std::abs(b.max.y)));
}
float OwnedController::Width(void* data) { const auto& b=Owner(data).bounds_; return b.max.x-b.min.x; }
float OwnedController::Length(void* data) { const auto& b=Owner(data).bounds_; return b.max.y-b.min.y; }
float OwnedController::Height(void* data) { const auto& b=Owner(data).bounds_; return b.max.z-b.min.z; }
void* OwnedController::GetBounds(void* data, void* out) {
    const auto& b = Owner(data).bounds_;
    std::memcpy(out, &b.min, sizeof(Vector));
    std::memcpy(static_cast<std::byte*>(out) + sizeof(Vector), &b.max, sizeof(Vector));
    return out;
}

bool OwnedController::Open(std::uint32_t formId, const PathingDataView& pathing, Bounds bounds) {
    Close();
    if (!api_.Complete() || !formId || !pathing.link || !pathing.link->object ||
        !bounds.Valid() || !api_.isNonActorReference(formId)) {
        status_ = ControllerStatus::Unavailable;
        return false;
    }
    bounds_ = bounds;
    api_.construct(component_.data(), nullptr, formId, &pathing);
    constructed_ = true;
    data_ = Read<void*>(component_.data(), 0x78);
    if (!data_ || !Read<void*>(component_.data(), 0x48) ||
        Method<std::uint32_t (*)(void*)>(data_, 0x10)(data_) != formId) {
        Close();
        status_ = ControllerStatus::Unavailable;
        return false;
    }
    originalDataTable_ = Read<std::uintptr_t>(data_, 0);
    std::memcpy(dataTable_.data(), reinterpret_cast<const void*>(originalDataTable_), 0x188);
    dataTable_[49] = reinterpret_cast<std::uintptr_t>(this);
    dataTable_[0x70/8] = reinterpret_cast<std::uintptr_t>(&Radius);
    dataTable_[0x78/8] = reinterpret_cast<std::uintptr_t>(&Width);
    dataTable_[0x80/8] = reinterpret_cast<std::uintptr_t>(&Length);
    dataTable_[0x88/8] = reinterpret_cast<std::uintptr_t>(&Height);
    dataTable_[0x90/8] = reinterpret_cast<std::uintptr_t>(&GetBounds);
    Write(data_, 0, dataTable_.data());

    // 142739530 registers internal-state control and selects the complete agent
    // inventory. 142738ef0 then selects its normal ground/flight configuration.
    api_.activate(component_.data());
    api_.selectNormal(component_.data());
    if (Read<std::uint8_t>(component_.data(), 0x5c) != 2 ||
        !Interface(api_.setGoalKey) || !Interface(api_.storedOutputKey)) {
        Close();
        status_ = ControllerStatus::Unavailable;
        return false;
    }
    formId_ = formId;
    status_ = ControllerStatus::Ready;
    return true;
}

bool OwnedController::Submit(void* request) {
    if (!constructed_ || !formId_) return false;
    Cancel();
    if (!request) return false;
    auto* setter = Interface(api_.setGoalKey);
    if (!setter) { status_ = ControllerStatus::Unavailable; return false; }
    const bool accepted = Method<bool (*)(void*, void**)>(setter, 8)(setter, &request);
    if (!accepted) {
        // A rejected enqueue can already have changed the arbiter's request.
        Method<void (*)(void*)>(setter, 0x20)(setter);
        status_ = ControllerStatus::Blocked;
        return false;
    }
    status_ = ControllerStatus::Waiting;
    return true;
}

bool OwnedController::SubmitDestination(Vector position, float stopDistance, bool flight) {
    if (!constructed_ || !formId_) return false;
    // A new destination invalidates the previous trip even when preparation
    // fails, so the previous goal can never keep steering after a rejected order.
    Cancel();
    if (!api_.makeRequest || !api_.releaseRequest || !Finite(position) ||
        !std::isfinite(stopDistance) || stopDistance <= 0) {
        status_ = ControllerStatus::Unavailable;
        return false;
    }
    void* request = api_.makeRequest(data_,position,stopDistance,flight);
    if (!request) { status_ = ControllerStatus::Unavailable; return false; }
    const bool accepted = Submit(request);
    api_.releaseRequest(request);
    return accepted;
}

ControllerFrame OwnedController::Update(float seconds, bool flight) {
    ControllerFrame frame{status_, generation_};
    if (status_ != ControllerStatus::Waiting && status_ != ControllerStatus::Following)
        return frame;
    if (!std::isfinite(seconds) || seconds <= 0 || seconds > 0.25f) return frame;
    auto* output = Interface(api_.storedOutputKey);
    if (!output) { status_ = frame.status = ControllerStatus::Unavailable; return frame; }
    // Registered getter = agent+0x20; speed = agent+0x40. A getter alone has
    // no freshness marker. Poison this private agent's speed before updating.
    Write(output, 0x20, std::numeric_limits<float>::quiet_NaN());
    Vector rotation{};
    Method<void (*)(void*, Vector*)>(data_, 0x30)(data_, &rotation);
    alignas(16) std::array<std::byte, 0x48> updateResult{};
    alignas(16) std::array<std::byte, 0x18> auxiliary{};
    const std::uint8_t updateFlags = 0;
    api_.update(component_.data(), seconds, &updateFlags, updateResult.data(), auxiliary.data(), false);

    auto* setter = Interface(api_.setGoalKey);
    if (!setter) { status_ = frame.status = ControllerStatus::Unavailable; return frame; }
    // 142707dc0/142707b40/1427091d0: arbiter+0x60 is 1 pending,
    // 2 path available, 3 complete; setter interface is arbiter+0x18.
    const auto state = Read<std::uint32_t>(setter, 0x48);
    if (state == 3) status_ = ControllerStatus::Arrived;
    else if (state == 0) status_ = ControllerStatus::Blocked;
    else if (state == 1) status_ = ControllerStatus::Waiting;
    else if (state == 2) status_ = ControllerStatus::Following;
    else status_ = ControllerStatus::Unavailable;
    frame.status = status_;
    if (status_ != ControllerStatus::Following || output != Interface(api_.storedOutputKey))
        return frame;
    PlannerSample sample;
    const auto* object = static_cast<const std::byte*>(output) - 0x20;
    if (ReadStoredOutput({object, sizeof(StoredPlannerOutput)}, sample))
        frame.motion = Decode(sample, rotation, flight);
    return frame;
}

void OwnedController::Cancel() {
    ++generation_;
    if (!constructed_) return;
    if (auto* setter = Interface(api_.setGoalKey))
        Method<void (*)(void*)>(setter, 0x20)(setter);
    status_ = formId_ ? ControllerStatus::Ready : ControllerStatus::Closed;
}

void OwnedController::Close() {
    Cancel();
    if (constructed_) {
        // Keep our bounds callbacks alive through cancellation; restore only
        // after that, before the engine releases its data and agent references.
        if (data_ && originalDataTable_) Write(data_, 0, originalDataTable_);
        api_.destroy(component_.data());
    }
    constructed_ = false;
    data_ = nullptr;
    originalDataTable_ = 0;
    formId_ = 0;
    component_.fill(std::byte{});
    status_ = ControllerStatus::Closed;
}
} // namespace native_movement
