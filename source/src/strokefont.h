#pragma once
// A line-stroke font for the map route editor (2026-10-29: text fields in the star map movie drew boxes; the movie's
// $MAIN_Font_Bold holds only the glyphs of its own static text). Each glyph is polylines on a 4 x 6 grid (x right, y down),
// drawn with the same Graphics calls as the routes, so it always shows. Upper case only (lower case is drawn upper case,
// as the map's own labels are). Pure: tests/strokefont_test.cpp.
#include <cctype>
#include <string>
#include <vector>

namespace strokefont {
// Polylines as two-digit points "xy", separated by spaces; '|' starts a new polyline.
inline const char* Glyph(char c)
{
    switch (std::toupper(static_cast<unsigned char>(c))) {
    case 'A': return "06 02 20 42 46|03 43";
    case 'B': return "06 00 30 41 42 33 03|33 44 45 36 06";
    case 'C': return "41 30 10 01 05 16 36 45";
    case 'D': return "00 20 42 44 26 06 00";
    case 'E': return "40 00 06 46|03 33";
    case 'F': return "40 00 06|03 33";
    case 'G': return "41 30 10 01 05 16 36 45 43 23";
    case 'H': return "00 06|40 46|03 43";
    case 'I': return "10 30|20 26|16 36";
    case 'J': return "40 45 36 16 05";
    case 'K': return "00 06|40 03 46";
    case 'L': return "00 06 46";
    case 'M': return "06 00 23 40 46";
    case 'N': return "06 00 46 40";
    case 'O': return "10 30 41 45 36 16 05 01 10";
    case 'P': return "06 00 30 41 42 33 03";
    case 'Q': return "10 30 41 45 36 16 05 01 10|24 46";
    case 'R': return "06 00 30 41 42 33 03|23 46";
    case 'S': return "41 30 10 01 02 13 33 44 45 36 16 05";
    case 'T': return "00 40|20 26";
    case 'U': return "00 05 16 36 45 40";
    case 'V': return "00 26 40";
    case 'W': return "00 16 23 36 40";
    case 'X': return "00 46|40 06";
    case 'Y': return "00 23 40|23 26";
    case 'Z': return "00 40 06 46";
    case '0': return "10 30 41 45 36 16 05 01 10|05 41";
    case '1': return "11 20 26|16 36";
    case '2': return "01 10 30 41 42 06 46";
    case '3': return "01 10 30 41 42 33 13|33 44 45 36 16 05";
    case '4': return "30 04 44|30 36";
    case '5': return "40 00 03 33 44 45 36 06";
    case '6': return "41 30 10 01 05 16 36 45 44 33 03";
    case '7': return "00 40 16";
    case '8': return "13 02 01 10 30 41 42 33 13 04 05 16 36 45 44 33";
    case '9': return "43 13 02 01 10 30 41 45 36 06";
    case '.': return "25 26";
    case ',': return "25 16";
    case ':': return "21 22|25 26";
    case ';': return "21 22|25 16";
    case '-': return "13 33";
    case '+': return "13 33|22 24";
    case '/': return "06 40";
    case '(': return "30 21 25 36";
    case ')': return "10 21 25 16";
    case '[': return "30 20 26 36";
    case ']': return "10 20 26 16";
    case '<': return "31 03 35";
    case '>': return "11 33 15";
    case '=': return "02 42|04 44";
    case '%': return "06 40|00 01|45 46";
    case '\'': return "20 21";
    case '"': return "10 11|30 31";
    case '_': return "06 46";
    case '?': return "01 10 30 41 42 23 24|25 26";
    case '!': return "20 24|25 26";
    case '#': return "11 15|31 35|02 42|04 44";
    case '&': return "13 33|22 24";
    case '*': return "11 35|31 15|03 43";
    case '@': return "33 22 23 34 44 41 30 10 01 05 16 36";
    default: return "";  // space and anything unknown: a gap
    }
}
struct Point { double x, y; };
using Polyline = std::vector<Point>;
constexpr double kAdvance = 5.5;  // grid units per character
constexpr double kHeight = 6.0;
// The polylines of `text` with its top-left at (x, y), `unit` pixels per grid unit, at most `maxChars` characters.
inline std::vector<Polyline> Layout(const std::string& text, double x, double y, double unit, std::size_t maxChars = std::string::npos)
{
    std::vector<Polyline> out;
    std::size_t i = 0;
    for (const char c : text) {
        if (i >= maxChars) break;
        const char* g = Glyph(c);
        Polyline line;
        for (const char* p = g; *p;) {
            if (*p == '|') { if (!line.empty()) out.push_back(line); line.clear(); ++p; continue; }
            if (*p == ' ') { ++p; continue; }
            if (!p[1]) break;
            line.push_back({ x + (i * kAdvance + (p[0] - '0')) * unit, y + (p[1] - '0') * unit });
            p += 2;
        }
        if (!line.empty()) out.push_back(line);
        ++i;
    }
    return out;
}
inline std::size_t Fit(double width, double unit) { return unit > 0 ? std::size_t(width / (kAdvance * unit)) : 0; }
// Word wrap into lines of at most `chars` characters (a word longer than a line is cut).
inline std::vector<std::string> Wrap(const std::string& text, std::size_t chars)
{
    std::vector<std::string> lines;
    if (!chars) return lines;
    std::string line, word;
    auto flush = [&] {
        while (word.size() > chars) { if (!line.empty()) { lines.push_back(line); line.clear(); } lines.push_back(word.substr(0, chars)); word.erase(0, chars); }
        if (word.empty()) return;
        if (line.empty()) line = word;
        else if (line.size() + 1 + word.size() <= chars) line += " " + word;
        else { lines.push_back(line); line = word; }
        word.clear();
    };
    for (const char c : text) { if (c == ' ') flush(); else word.push_back(c); }
    flush();
    if (!line.empty()) lines.push_back(line);
    return lines;
}
}
