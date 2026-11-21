#pragma once
#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

// Authoring metadata is additive. Empty section rules inherit the route rule;
// legacy routes retain hover travel. A section belongs to its two endpoints.
namespace routepolicy {
enum class Travel : int { Inherit = -1, Both = 0, Drive = 1, Hover = 2 };
enum class Control : int { None, Yield, Stop, Signal };
struct Section {
    Travel travel = Travel::Inherit;
    double speed = 0;
    int priority = 0;
    Control control = Control::None;
    double stopSeconds = 1, signalPeriod = 60, signalGreen = 30, signalOffset = 0;
};
inline const char* Name(Travel m) {
    return m == Travel::Drive ? "Drive only" : m == Travel::Hover ? "Hover only" : m == Travel::Both ? "Both (prefer drive)" : "Inherit route";
}
inline const char* Token(Travel m) { return m == Travel::Drive ? "drive" : m == Travel::Hover ? "hover" : m == Travel::Both ? "both" : "inherit"; }
inline bool Parse(const std::string& s, Travel& m) {
    if (s == "drive") m = Travel::Drive;
    else if (s == "hover") m = Travel::Hover;
    else if (s == "both") m = Travel::Both;
    else if (s == "inherit") m = Travel::Inherit;
    else return false;
    return true;
}
inline bool Valid(const Section& s) {
    return int(s.travel) >= -1 && int(s.travel) <= 2 && int(s.control) >= 0 && int(s.control) <= 3 &&
        std::isfinite(s.speed) && s.speed >= 0 && s.speed <= 150 && s.priority >= -100 && s.priority <= 100 &&
        std::isfinite(s.stopSeconds) && s.stopSeconds >= 0 && s.stopSeconds <= 600 &&
        std::isfinite(s.signalPeriod) && s.signalPeriod >= 2 && s.signalPeriod <= 3600 &&
        std::isfinite(s.signalGreen) && s.signalGreen >= 0 && s.signalGreen <= s.signalPeriod && std::isfinite(s.signalOffset);
}
inline bool Read(const std::string& text, std::vector<Section>& out) {
    std::istringstream rows(text); std::string row;
    while (std::getline(rows, row, ';')) {
        if (row.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        std::replace(row.begin(), row.end(), ',', ' ');
        std::istringstream in(row); std::string mode, extra; Section s; int control = 0;
        if (!(in >> mode >> s.speed >> s.priority >> control >> s.stopSeconds >> s.signalPeriod >> s.signalGreen >> s.signalOffset) ||
            (in >> extra) || !Parse(mode, s.travel)) return false;
        s.control = Control(control); if (!Valid(s)) return false;
        out.push_back(s);
    }
    return true;
}
inline std::string Write(const std::vector<Section>& sections) {
    std::ostringstream out; out.precision(12);
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const auto& s = sections[i]; if (i) out << " | ";
        out << Token(s.travel) << ',' << s.speed << ',' << s.priority << ',' << int(s.control) << ',' << s.stopSeconds << ',' << s.signalPeriod << ',' << s.signalGreen << ',' << s.signalOffset;
    }
    return out.str();
}
// INI uses ';' for comments, so section entries are separated by '|'.
inline bool ReadIni(std::string text, std::vector<Section>& out) { std::replace(text.begin(), text.end(), '|', ';'); return Read(text, out); }
inline std::size_t Segment(std::size_t n, bool closed, double source, int lane) {
    if (n < 2) return 0;
    const double span = double(closed ? n : n - 1);
    source = std::clamp(source, 0.0, span);
    // On lane 1 the outgoing stretch at a vertex is the previous stretch.
    int i = int(std::floor(source - ((lane & 1) ? 1e-6 : 0)));
    if (closed) i = (i % int(n) + int(n)) % int(n);
    return std::size_t(std::clamp(i, 0, int(closed ? n - 1 : n - 2)));
}
inline Section Resolve(Travel route, const std::vector<Section>& sections, std::size_t i) {
    Section s = i < sections.size() ? sections[i] : Section{};
    if (s.travel == Travel::Inherit) s.travel = route;
    return s;
}
inline void Insert(std::vector<Section>& sections, std::size_t oldN, std::size_t at) {
    if (sections.empty()) return;
    sections.resize(oldN);
    const Section value = oldN ? sections[at ? std::min(at - 1, oldN - 1) : 0] : Section{};
    sections.insert(sections.begin() + std::ptrdiff_t(std::min(at, oldN)), value);
}
inline void Reverse(std::vector<Section>& sections, std::size_t n, bool closed) {
    if (sections.empty() || n < 2) return;
    sections.resize(n); const auto old = sections;
    for (std::size_t i = 0; i < n; ++i) sections[i] = old[(2 * n - 2 - i) % n];
    if (!closed) sections.back() = Section{};
}
}
