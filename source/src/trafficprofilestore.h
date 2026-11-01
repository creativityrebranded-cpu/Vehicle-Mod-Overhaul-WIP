#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

// A save holds a 24-bit profile identity; files are mutable within that playthrough.
// Reserve identities with an exclusive directory create; never recycle an old profile.
namespace trafficprofilestore {
namespace fs = std::filesystem;
inline constexpr std::uint32_t kMaxId = 0xFFFFFF;
inline std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){return char(std::tolower(c));});
    return s;
}
inline fs::path Folder(const fs::path& root, std::uint32_t id) {
    if (!id || id > kMaxId) throw std::runtime_error("Invalid traffic profile identity");
    return root / std::to_string(id);
}
inline fs::path Open(const fs::path& root, std::uint32_t id) {
    const auto folder = Folder(root, id);
    if (!fs::is_regular_file(folder / "ready") || !fs::is_directory(folder / "routes"))
        throw std::runtime_error("This save's traffic profile is missing or incomplete");
    return folder / "routes";
}
inline void Write(const fs::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
    stream.close();
    if (!stream) throw std::runtime_error("Could not write traffic profile");
}
struct Created { std::uint32_t id; fs::path routes; bool imported; };
inline Created Create(const fs::path& root, const fs::path& defaults, const fs::path& legacy,
                      bool established, std::uint32_t seed) {
    if (!fs::is_directory(defaults)) throw std::runtime_error("Traffic Addon defaults are missing");
    fs::create_directories(root);
    auto id = seed % kMaxId + 1;
    fs::path folder;
    bool reserved = false;
    for (std::uint32_t count = 0; count < kMaxId; ++count) {
        folder = Folder(root, id);
        if (fs::create_directory(folder)) { reserved = true; break; }
        id = id == kMaxId ? 1 : id + 1;
    }
    if (!reserved) throw std::runtime_error("No unused traffic profile identity");
    const bool imported = established && fs::create_directory(root / "legacy-imported");
    try {
        const auto routes = folder / "routes";
        fs::create_directory(routes);
        for (const auto& file : fs::directory_iterator(defaults)) {
            if (!file.is_regular_file()) continue;
            const auto name = Lower(file.path().filename().string());
            const auto extension = Lower(file.path().extension().string());
            if (extension != ".ini" && extension != ".txt") continue;
            if (!imported && (name.starts_with("editor_") || name == "vehiclegroups.ini")) continue;
            fs::copy_file(file.path(), routes / file.path().filename());
        }
        if (imported) {
            for (const auto* name : {"trafficpool.ini", "ringroad.ini"})
                if (fs::is_regular_file(legacy / name)) fs::copy_file(legacy / name, folder / name);
        }
        Write(folder / "ready", "Traffic profile v1\n");
        if (imported) Write(root / "legacy-imported" / "owner.txt", std::to_string(id));
        return {id, routes, imported};
    } catch (...) {
        // Leave an incomplete profile reserved; never attach a save to partial files.
        if (imported && fs::is_empty(root / "legacy-imported")) fs::remove(root / "legacy-imported");
        throw;
    }
}
}
