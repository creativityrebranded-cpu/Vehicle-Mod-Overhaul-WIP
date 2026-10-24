#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
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
inline constexpr std::uint32_t kPadPressBits = 0xFFFFu | kLT | kRT;  // buttons and triggers, never the stick directions

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

// Where an alternative counts (2026-10-24, the situation-based controller map): "Pad:DUp@vehicle". The prompt and the
// hold come from the one situation the player is in now; an alternative without '@' counts everywhere.
enum : std::uint8_t {
    kCtxFoot = 1, kCtxGround = 2, kCtxHover = 4, kCtxVtol = 8, kCtxScanner = 16, kCtxPassenger = 32,
    kCtxFlying = kCtxHover | kCtxVtol, kCtxVehicle = kCtxGround | kCtxFlying, kCtxAll = 0xFF,
};
struct CtxName { const char* name; std::uint8_t ctx; };
inline const std::vector<CtxName>& CtxNames()
{
    static const std::vector<CtxName> t = { { "foot", kCtxFoot }, { "ground", kCtxGround }, { "hover", kCtxHover }, { "vtol", kCtxVtol },
        { "scanner", kCtxScanner }, { "passenger", kCtxPassenger }, { "flying", kCtxFlying }, { "vehicle", kCtxVehicle } };
    return t;
}
// "Pad:DLeft:tap" counts once, on a release before kGestureHoldMs; "Pad:DLeft:hold" from kGestureHoldMs on. Two actions
// can share a button that way (tap one, hold the other) without either firing for the other's gesture.
enum class Gesture : std::uint8_t { Raw, Tap, Hold };
inline constexpr std::uint64_t kGestureHoldMs = 500;

struct Alt {
    int vk = 0;               // keyboard / mouse virtual key; 0 = none
    std::uint32_t pad = 0;    // controller bit
    std::uint32_t mod = 0;    // controller chord modifier (held), 0 = a plain button
    bool useDefault = false;  // "default"
    bool defaultKeyboard = true, defaultController = true;
    std::uint8_t ctx = 0;     // situations it counts in (kCtx*), 0 = every one
    Gesture gesture = Gesture::Raw;
};
inline bool InCtx(const Alt& a, std::uint8_t ctx) { return !a.ctx || (a.ctx & ctx); }

// One tap / hold alternative's state (bindings.cpp keeps one per alternative and updates it once a frame).
struct GestureState
{
    bool down = false, held = false;
    std::uint64_t at = 0;
    // `on` = the button (and its chord modifier) held now. True for one update after a short press (Tap), or while held
    // past kGestureHoldMs (Hold).
    bool Update(bool on, Gesture g, std::uint64_t now)
    {
        bool out = false;
        if (on) {
            if (!down) { down = true; held = false; at = now; }
            if (now - at >= kGestureHoldMs) held = true;
            out = g == Gesture::Hold && held;
        }
        else {
            out = g == Gesture::Tap && down && !held;
            down = held = false;
        }
        return out;
    }
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
        if (Same(w, "default-key")) { out.push_back({ 0, 0, 0, true, true, false }); continue; }
        if (Same(w, "default-pad")) { out.push_back({ 0, 0, 0, true, false, true }); continue; }
        if (w.size() > 4 && Same(w.substr(0, 4), "pad:")) {
            std::string_view b = w.substr(4);
            // "@vehicle|scanner": the situations; ":tap" / ":hold": the gesture.
            std::uint8_t ctx = 0;
            bool okCtx = true;
            if (const auto at = b.find('@'); at != std::string_view::npos) {
                std::string_view list = b.substr(at + 1);
                b = Trim(b.substr(0, at));
                while (!list.empty()) {
                    const auto bar = list.find('|');
                    const auto n = Trim(list.substr(0, bar));
                    list = bar == std::string_view::npos ? std::string_view{} : list.substr(bar + 1);
                    std::uint8_t c = 0;
                    for (const auto& cn : CtxNames()) if (Same(n, cn.name)) c = cn.ctx;
                    if (!c) okCtx = false;
                    ctx |= c;
                }
            }
            Gesture g = Gesture::Raw;
            if (const auto colon = b.find(':'); colon != std::string_view::npos) {
                const auto gw = Trim(b.substr(colon + 1));
                b = Trim(b.substr(0, colon));
                if (Same(gw, "tap")) g = Gesture::Tap;
                else if (Same(gw, "hold")) g = Gesture::Hold;
                else okCtx = false;
            }
            const auto plus = b.find('+');
            const std::uint32_t mod = plus == std::string_view::npos ? 0 : PadCode(Trim(b.substr(0, plus)));
            const std::uint32_t pad = PadCode(Trim(plus == std::string_view::npos ? b : b.substr(plus + 1)));
            if (okCtx && pad && (plus == std::string_view::npos || mod)) { out.push_back({ 0, pad, mod, false, true, true, ctx, g }); continue; }
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

// The raw "pressed" state of one controller alternative (its gesture aside): its button, with its chord modifier for a
// chord, and for a plain button no chord modifier held.
inline bool PadOn(const Alt& a, std::uint32_t pad, std::uint32_t modifiers)
{
    if (!a.pad || !(pad & a.pad)) return false;
    if (a.mod) return (pad & a.mod) != 0;
    return !(pad & modifiers & ~a.pad);  // plain: only while no chord modifier is held
}

// Held: any alternative. keyHeld(vk) answers for keys and mouse buttons; pad is the controller's bit set now.
// "default" is the action's own key (defaultVk) and, when the game binds one, its controller button (defaultPad).
// ctx: the situation now (one kCtx* bit); gestures[i]: the tap / hold result of alternative i this frame.
template <class KeyHeld>
bool Held(const Binding& b, int defaultVk, std::uint32_t pad, std::uint32_t modifiers, KeyHeld keyHeld, std::uint32_t defaultPad = 0,
    std::uint8_t ctx = kCtxAll, const std::vector<bool>* gestures = nullptr)
{
    for (std::size_t i = 0; i < b.size(); ++i) {
        const auto& a = b[i];
        if (a.useDefault) {
            if (a.defaultKeyboard && defaultVk && keyHeld(defaultVk)) return true;
            if (a.defaultController && defaultPad && (pad & defaultPad) && !(pad & modifiers & ~defaultPad)) return true;
            continue;
        }
        if (a.vk) { if (keyHeld(a.vk)) return true; continue; }
        if (!InCtx(a, ctx)) continue;
        if (a.gesture != Gesture::Raw) { if (gestures && i < gestures->size() && (*gestures)[i]) return true; continue; }
        if (PadOn(a, pad, modifiers)) return true;
    }
    return false;
}

// The controller's analog state (2026-10-24, the user: analog stick input for physical VTOL): sticks -1 .. 1, triggers 0 .. 1.
struct Axes { double lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0; };
inline constexpr double kStickDeadzone = 0.24;    // XInput's own left-stick deadzone (7849 of 32767)
inline constexpr double kTriggerDeadzone = 0.12;  // XInput's trigger threshold (30 of 255)
// 0 inside the deadzone, then rising to 1 at full deflection (so the first movement past it starts from 0, not a jump).
inline double Past(double v, double deadzone)
{
    if (!std::isfinite(v) || v <= deadzone) return 0.0;
    return v >= 1.0 ? 1.0 : (v - deadzone) / (1.0 - deadzone);
}
// How far one controller bit is pressed: a stick direction or trigger by its deflection, 0 for anything else.
inline double AnalogAmount(std::uint32_t bit, const Axes& a)
{
    switch (bit) {
    case kLUp: return Past(a.ly, kStickDeadzone);
    case kLDown: return Past(-a.ly, kStickDeadzone);
    case kLLeft: return Past(-a.lx, kStickDeadzone);
    case kLRight: return Past(a.lx, kStickDeadzone);
    case kRUp: return Past(a.ry, kStickDeadzone);
    case kRDown: return Past(-a.ry, kStickDeadzone);
    case kRLeft: return Past(-a.rx, kStickDeadzone);
    case kRRight: return Past(a.rx, kStickDeadzone);
    case kLT: return Past(a.lt, kTriggerDeadzone);
    case kRT: return Past(a.rt, kTriggerDeadzone);
    default: return 0.0;
    }
}
// Held, as an amount 0 .. 1: a key, a button, a chord or a gesture counts as 1; a plain stick direction or trigger as its
// deflection (the strongest alternative wins). Same rules as Held otherwise.
template <class KeyHeld>
double Amount(const Binding& b, int defaultVk, std::uint32_t pad, std::uint32_t modifiers, KeyHeld keyHeld, std::uint32_t defaultPad,
    std::uint8_t ctx, const std::vector<bool>* gestures, const Axes& axes)
{
    double out = 0.0;
    for (std::size_t i = 0; i < b.size(); ++i) {
        const auto& a = b[i];
        if (a.useDefault) {
            if (a.defaultKeyboard && defaultVk && keyHeld(defaultVk)) return 1.0;
            if (a.defaultController && defaultPad && !(pad & modifiers & ~defaultPad))
                out = std::max(out, AnalogAmount(defaultPad, axes) > 0 ? AnalogAmount(defaultPad, axes) : (pad & defaultPad) ? 1.0 : 0.0);
            continue;
        }
        if (a.vk) { if (keyHeld(a.vk)) return 1.0; continue; }
        if (!a.pad || !InCtx(a, ctx)) continue;
        if (a.gesture != Gesture::Raw) { if (gestures && i < gestures->size() && (*gestures)[i]) out = std::max(out, 1.0); continue; }
        if (!a.mod && AnalogAmount(a.pad, axes) > 0.0) {
            if (!(pad & modifiers & ~a.pad)) out = std::max(out, AnalogAmount(a.pad, axes));
            continue;
        }
        if (PadOn(a, pad, modifiers)) out = std::max(out, 1.0);
    }
    return out;
}

// The controller buttons whose GAME action this binding takes over in situation ctx (inputgate blocks the game's event
// for them, 2026-10-24: a mod button must never also do the game's thing). A chord's second button counts only while its
// modifier is held; the modifier itself cannot be saved (the game acts on its press), which is why the defaults use none.
inline std::uint32_t GameBlockBits(const Binding& b, std::uint8_t ctx, std::uint32_t pad)
{
    std::uint32_t out = 0;
    for (const auto& a : b) {
        if (a.useDefault || a.vk || !a.pad || !InCtx(a, ctx)) continue;
        if (a.mod && !(pad & a.mod)) continue;
        out |= a.pad;
    }
    return out & kPadPressBits;
}
// A controller bit -> the code the game's input events carry (XInput's mask; 9 / 10 the triggers), 0 for a stick direction.
inline std::uint32_t GameCode(std::uint32_t bit)
{
    if (bit == kLT) return 9;
    if (bit == kRT) return 10;
    return bit <= 0xFFFFu ? bit : 0;
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
// How a controller's buttons are named on screen. Bindings are always written with the Xbox names.
enum class PadStyle { Xbox, PlayStation };
inline std::string PadLabel(std::uint32_t bit, PadStyle style)
{
    if (style == PadStyle::Xbox) return PadLabel(bit);
    struct N { std::uint32_t bit; const char* name; };
    static const N kPs[] = { { kA, "Cross" }, { kB, "Circle" }, { kX, "Square" }, { kY, "Triangle" }, { kLB, "L1" }, { kRB, "R1" },
        { kLT, "L2" }, { kRT, "R2" }, { kLS, "L3" }, { kRS, "R3" }, { kStart, "Options" }, { kView, "Create" } };
    for (const auto& n : kPs) if (n.bit == bit) return n.name;
    return PadLabel(bit);
}
// Which device's inputs a prompt names: the one in use (games switch with the last input), or both.
enum class Show { Both, Keyboard, Pad };
// "N" with the keyboard in use, "LB+DUp" (or "L1+DUp") with the controller; the other device's inputs when the one in use
// has none bound, so a prompt never reads "unbound" while something works.
inline std::string Label(const Binding& b, int defaultVk, std::uint32_t defaultPad, Show show, PadStyle style, std::uint8_t ctx = kCtxAll)
{
    auto collect = [&](bool pad) {
        std::string s;
        auto add = [&](const std::string& t) {
            if (t.empty() || s == t || s.find(" / " + t) != std::string::npos || s.rfind(t + " / ", 0) == 0) return;
            if (!s.empty()) s += " / ";
            s += t;
        };
        for (const auto& a : b) {
            if (a.useDefault) {
                if (!pad && a.defaultKeyboard && defaultVk) add(KeyLabel(defaultVk));
                if (pad && a.defaultController && defaultPad) add(PadLabel(defaultPad, style));
            }
            else if (!pad && a.vk) add(KeyLabel(a.vk));
            else if (pad && a.pad && InCtx(a, ctx))
                add((a.mod ? PadLabel(a.mod, style) + "+" + PadLabel(a.pad, style) : PadLabel(a.pad, style)) + (a.gesture == Gesture::Hold ? " (hold)" : ""));
        }
        return s;
    };
    std::string s;
    if (show == Show::Both) {
        s = collect(false);
        const auto p = collect(true);
        if (!p.empty()) s += (s.empty() ? "" : " / ") + p;
    }
    else {
        s = collect(show == Show::Pad);
        if (s.empty()) s = collect(show != Show::Pad);
    }
    return s.empty() ? "unbound" : s;
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
        if (a.useDefault) { add(a.defaultKeyboard && defaultVk ? KeyLabel(defaultVk) : std::string()); add(a.defaultController && defaultPad ? PadLabel(defaultPad) : std::string()); }
        else if (a.vk) add(KeyLabel(a.vk));
        else if (a.pad) add(a.mod ? PadLabel(a.mod) + "+" + PadLabel(a.pad) : PadLabel(a.pad));
    }
    return s.empty() ? "unbound" : s;
}

// Persistence syntax, including split defaults so remapping one device preserves
// the other device's live game/default binding.
inline std::string Spec(const Binding& binding)
{
    std::string out;
    for (const auto& a : binding) {
        std::string part;
        if (a.useDefault) part=a.defaultKeyboard?(a.defaultController?"default":"default-key"):"default-pad";
        else if (a.vk) part=KeyLabel(a.vk);
        else if (a.pad) {
            part="Pad:"+(a.mod?PadLabel(a.mod)+"+":"")+PadLabel(a.pad);
            if (a.gesture==Gesture::Tap) part+=":tap"; else if (a.gesture==Gesture::Hold) part+=":hold";
            if (a.ctx) {
                // The widest names first, so "vehicle" is written instead of its three parts.
                std::uint8_t left=a.ctx; std::string list;
                for (const char* n : { "vehicle", "flying", "foot", "ground", "hover", "vtol", "scanner", "passenger" })
                    for (const auto& cn : CtxNames()) if (std::string_view(n)==cn.name && (left & cn.ctx)==cn.ctx) { left&=std::uint8_t(~cn.ctx); list+=(list.empty()?"":"|")+std::string(n); }
                part+="@"+list;
            }
        }
        if (!part.empty()) {if(!out.empty())out+=", ";out+=part;}
    }
    return out.empty()?"none":out;
}
inline Binding RemapDevice(const Binding& original, Alt replacement)
{
    const bool controller=replacement.pad!=0;
    Binding out;
    for (auto a : original) {
        if (a.useDefault) {
            if(controller)a.defaultController=false;else a.defaultKeyboard=false;
            if(a.defaultKeyboard||a.defaultController)out.push_back(a);
        }
        else if (controller ? a.vk!=0 : a.pad!=0) out.push_back(a);
    }
    out.push_back(replacement);
    return out;
}
// The game's Controls page shows each action as numbered slots per device (main, alt), keyboard/mouse apart from the
// controller. A slot is one alternative of that device with "default" resolved to the key / button it stands for.
struct Input {
    int vk = 0;
    std::uint32_t pad = 0, mod = 0;
    bool Empty() const { return !vk && !pad; }
    bool IsPad() const { return pad != 0; }
    bool operator==(const Input& o) const { return vk == o.vk && pad == o.pad && mod == o.mod; }
};
struct Slot { std::size_t index; Input in; bool fromDefault; };
inline std::vector<Slot> DeviceSlots(const Binding& b, bool pad, int defaultVk, std::uint32_t defaultPad)
{
    std::vector<Slot> out;
    for (std::size_t i = 0; i < b.size(); ++i) {
        const auto& a = b[i];
        if (a.useDefault) {
            if (!pad && a.defaultKeyboard && defaultVk) out.push_back({ i, { defaultVk, 0, 0 }, true });
            if (pad && a.defaultController && defaultPad) out.push_back({ i, { 0, defaultPad, 0 }, true });
        }
        else if (!pad && a.vk) out.push_back({ i, { a.vk, 0, 0 }, false });
        else if (pad && a.pad) out.push_back({ i, { 0, a.pad, a.mod }, false });
    }
    return out;
}
// The device's slot-th input becomes `in` (empty: cleared); a slot past the end is appended. The other device is kept.
inline Binding SetSlot(const Binding& b, bool pad, int slot, Input in, int defaultVk, std::uint32_t defaultPad)
{
    Binding out = b;
    const auto slots = DeviceSlots(b, pad, defaultVk, defaultPad);
    const Alt explicitAlt{ in.vk, in.pad, in.mod, false };
    if (slot < 0 || std::size_t(slot) >= slots.size()) {
        if (!in.Empty()) out.push_back(explicitAlt);
        return out;
    }
    const auto& s = slots[std::size_t(slot)];
    if (s.fromDefault) {
        auto& a = out[s.index];
        (pad ? a.defaultController : a.defaultKeyboard) = false;
        const bool keep = a.defaultKeyboard || a.defaultController;
        if (!keep) out.erase(out.begin() + std::ptrdiff_t(s.index));
        if (!in.Empty()) out.insert(out.begin() + std::ptrdiff_t(s.index), explicitAlt);
        return out;
    }
    if (in.Empty()) out.erase(out.begin() + std::ptrdiff_t(s.index));
    else {
        // A new button keeps the slot's situations and gesture ("Pad:DLeft:hold@vehicle" rebound stays a vehicle hold).
        Alt next = explicitAlt;
        if (next.pad && out[s.index].pad) { next.ctx = out[s.index].ctx; next.gesture = out[s.index].gesture; }
        out[s.index] = next;
    }
    return out;
}
// Every alternative that is `in` removed; a "default" that stands for it gives up that device only.
inline Binding Without(const Binding& b, Input in, int defaultVk, std::uint32_t defaultPad)
{
    Binding out;
    for (auto a : b) {
        if (a.useDefault) {
            if (!in.IsPad() && a.defaultKeyboard && defaultVk == in.vk) a.defaultKeyboard = false;
            if (in.IsPad() && !in.mod && a.defaultController && defaultPad == in.pad) a.defaultController = false;
            if (a.defaultKeyboard || a.defaultController) out.push_back(a);
            continue;
        }
        if (Input{ a.vk, a.pad, a.mod } == in) continue;
        out.push_back(a);
    }
    return out;
}

struct Capture
{
    enum class Kind { Wait, Key, Pad, Cancel, Default, Clear };
    struct Result {Kind kind=Kind::Wait;Alt alt;};
    bool armed=false;std::uint32_t firstPad=0;
    Result Update(int key,std::uint32_t pad)
    {
        if(!armed){if(!key&&!pad)armed=true;return {};}
        if(key==27)return {Kind::Cancel,{}};
        if(key==8)return {Kind::Default,{}};
        if(key==46)return {Kind::Clear,{}};
        if(key)return {Kind::Key,{key,0,0,false}};
        if(!firstPad&&pad)firstPad=pad&(~pad+1u);
        if(firstPad){
            const auto extra=pad&~firstPad;
            if(extra&&(pad&firstPad))return {Kind::Pad,{0,extra&(~extra+1u),firstPad,false}};
            if(!(pad&firstPad))return {Kind::Pad,{0,firstPad,0,false}};
        }
        return {};
    }
};

// The device the prompts name (2026-10-24, the user: change "only when a button from a source is used"). Mouse moves,
// stick moves and held buttons never switch it; a fresh press from each device in the same tick keeps the current one.
enum class Device { Keyboard, Pad };
// The controller buttons newly pressed between two states.
inline std::uint32_t PadPresses(std::uint32_t prev, std::uint32_t now) { return now & ~prev & kPadPressBits; }
struct DeviceTracker
{
    Device device = Device::Keyboard;
    // True when the device changed.
    bool Update(bool keyPress, bool padPress)
    {
        const Device next = keyPress == padPress ? device : padPress ? Device::Pad : Device::Keyboard;
        const bool changed = next != device;
        device = next;
        return changed;
    }
};
}
