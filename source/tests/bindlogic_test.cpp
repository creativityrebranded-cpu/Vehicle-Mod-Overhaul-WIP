#include "../src/bindlogic.h"
#include <cstdio>
#include <set>
#include <string>

static int failures = 0;
static void check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; }

int main()
{
    using namespace bindlogic;
    std::set<int> down;
    auto key = [&](int vk) { return down.count(vk) > 0; };

    // Parsing
    std::string bad;
    auto b = Parse("T, Mouse4, Pad:A, Pad:LB+DUp", &bad);
    check(b.size() == 4 && bad.empty(), "a key, a mouse button, a pad button and a chord parse");
    check(b[0].vk == 'T' && b[1].vk == 0x05 && b[2].pad == kA && b[3].mod == kLB && b[3].pad == kDUp, "each alternative keeps its kind");
    check(Parse("t")[0].vk == 'T' && Parse("pad:rt")[0].pad == kRT && Parse("lshift")[0].vk == 0xA0, "names ignore case");
    check(Parse("F7")[0].vk == 0x76 && Parse("F24")[0].vk == 0x87 && Parse("Numpad5")[0].vk == 0x65, "function keys and the number pad");
    check(Parse(",")[0].vk == 0xBC && Parse("], ;").size() == 2, "punctuation keys, the comma key included");
    check(Parse("none").empty() && Parse("").empty() && Parse("  ").empty(), "none and empty lines bind nothing");
    bad.clear();
    auto partly = Parse("T, Banana, Pad:Q, Pad:XX+A", &bad);
    check(partly.size() == 1 && bad == "Banana, Pad:Q, Pad:XX+A", "unknown words are skipped and reported");
    check(Parse("default")[0].useDefault, "default");

    // Held: keys and the default key
    auto t = Parse("default, Pad:A");
    check(!Held(t, 'T', 0, 0, key), "nothing held");
    down = { 'T' };
    check(Held(t, 'T', 0, 0, key), "default = the module's key");
    check(!Held(Parse("Pad:A"), 'T', 0, 0, key), "a pad-only binding ignores the module's key");
    down.clear();
    check(Held(t, 'T', kA, 0, key), "the pad button");
    check(Held(Parse("default"), 0, kRT, 0, key, kRT), "default also = the game's controller button");
    check(!Held(Parse("default"), 0, kRT, 0, key, 0), "no game button, no pad");

    // Chords: a held modifier quiets plain pad bindings of other buttons
    auto plainA = Parse("Pad:A"), chord = Parse("Pad:LB+A");
    const std::uint32_t mods = Modifiers({ plainA, chord });
    check(mods == kLB, "chord modifiers collected");
    check(Held(plainA, 0, kA, mods, key) && !Held(chord, 0, kA, mods, key), "A alone: the plain binding only");
    check(!Held(plainA, 0, kA | kLB, mods, key) && Held(chord, 0, kA | kLB, mods, key), "LB+A: the chord only");
    check(Held(Parse("Pad:LB"), 0, kLB, mods, key), "the modifier itself can still be a plain binding");

    // XInput state -> bits
    check(PadBits(0x1000, 0, 0, 0, 0, 0, 0) == kA, "buttons pass through");
    check(PadBits(0, 200, 50, 0, 0, 0, 0) == kLT, "a trigger past its threshold");
    check(PadBits(0, 0, 0, -20000, 0, 0, 20000) == (kLLeft | kRUp), "stick directions past half way");
    check(PadBits(0, 0, 0, 8000, -8000, 0, 0) == 0, "small stick moves count as nothing");

    // The game's controller codes
    check(GamePadBit(0x1000) == kA && GamePadBit(9) == kLT && GamePadBit(10) == kRT, "the game's codes map to buttons");
    check(GamePadBit(0) == 0 && GamePadBit(0x3000) == 0 && GamePadBit(0x10000) == 0, "unknown codes map to nothing");

    // Labels
    check(Label(Parse("default, Pad:LB+DUp"), 'N') == "N / LB+DUp", "label lists the alternatives");
    check(Label(Parse("default, Pad:RT"), 'W', kRT) == "W / RT", "the game's button is named once");
    check(Label(Parse("none"), 'N') == "unbound" && Label(Parse("LShift"), 0) == "LShift", "unbound and named keys");

    std::printf("Bindings: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
