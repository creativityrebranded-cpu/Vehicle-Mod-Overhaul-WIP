#include "trafficprofilestore.h"
#include <chrono>
#include <iostream>

using namespace trafficprofilestore;
std::string Read(const fs::path& p) {
    std::ifstream stream(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main() {
    const auto sandbox = fs::temp_directory_path() / ("fvo-profile-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        const auto defaults = sandbox / "installed", legacy = sandbox / "legacy", profiles = sandbox / "profiles";
        fs::create_directories(defaults); fs::create_directories(legacy);
        Write(defaults / "NewAtlantis.ini", "installed defaults");
        Write(defaults / "Editor_city.ini", "old custom route");
        Write(defaults / "VehicleGroups.ini", "old custom groups");
        Write(defaults / "ignored.dll", "not route data");
        Write(legacy / "trafficpool.ini", "legacy vehicle IDs");
        Write(legacy / "ringroad.ini", "legacy ring");
        const auto a = Create(profiles, defaults, legacy, true, 123);
        Check(a.imported, "first existing playthrough should import legacy setup");
        Check(Read(a.routes / "Editor_city.ini") == "old custom route", "legacy routes preserved");
        Check(Read(a.routes / "VehicleGroups.ini") == "old custom groups", "legacy groups preserved");
        Check(Read(a.routes.parent_path() / "trafficpool.ini") == "legacy vehicle IDs", "legacy fleet preserved");
        Check(Read(a.routes.parent_path() / "ringroad.ini") == "legacy ring", "legacy ring preserved");
        Check(!fs::exists(a.routes / "ignored.dll"), "only traffic input files are copied");
        Write(a.routes / "NewAtlantis.ini", "playthrough A changed route");
        Write(a.routes / "VehicleGroups.ini", "playthrough A groups");
        const auto b = Create(profiles, defaults, legacy, false, 123);
        Check(a.id != b.id && !b.imported, "collisions allocate distinct identities");
        Check(Read(b.routes / "NewAtlantis.ini") == "installed defaults", "new playthrough starts fresh");
        Check(!fs::exists(b.routes / "Editor_city.ini") && !fs::exists(b.routes / "VehicleGroups.ini"), "no custom routes or groups leak");
        Check(!fs::exists(b.routes.parent_path() / "trafficpool.ini") && !fs::exists(b.routes.parent_path() / "ringroad.ini"), "no vehicle IDs or legacy rings leak");
        Write(b.routes / "VehicleGroups.ini", "playthrough B groups");
        Check(Read(Open(profiles, a.id) / "VehicleGroups.ini") == "playthrough A groups", "reloading A retains its groups");
        Check(Read(Open(profiles, b.id) / "VehicleGroups.ini") == "playthrough B groups", "reloading B retains its groups");
        Check(Read(Open(profiles, a.id) / "NewAtlantis.ini") == "playthrough A changed route", "same playthrough keeps route changes");
        Check(Read(defaults / "NewAtlantis.ini") == "installed defaults", "installed defaults untouched");
        const auto c = Create(profiles, defaults, legacy, true, 123);
        Check(!c.imported && !fs::exists(c.routes / "VehicleGroups.ini"), "another existing playthrough cannot re-import shared setup");
        bool refused = false; try { Open(profiles, 999999); } catch (...) { refused = true; }
        Check(refused && !fs::exists(Folder(profiles, 999999)), "missing saved profile is not silently reset");
        fs::remove(a.routes.parent_path() / "ready");
        refused = false; try { Open(profiles, a.id); } catch (...) { refused = true; }
        Check(refused, "partial profile cannot be loaded");
        refused = false; try { Folder(profiles, 0); } catch (...) { refused = true; }
        Check(refused, "zero is not a persistent identity");
        // Everything removed is under the unique test-owned directory above.
        fs::remove_all(sandbox);
        std::cout << "PASS playthrough isolation, reload, legacy migration, collisions, missing/partial profiles\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << " (test files: " << sandbox << ")\n";
        return 1;
    }
}
