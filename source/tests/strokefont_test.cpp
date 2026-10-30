#include "strokefont.h"
#include <cstdio>
#include <cstring>

int main()
{
    using namespace strokefont;
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* name) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", name); } };
    // Every character the editor writes has a glyph, inside its 4 x 6 cell, with whole two-digit points.
    const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,:;-+/()[]<>=%'\"_?!#&*@";
    bool all = true, inside = true, wellFormed = true;
    for (const char* c = chars; *c; ++c) {
        const char* g = Glyph(*c);
        if (!*g) { all = false; std::printf("no glyph for %c\n", *c); }
        for (const char* p = g; *p;) {
            if (*p == '|' || *p == ' ') { ++p; continue; }
            if (!p[1] || p[1] == ' ' || p[1] == '|') { wellFormed = false; break; }
            if (p[0] < '0' || p[0] > '4' || p[1] < '0' || p[1] > '6') inside = false;
            p += 2;
        }
    }
    check(all, "every character has a glyph");
    check(inside, "glyphs stay in their cell");
    check(wellFormed, "points are two digits");
    check(!*Glyph(' ') && std::strcmp(Glyph('a'), Glyph('A')) == 0, "space is a gap; lower case draws upper case");
    const auto l = Layout("HI", 10, 20, 2);
    check(l.size() == 6 && l[0][0].x == 10 && l[0][0].y == 20 && l[3][0].x == 10 + 5.5 * 2 + 2, "layout advances per character");
    check(Layout("ABCDEF", 0, 0, 1, 2).size() == Layout("AB", 0, 0, 1).size(), "cut at the width");
    check(Fit(110, 2) == 10, "characters per width");
    const auto w = Wrap("the quick brown fox jumps", 10);
    check(w.size() == 3 && w[0] == "the quick" && w[1] == "brown fox" && w[2] == "jumps", "word wrap");
    check(Wrap("abcdefghijklmnop", 5).size() == 4, "long words are cut");
    std::printf("strokefont: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
