#include "routeguidance.h"
#include "trafficprofile.h"
#include "trafficprofilestore.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
unsigned notices = 0;
namespace logger { void Write(const char*, ...) {} }
namespace notify { void Show(const char*, ...) { ++notices; } }
using trafficprofilestore::Write;
std::string Read(const fs::path& p) { std::ifstream s(p, std::ios::binary); return {std::istreambuf_iterator<char>(s), {}}; }
unsigned checks = 0;
void Check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
int main() {
    const auto sandbox = fs::temp_directory_path() / ("fvo-snapshot-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        const auto defaults = sandbox / "defaults", storage = sandbox / "storage", root = storage / "TrafficProfiles";
        fs::create_directories(defaults); fs::create_directories(storage);
        Write(defaults / "City.ini", "installed default");
        float saved = 0;
        trafficprofile::testing::Configure(storage.wstring(), defaults.wstring(), &saved, false);
        trafficprofile::Allow(true);
        Check(trafficprofile::Routes().empty() && saved == 0, "no snapshot support must not initialize traffic");
        trafficprofile::SaveReady(true); trafficprofile::Allow(true);
        const fs::path working = trafficprofile::Routes();
        Check(!working.empty() && saved > 0, "fresh profile creates a separate working copy");
        const auto baseline = std::uint32_t(saved);
        Check(working != trafficprofilestore::Open(root, baseline), "saved baseline must not be edited directly");
        Write(working / "journeys.state", "journey A");
        Write(working / "City.ini", "route A"); Write(working / "VehicleGroups.ini", "groups A");
        const std::string activeFleet="[TestWorld]\npair_fleet.1=FF100075 FF100079 FE004001 0\n";
        const std::string surplusFleet="[TestWorld]\npair_fleet.1=FF100075 FF100079 FE004001 0 1\n";
        Write(trafficprofile::File(L"trafficpool.ini"), activeFleet);
        trafficprofile::BeforeSave(); const auto saveA = std::uint32_t(saved); trafficprofile::AfterSave();
        Check(saveA != baseline, "save publishes a new snapshot identity");
        Check(fs::path(trafficprofile::Routes()) == working, "saving must not change active paths or reset traffic");
        Write(working / "journeys.state", "journey B");
        Write(working / "City.ini", "route B"); Write(working / "VehicleGroups.ini", "groups B");
        Write(trafficprofile::File(L"trafficpool.ini"), surplusFleet);
        trafficprofile::BeforeSave(); const auto saveB = std::uint32_t(saved); trafficprofile::AfterSave();
        Check(saveB != saveA, "later save gets a distinct snapshot");
        Check(Read(trafficprofilestore::Open(root, saveA) / "City.ini") == "route A", "later edits must not overwrite save A");
        Write(working / "City.ini", "unsaved later route");
        saved = float(saveA); trafficprofile::BeginLoad(); trafficprofile::Allow(true);
        const fs::path loadedA = trafficprofile::Routes();
        Check(Read(loadedA / "journeys.state") == "journey A", "saved journey state branches with its individual game save");
        Check(Read(loadedA / "City.ini") == "route A", "older save restores older routes");
        Check(Read(loadedA / "VehicleGroups.ini") == "groups A", "older save restores older groups");
        Check(Read(trafficprofile::File(L"trafficpool.ini")) == activeFleet, "older save keeps the pair active");
        Check(loadedA != working, "loading must create a fresh working copy");
        Write(loadedA / "City.ini", "branch C");
        trafficprofile::BeforeSave(); const auto saveC = std::uint32_t(saved); trafficprofile::AfterSave();
        Check(saveC != saveA && saveC != saveB, "branching older save creates a new identity");
        saved = float(saveB); trafficprofile::BeginLoad(); trafficprofile::Allow(true);
        const fs::path loadedB = trafficprofile::Routes();
        Check(Read(loadedB / "journeys.state") == "journey B", "later save retains its separate journey state");
        Check(Read(loadedB / "City.ini") == "route B", "loading B after branching A preserves B");
        Check(Read(loadedB / "VehicleGroups.ini") == "groups B", "branching cannot overwrite another save's groups");
        Check(Read(trafficprofile::File(L"trafficpool.ini")) == surplusFleet, "lower-target save restores surplus still waiting for cover");
        Write(trafficprofile::File(L"trafficpool.ini"), "[TestWorld]\nretiring=FF100075 FE004001 FF100079 001591EA\n");
        Write(loadedB / "City.ini", "unsaved B edit");
        trafficprofile::BeginLoad(); trafficprofile::Allow(true);
        const fs::path reloadedB = trafficprofile::Routes();
        Check(reloadedB != loadedB && Read(reloadedB / "City.ini") == "route B", "same-ID reload must discard later unsaved traffic edits");
        Check(Read(trafficprofile::File(L"trafficpool.ini")) == surplusFleet, "unsaved cleanup cannot overwrite saved fleet state");

        // The same lock used by the addon keeps a game save out of a partial config write.
        std::atomic<bool> writerReady{false}, releaseWriter{false}, saveStarted{false}, saveFinished{false};
        std::thread writer([&] {
            trafficprofile::Guard lock(trafficprofile::Mutex());
            Write(reloadedB / "City.ini", "atomic route"); writerReady = true;
            while (!releaseWriter) std::this_thread::yield();
            Write(reloadedB / "VehicleGroups.ini", "atomic groups");
        });
        while (!writerReady) std::this_thread::yield();
        std::thread saver([&] { saveStarted = true; trafficprofile::BeforeSave(); trafficprofile::AfterSave(); saveFinished = true; });
        while (!saveStarted) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const bool blocked = !saveFinished;
        releaseWriter = true; writer.join(); saver.join();
        Check(blocked, "snapshot must wait for the active file writer");
        const auto atomicSave = std::uint32_t(saved);
        const auto atomicRoutes = trafficprofilestore::Open(root, atomicSave);
        Check(Read(atomicRoutes / "City.ini") == "atomic route" && Read(atomicRoutes / "VehicleGroups.ini") == "atomic groups", "snapshot must not capture half a route/group update");

        routeguidance::beforeSave=+[](){return 0;};
        trafficprofile::BeforeSave();trafficprofile::AfterSave();
        Check(std::uint32_t(saved)==atomicSave,"failed journey recording cannot publish a new save identity");
        routeguidance::beforeSave=nullptr;
        // Prior v1.2 folders keep their contents and become read-only migration sources.
        Write(defaults / "Editor_legacy.ini", "legacy route");
        Write(defaults / "VehicleGroups.ini", "legacy groups");
        const auto legacy = trafficprofilestore::Create(root, defaults, storage, true, 444);
        saved = float(legacy.id); trafficprofile::BeginLoad(); trafficprofile::Allow(true);
        const fs::path migrated = trafficprofile::Routes();
        Check(Read(migrated / "Editor_legacy.ini") == "legacy route" && Read(migrated / "VehicleGroups.ini") == "legacy groups", "v1.2 saved profiles remain readable");
        Write(migrated / "Editor_legacy.ini", "migrated edit");
        trafficprofile::BeforeSave(); const auto migratedSave = std::uint32_t(saved); trafficprofile::AfterSave();
        Check(migratedSave != legacy.id && Read(legacy.routes / "Editor_legacy.ini") == "legacy route", "migration must preserve the original folder for older saves");
        saved = 0; trafficprofile::BeginLoad(); trafficprofile::Allow(true);
        const fs::path otherPlaythrough = trafficprofile::Routes();
        Check(Read(otherPlaythrough / "City.ini") == "installed default" && !fs::exists(otherPlaythrough / "VehicleGroups.ini"), "another playthrough starts fresh without inherited edits");

        // Failure leaves the last published identity and old snapshot intact.
        saved = float(atomicSave); trafficprofile::BeginLoad(); trafficprofile::Allow(true);
        const fs::path failing = trafficprofile::Routes();
        fs::remove(failing.parent_path() / "ready");
        const auto noticesBefore = notices;
        trafficprofile::BeforeSave(); trafficprofile::AfterSave();
        Check(std::uint32_t(saved) == atomicSave && Read(atomicRoutes / "City.ini") == "atomic route", "failed snapshot must retain the previous valid identity and files");
        Check(notices == noticesBefore, "worker/save callback must not invoke the main-thread HUD API");
        trafficprofile::Report();
        Check(notices == noticesBefore + 1, "main frame reports a snapshot failure");
        saved = float(0xFFFFFE); trafficprofile::BeginLoad(); trafficprofile::Allow(true);
        Check(trafficprofile::Routes().empty() && std::uint32_t(saved) == 0xFFFFFE, "missing snapshots must not reset a saved identity");
        trafficprofile::BeginLoad();
        // Unique, test-owned directory: no player files are touched.
        fs::remove_all(sandbox);
        std::cout << "traffic snapshots: " << checks << " checks, 0 failures\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << " (" << sandbox << ")\n"; return 1; }
}
