#pragma once
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The binding toolkit's pure part (bindings.cpp does the input): parsing a binding line, telling whether it is held, and
// naming it. A line lists alternatives separated by ',' and any one held counts:
//   T                 a key (letters, digits, F1-F24, Shift / LShift / RShift, Ctrl, Alt, Space, Tab, Enter, Backspace,
//                     Insert, Delete, Home, End, PageUp, PageDown, Up / Down / Left / Right, Numpad0-9, NumpadPlus,
//                     NumpadMinus, NumpadStar, NumpadSlash, NumpadDot, and ; = , - . / ` [ \ ] ')
//   Mouse4            a mouse button (Mouse1 left, Mouse2 right, Mouse3 middle, Mouse4 / Mouse5 side)
//   Pad:A             a controller button: A B X Y LB RB LT RT LS RS (stick clicks) Start View DUp DDown DLeft DRight,
//                     LStickUp / LStickDown / LStickLeft / LStickRight, RStickUp / RStickDown / RStickLeft / RStickRight
//   Pad:LB+DUp        a chord: hold the first, press the second. While any chord modifier is held, plain pad bindings of
//                     other buttons stay quiet, so LB+A never also fires a plain A binding.
//   default           the action's own key (the one the mod picks, or the game's binding it follows)
//   none              nothing (an empty line too)
// Unknown words are skipped and reported (Parse's `bad`).
namespace bindlogic {
// Controller bits: the low 16 are XInput's button mask; the rest are the triggers and stick directions past a threshold.
enum : std::uint32_t {
    kDUp = 0x0001, kDDown = 0x0002, kDLeft = 0x0004, kDRight = 0x0008, kStart = 0x0010, kView = 0x0020, kLS = 0x0040, kRS = 0x0080,
    kLB = 0x0100, kRB = 0x0200, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000,
    kLT = 1u << 16, kRT = 1u << 17,
    kLUp = 1u << 18, kLDown = 1u << 19, kLLeft = 1u << 20, kLRight = 1u << 21,
    kRUp = 1u << 22, kRDown = 1u << 23, kRLeft = 1u << 24, kRRight = 1u << 25,
};
inline constexpr std::uint8_t kTriggerOn = 100;   // of 255
inline constexpr int kStickOn = 16000;            // of 32767 (about half way)

// XInput's raw state -> the bit set above.
inline std::uint32_t PadBits(std::uint16_t buttons, std::uint8_t lt, std::uint8_t rt, int lx, int ly, int rx, int ry)
{
    std::uint32_t b = buttons;
    if (lt >= kTriggerOn) b |= kLT;
    if (rt >= kTriggerOn) b |= kRT;
    if (ly >= kStickOn) b |= kLUp;
    if (ly <= -kStickOn) b |= kLDown;
    if (lx <= -kStickOn) b |= kLLeft;
    if (lx >= kStickOn) b |= kLRight;
    if (ry >= kStickOn) b |= kRUp;
    if (ry <= -kStickOn) b |= kRDown;
    if (rx <= -kStickOn) b |= kRLeft;
    if (rx >= kStickOn) b |= kRRight;
    return b;
}

// The game's own gamepad code (ControlMap) -> a controller bit: XInput's single-bit masks as they are, 9 / 10 the triggers
// (the code space Bethesda's earlier games use; unknown codes give 0).
inline std::uint32_t GamePadBit(std::uint32_t code)
{
    if (code == 9) return kLT;
    if (code == 10) return kRT;
    if (code && code <= 0x8000 && (code & (code - 1)) == 0 && (code & 0xF3FF)) return code;
    return 0;
}

struct Alt {
    int vk = 0;               // keyboard / mouse virtual key; 0 = none
    std::uint32_t pad = 0;    // controller bit
    std::uint32_t mod = 0;    // controller chord modifier (held), 0 = a plain button
    bool useDefault = false;  // "default"
};
using Binding = std::vector<Alt>;

struct Name { const char* name; int vk; };
inline const std::vector<Name>& KeyNames()
{
    static const std::vector<Name> t = [] {
        std::vector<Name> v = {
            { "LShift", 0xA0 }, { "RShift", 0xA1 }, { "Shift", 0x10 }, { "LCtrl", 0xA2 }, { "RCtrl", 0xA3 }, { "Ctrl", 0x11 },
            { "LAlt", 0xA4 }, { "RAlt", 0xA5 }, { "Alt", 0x12 }, { "Space", 0x20 }, { "Tab", 0x09 }, { "Enter", 0x0D }, { "Backspace", 0x08 },
            { "Insert", 0x2D }, { "Delete", 0x2E }, { "Home", 0x24 }, { "End", 0x23 }, { "PageUp", 0x21 }, { "PageDown", 0x22 },
            { "Up", 0x26 }, { "Down", 0x28 }, { "Left", 0x25 }, { "Right", 0x27 }, { "CapsLock", 0x14 },
            { "NumpadPlus", 0x6B }, { "NumpadMinus", 0x6D }, { "NumpadStar", 0x6A }, { "NumpadSlash", 0x6F }, { "NumpadDot", 0x6E },
            { ";", 0xBA }, { "=", 0xBB }, { ",", 0xBC }, { "-", 0xBD }, { ".", 0xBE }, { "/", 0xBF }, { "`", 0xC0 }, { "[", 0xDB },
            { "\\", 0xDC }, { "]", 0xDD }, { "'", 0xDE },
            { "Mouse1", 0x01 }, { "Mouse2", 0x02 }, { "Mouse3", 0x04 }, { "Mouse4", 0x05 }, { "Mouse5", 0x06 },
        };
        static const char* const kF[24] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
            "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24" };
        for (int i = 0; i < 24; ++i) v.push_back({ kF[i], 0x70 + i });
        static const char* const kN[10] = { "Numpad0", "Numpad1", "Numpad2", "Numpad3", "Numpad4", "Numpad5", "Numpad6", "Numpad7", "Numpad8", "Numpad9" };
        for (int i = 0; i < 10; ++i) v.push_back({ kN[i], 0x60 + i });
        return v;
    }();
    return t;
}
struct PadName { const char* name; std::uint32_t bit; };
inline const std::vector<PadName>& PadNames()
{
    static const std::vector<PadName> t = {
        { "A", kA }, { "B", kB }, { "X", kX }, { "Y", kY }, { "LB", kLB }, { "RB", kRB }, { "LT", kLT }, { "RT", kRT }, { "LS", kLS }, { "RS", kRS },
        { "Start", kStart }, { "View", kView }, { "Back", kView }, { "DUp", kDUp }, { "DDown", kDDown }, { "DLeft", kDLeft }, { "DRight", kDRight },
        { "LStickUp", kLUp }, { "LStickDown", kLDown }, { "LStickLeft", kLLeft }, { "LStickRight", kLRight },
        { "RStickUp", kRUp }, { "RStickDown", kRDown }, { "RStickLeft", kRLeft }, { "RStickRight", kRRight },
    };
    return t;
}

inline bool Same(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
inline std::string_view Trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}
inline int KeyCode(std::string_view w)
{
    if (w.size() == 1 && std::isalnum(static_cast<unsigned char>(w[0]))) return std::toupper(static_cast<unsigned char>(w[0]));
    for (const auto& n : KeyNames()) if (Same(w, n.name)) return n.vk;
    return 0;
}
inline std::uint32_t PadCode(std::string_view w)
{
    for (const auto& n : PadNames()) if (Same(w, n.name)) return n.bit;
    return 0;
}

// One line -> its alternatives. `bad` collects the words that meant nothing.
inline Binding Parse(std::string_view line, std::string* bad = nullptr)
{
    Binding out;
    while (!line.empty()) {
        const auto comma = line.find(',', 1);  // a lone ',' is the comma key
        std::string_view w = Trim(line.substr(0, comma));
        line = comma == std::string_view::npos ? std::string_view{} : line.substr(comma + 1);
        if (w.empty() || Same(w, "none")) continue;
        if (Same(w, "default")) { out.push_back({ 0, 0, 0, true }); continue; }
        if (w.size() > 4 && Same(w.substr(0, 4), "pad:")) {
            std::string_view b = w.substr(4);
            const auto plus = b.find('+');
            const std::uint32_t mod = plus == std::string_view::npos ? 0 : PadCode(Trim(b.substr(0, plus)));
            const std::uint32_t pad = PadCode(Trim(plus == std::string_view::npos ? b : b.substr(plus + 1)));
            if (pad && (plus == std::string_view::npos || mod)) { out.push_back({ 0, pad, mod, false }); continue; }
        }
        else if (const int vk = KeyCode(w)) { out.push_back({ vk, 0, 0, false }); continue; }
        if (bad) { if (!bad->empty()) *bad += ", "; *bad += std::string(w); }
    }
    return out;
}

// Every chord modifier of these bindings (a plain pad binding is quiet while one of them is held).
inline std::uint32_t Modifiers(const std::vector<Binding>& all)
{
    std::uint32_t m = 0;
    for (const auto& b : all) for (const auto& a : b) m |= a.mod;
    return m;
}

// Held: any alternative. keyHeld(vk) answers for keys and mouse buttons; pad is the controller's bit set now.
// "default" is the action's own key (defaultVk) and, when the game binds one, its controller button (defaultPad).
template <class KeyHeld>
bool Held(const Binding& b, int defaultVk, std::uint32_t pad, std::uint32_t modifiers, KeyHeld keyHeld, std::uint32_t defaultPad = 0)
{
    for (const auto& a : b) {
        if (a.useDefault) {
            if (defaultVk && keyHeld(defaultVk)) return true;
            if (defaultPad && (pad & defaultPad) && !(pad & modifiers & ~defaultPad)) return true;
            continue;
        }
        if (a.vk) { if (keyHeld(a.vk)) return true; continue; }
        if (!a.pad || !(pad & a.pad)) continue;
        if (a.mod) { if (pad & a.mod) return true; continue; }
        if (!(pad & modifiers & ~a.pad)) return true;  // plain: only while no chord modifier is held
    }
    return false;
}

inline std::string KeyLabel(int vk)
{
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, char(vk));
    for (const auto& n : KeyNames()) if (n.vk == vk) return n.name;
    return "key " + std::to_string(vk);
}
inline std::string PadLabel(std::uint32_t bit)
{
    for (const auto& n : PadNames()) if (n.bit == bit) return n.name;
    return "?";
}
// "T / LB+DUp" (what the HUD and notices show); `defaultVk` / `defaultPad` name "default". A name already listed is not repeated.
inline std::string Label(const Binding& b, int defaultVk, std::uint32_t defaultPad = 0)
{
    std::string s;
    auto add = [&](const std::string& t) {
        if (t.empty()) return;
        for (std::size_t at = 0; (at = s.find(t, at)) != std::string::npos; at += t.size())
            if ((at == 0 || s.compare(at - 3, 3, " / ") == 0) && (at + t.size() == s.size() || s.compare(at + t.size(), 3, " / ") == 0)) return;
        if (!s.empty()) s += " / ";
        s += t;
    };
    for (const auto& a : b) {
        if (a.useDefault) { add(defaultVk ? KeyLabel(defaultVk) : std::string()); add(defaultPad ? PadLabel(defaultPad) : std::string()); }
        else if (a.vk) add(KeyLabel(a.vk));
        else if (a.pad) add(a.mod ? PadLabel(a.mod) + "+" + PadLabel(a.pad) : PadLabel(a.pad));
    }
    return s.empty() ? "unbound" : s;
}
}
