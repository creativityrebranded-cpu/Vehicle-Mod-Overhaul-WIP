#pragma once

#include "../trafficlinks.h"
#include "../fvo_addon_api.h"

#include <charconv>
#include <cstdint>
#include <numbers>
#include <string_view>

namespace factorysite {
namespace rules {
inline constexpr std::size_t maxManifestBytes = 64 * 1024;
inline constexpr double bayDistance = 2.0;
inline constexpr double minimumRun = 4.0;
inline constexpr double joinDistance = 2.0;
inline constexpr double joinHeight = 0.5;
}

struct Config {
    std::string key, world;
    unsigned vehicle = 0;
    std::string privateRoute, publicRoute, link;
    int privateLane = -1, publicLane = -1;
    FvoPoolSpot pose{};
};

struct ParseResult {
    Config config{};
    bool enabled = false;
    const char* reason = "manifest_missing";
    bool recognized = false;
};

inline bool Name(const std::string& name) {
    return !name.empty() && name.size() <= 200 &&
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_.-") == std::string::npos;
}

inline ParseResult ReadManifest(std::string_view text) {
    if (text.size() > rules::maxManifestBytes) return {{}, false, "manifest_too_large"};
    std::istringstream input{std::string(text)};
    std::map<std::string, std::string> fields;
    std::set<std::string> sections;
    std::string line, section, factory;
    bool fileHasEnabled = false, legacyDisabled = false;
    while (std::getline(input, line)) {
        line = trafficdata::Trim(line.substr(0, line.find(';')));
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() != ']') return {{}, false, "malformed_section"};
            section = trafficdata::Lower(trafficdata::Trim(line.substr(1, line.size() - 2)));
            if (!sections.insert(section).second) return {{}, false, "duplicate_section"};
            if (section.starts_with("factory.")) {
                if (!factory.empty()) return {{}, false, "multiple_factory_bays"};
                factory = section;
            } else if (section != "file" && !section.starts_with("route.") && !section.starts_with("link.")) {
                return {{}, false, "unexpected_manifest_section"};
            }
            continue;
        }
        const auto equals = line.find('=');
        if (section.empty() || equals == std::string::npos) return {{}, false, "malformed_field"};
        const auto key = trafficdata::Lower(trafficdata::Trim(line.substr(0, equals)));
        const auto value = trafficdata::Trim(line.substr(equals + 1));
        if (section.starts_with("factory.") && !fields.emplace(key, value).second)
            return {{}, false, "duplicate_factory_field"};
        if (section == "file" && key == "enabled") {
            if (fileHasEnabled) return {{}, false, "duplicate_file_enabled"};
            fileHasEnabled = true;
            legacyDisabled = value == "0";
        }
    }
    if (factory.empty()) return {{}, false, "factory_section_missing"};
    if (!legacyDisabled) return {{}, false, "legacy_file_must_be_disabled"};
    if (fields["enabled"] == "0") return {{}, false, "factory_disabled"};
    if (fields["enabled"] != "1") return {{}, false, "invalid_factory_enabled"};
    for (const auto& [field, value] : fields) {
        (void)value;
        if (field != "enabled" && field != "world" && field != "vehicle" && field != "pose" &&
            field != "private_route" && field != "private_lane" && field != "public_route" &&
            field != "public_lane" && field != "link")
            return {{}, false, "unknown_factory_field"};
    }

    Config bay;
    bay.key = factory.substr(8);
    bay.world = trafficdata::Lower(fields["world"]);
    bay.privateRoute = trafficdata::Lower(fields["private_route"]);
    bay.publicRoute = trafficdata::Lower(fields["public_route"]);
    bay.link = trafficdata::Lower(fields["link"]);
    if (!Name(bay.key) || !Name(bay.world) || !Name(bay.privateRoute) || !Name(bay.publicRoute) || !Name(bay.link))
        return {{}, false, "invalid_factory_name"};
    if (bay.privateRoute == bay.publicRoute) return {{}, false, "factory_route_must_be_private"};
    if (sections.size() != 4 || !sections.contains("route." + bay.privateRoute) || !sections.contains("link." + bay.link))
        return {{}, false, "manifest_requires_only_private_route_and_exit"};

    const auto& form = fields["vehicle"];
    const auto parsedForm = std::from_chars(form.data(), form.data() + form.size(), bay.vehicle, 16);
    if (form.size() != 8 || parsedForm.ec != std::errc{} || parsedForm.ptr != form.data() + form.size() ||
        !bay.vehicle || (bay.vehicle & 0xff000000u) == 0xff000000u)
        return {{}, false, "invalid_placed_vehicle_id"};
    bay.vehicle = trafficdata::MapId(bay.vehicle);
    if (!bay.vehicle || (bay.vehicle & 0xff000000u) == 0xff000000u)
        return {{}, false, "placed_vehicle_mapping_invalid"};

    const auto lane = [&](const char* key, int& index) {
        const auto& value = fields[key];
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), index);
        return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && index >= 0;
    };
    if (!lane("private_lane", bay.privateLane) || !lane("public_lane", bay.publicLane))
        return {{}, false, "invalid_lane_number"};
    bool poseValid = false;
    const auto pose = trafficdata::Numbers(fields["pose"], poseValid);
    if (!poseValid || pose.size() != 4 || std::abs(pose[3]) > 2.0 * std::numbers::pi)
        return {{}, false, "invalid_pose_xyz_heading_radians"};
    bay.pose = {pose[0], pose[1], pose[2], pose[3]};
    return {std::move(bay), true, "authored_factory_bay"};
}

inline ParseResult Parse(std::string_view text) {
    if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    std::string_view rest = text;
    while (!rest.empty()) {
        const auto newline = rest.find('\n');
        auto line = rest.substr(0, newline);
        line = line.substr(0, line.find(';'));
        const auto lhs = line.find_first_not_of(" \t\r");
        if (lhs != std::string_view::npos && line[lhs] == '[') {
            line.remove_prefix(lhs + 1);
            const auto section = trafficdata::Lower(trafficdata::Trim(std::string(line.substr(0, line.find(']')))));
            if (section.starts_with("factory.")) {
                auto result = ReadManifest(text);
                result.recognized = true;
                return result;
            }
        }
        if (newline == std::string_view::npos) break;
        rest.remove_prefix(newline + 1);
    }
    return {};
}

inline bool WheelStation(const trafficdata::Route& route, const trafficroute::Line& line, int lane, double u) {
    if (!trafficroute::Available(line, lane, u)) return false;
    const auto at = routepolicy::Segment(route.points.size(), line.closed, line.ToSource(u), lane);
    if (routepolicy::Resolve(route.travel, route.sections, at).travel == routepolicy::Travel::Hover) return false;
    return !route.road.enabled || roadplan::PrimitiveAt(route.road, at).kind != roadplan::Open;
}

// Authored geometry only. Handoff and Siting still require a current reference,
// physical support, concealment, and a clear destination from their live owners.
inline const char* Validate(const Config& bay, const trafficdata::Data& data, const std::string& gameWorld) {
    if (gameWorld.empty() || !trafficdata::SameWorld(data, bay.world, gameWorld)) return "wrong_factory_world";
    const trafficdata::Route* privateRoute = nullptr;
    const trafficdata::Route* publicRoute = nullptr;
    for (const auto& route : data.routes) {
        if (route.removed || route.world.empty() || !trafficdata::SameWorld(data, route.world, gameWorld)) continue;
        if (route.name == bay.privateRoute) {
            if (privateRoute) return "ambiguous_private_route";
            privateRoute = &route;
        }
        if (route.name == bay.publicRoute) {
            if (publicRoute) return "ambiguous_public_route";
            publicRoute = &route;
        }
    }
    if (!privateRoute || !publicRoute) return "factory_route_missing";
    if (privateRoute->ring || privateRoute->closed || privateRoute->travel != routepolicy::Travel::Drive)
        return "factory_requires_open_drive_route";
    if (std::any_of(privateRoute->sections.begin(), privateRoute->sections.end(), [](const auto& s) {
        return s.travel != routepolicy::Travel::Drive && s.travel != routepolicy::Travel::Inherit;
    })) return "private_route_section_not_drive";
    if (privateRoute->road.enabled && std::any_of(privateRoute->road.primitives.begin(), privateRoute->road.primitives.end(),
        [](const auto& tile) { return tile.kind == roadplan::Open; })) return "private_route_has_open_air_tile";

    const auto privateLine = trafficroute::Build(*privateRoute);
    const auto publicLine = trafficroute::Build(*publicRoute);
    if (!trafficroute::HasLane(privateLine, privateRoute->lanes, bay.privateLane) ||
        !trafficroute::HasLane(publicLine, publicRoute->lanes, bay.publicLane))
        return "factory_lane_missing";

    const trafficdata::Link* connection = nullptr;
    for (const auto& link : data.links) {
        if (link.name != bay.link) continue;
        if (connection) return "ambiguous_factory_exit";
        connection = &link;
    }
    if (!connection) return "factory_exit_missing";
    const auto& link = *connection;
    const auto validStation = [](const trafficdata::Route& route, double at) {
        return std::isfinite(at) && at >= 0.0 &&
            at <= (route.ring ? 360.0 : double(route.points.size() - (route.closed ? 0 : 1)));
    };
    const bool outbound = link.type == "exit" && link.from == bay.privateRoute && link.to == bay.publicRoute;
    const bool merge = link.type == "merge" && link.to == bay.privateRoute && link.from == bay.publicRoute;
    if ((!outbound && !merge) ||
        (outbound && (link.fromLane != bay.privateLane || link.toLane != bay.publicLane)) ||
        (merge && (link.toLane != bay.privateLane || link.fromLane != bay.publicLane)))
        return "factory_requires_explicit_directed_exit";
    const auto& fromRoute = outbound ? *privateRoute : *publicRoute;
    const auto& toRoute = outbound ? *publicRoute : *privateRoute;
    if (!validStation(fromRoute, link.at) || (!link.toEnd && !validStation(toRoute, link.toAt)))
        return "factory_exit_station_out_of_range";
    const double fromU = trafficlinks::U(fromRoute, link.at);
    const double toU = link.toEnd ? trafficlinks::EndU(toRoute, true) : trafficlinks::U(toRoute, link.toAt);
    const double exitU = outbound ? fromU : toU;
    const double arrivalU = outbound ? toU : fromU;
    const double bayU = trafficroute::NearestProgress(privateLine, bay.privateLane, bay.pose.x, bay.pose.y, bay.pose.z, false, 0.0);
    const auto atBay = trafficroute::At(privateLine, bay.privateLane, bayU);
    if (std::hypot(atBay.x - bay.pose.x, atBay.y - bay.pose.y, atBay.z - bay.pose.z) > rules::bayDistance)
        return "factory_pose_outside_private_lane";
    const double distance = trafficroute::DistanceAlong(privateLine, bay.privateLane, bayU, exitU);
    if (!std::isfinite(distance) || distance < rules::minimumRun ||
        !trafficroute::ConnectedAlong(privateLine, bay.privateLane, bayU, exitU))
        return "factory_exit_unreachable_on_private_lane";
    if (!WheelStation(*privateRoute, privateLine, bay.privateLane, bayU) ||
        !WheelStation(*privateRoute, privateLine, bay.privateLane, exitU) ||
        !WheelStation(*publicRoute, publicLine, bay.publicLane, arrivalU))
        return "factory_exit_requires_wheel_surface";
    const auto from = trafficroute::At(privateLine, bay.privateLane, exitU);
    const auto to = trafficroute::At(publicLine, bay.publicLane, arrivalU);
    if (std::abs(from.z - to.z) > rules::joinHeight ||
        std::hypot(from.x - to.x, from.y - to.y) > rules::joinDistance)
        return "factory_exit_geometry_disconnected";
    const auto turn = trafficlinks::TurnPath(privateLine, bay.privateLane, exitU, publicLine, bay.publicLane, arrivalU, link.radius);
    if (!turn.empty()) {
        const auto& end = turn.back();
        const double endU = trafficroute::NearestProgress(publicLine, bay.publicLane, end.x, end.y, end.z, true, arrivalU);
        if (!WheelStation(*publicRoute, publicLine, bay.publicLane, endU))
            return "factory_arrival_requires_wheel_surface";
    }
    return nullptr;
}
}
