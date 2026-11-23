#pragma once

#include <charconv>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fleetownershipstore {
inline constexpr auto kFileName = L"traffic_owned.state";
inline constexpr auto kPlayerControlFileName = L"traffic_player_control.state";
inline constexpr std::size_t kMaxRecords = 64 * 1024;

// Cruise (3) is appended: a build that knows only 0..2 reads it as unknown, which protects the vehicle.
enum class Control { Unknown, Manual, Autopilot, Cruise };
inline constexpr unsigned long long kResumeWindowMs = 60000;  // RS-1: after the first play tick, wait this long for the player at the wheel
inline constexpr unsigned long long kResumeSettleMs = 1500;   // RS-1: seated at the wheel this long before Auto resumes
inline constexpr bool ResumesOnLoad(Control control) {
    return control == Control::Autopilot || control == Control::Cruise;
}
struct PlayerControl {
    unsigned vehicle = 0, base = 0;
    Control control = Control::Unknown;

    bool SavedAutopilot(unsigned currentVehicle, unsigned currentBase) const {
        return currentVehicle && currentBase && vehicle == currentVehicle &&
            base == currentBase && ResumesOnLoad(control);
    }
};

inline std::string WritePlayerControl(const PlayerControl& control) {
    std::ostringstream out;
    out << "FVO_PLAYER_CONTROL 1\n" << std::hex << control.vehicle << ' ' << control.base << ' '
        << static_cast<unsigned>(control.control) << '\n';
    return out.str();
}

inline PlayerControl ReadPlayerControl(std::istream& in) {
    std::string header;
    PlayerControl control;
    unsigned mode = 0;
    if (!std::getline(in, header) || header != "FVO_PLAYER_CONTROL 1" ||
        !(in >> std::hex >> control.vehicle >> control.base >> mode) ||
        mode > static_cast<unsigned>(Control::Cruise) || !(in >> std::ws).eof() ||
        !control.vehicle || !control.base) return {};
    control.control = static_cast<Control>(mode);
    return control;
}

inline std::string Write(const std::vector<unsigned>& vehicles) {
    if (vehicles.size() > kMaxRecords)
        throw std::runtime_error("Too many vehicle ownership records");
    std::ostringstream out;
    out << "FVO_TRAFFIC_OWNED 1\n" << std::hex;
    for (const auto vehicle : vehicles) {
        if (vehicle) out << vehicle << '\n';
    }
    return out.str();
}

inline std::vector<unsigned> Read(std::istream& in) {
    std::string line;
    if (!std::getline(in, line) || line != "FVO_TRAFFIC_OWNED 1")
        throw std::runtime_error("Invalid saved vehicle ownership header");

    std::vector<unsigned> vehicles;
    while (std::getline(in, line)) {
        unsigned vehicle = 0;
        const auto first = line.data();
        const auto last = first + line.size();
        const auto result = std::from_chars(first, last, vehicle, 16);
        if (result.ec != std::errc{} || result.ptr != last ||
            !vehicle || vehicles.size() == kMaxRecords)
            throw std::runtime_error("Invalid saved vehicle ownership record");
        vehicles.push_back(vehicle);
    }
    if (!in.eof()) throw std::runtime_error("Could not read saved vehicle ownership");
    return vehicles;
}
}
