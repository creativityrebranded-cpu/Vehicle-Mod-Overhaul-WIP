#pragma once

#include <charconv>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fleetownershipstore {
inline constexpr auto kFileName = L"traffic_owned.state";
inline constexpr std::size_t kMaxRecords = 64 * 1024;

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
