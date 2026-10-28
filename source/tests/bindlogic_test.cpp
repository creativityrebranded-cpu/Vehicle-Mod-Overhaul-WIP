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

    {
        const auto remappedKey=RemapDevice(Parse("default, Pad:LB+A"),{'K',0,0,false});
        down={'T'};
        check(!Held(remappedKey,'T',0,0,key,kRT),"menu keyboard remap releases the old default key");
        down={'K'};
        check(Held(remappedKey,'T',0,0,key,kRT),"keyboard remap accepts the new key");
        down.clear();
        check(Held(remappedKey,'T',kRT,0,key,kRT),"keyboard remap preserves the live default controller binding");
        check(Held(remappedKey,'T',kLB|kA,kLB,key,kRT),"keyboard remap preserves explicit controller chords");
        const auto remappedPad=RemapDevice(Parse("default, Mouse4"),{0,kB,kLB,false});
        down={'T'};
        check(Held(remappedPad,'T',0,0,key,kRT),"controller remap preserves the default keyboard binding");
        down.clear();
        check(!Held(remappedPad,'T',kRT,0,key,kRT)&&Held(remappedPad,'T',kB|kLB,kLB,key,kRT),"controller remap replaces the old controller binding");
        std::string roundTripBad;
        const auto roundTrip=Parse(Spec(remappedPad),&roundTripBad);
        check(roundTripBad.empty()&&Spec(roundTrip)==Spec(remappedPad),"split defaults and chords survive persistence and reload");
        check(Spec(Parse(Spec(Parse(",, Pad:A"))))==Spec(Parse(",, Pad:A")),"the comma key survives binding serialization");
        Capture capture;
        check(capture.Update(13,kA).kind==Capture::Kind::Wait&&!capture.armed,"capture ignores the key or button used to select the row");
        capture.Update(0,0);
        auto result=capture.Update('K',0);
        check(result.kind==Capture::Kind::Key&&result.alt.vk=='K',"after release the next keyboard input is captured");
        Capture chordCapture;chordCapture.Update(0,0);
        check(chordCapture.Update(0,kLB).kind==Capture::Kind::Wait,"controller first button waits for a chord or release");
        result=chordCapture.Update(0,kLB|kA);
        check(result.kind==Capture::Kind::Pad&&result.alt.mod==kLB&&result.alt.pad==kA,"controller chord captures its modifier and second button");
        Capture plain;plain.Update(0,0);plain.Update(0,kB);result=plain.Update(0,0);
        check(result.kind==Capture::Kind::Pad&&result.alt.pad==kB&&!result.alt.mod,"releasing a single controller button binds it without a chord");
        Capture cancel;cancel.Update(0,0);
        check(cancel.Update(27,0).kind==Capture::Kind::Cancel&&cancel.Update(8,0).kind==Capture::Kind::Default&&cancel.Update(46,0).kind==Capture::Kind::Clear,
            "capture supports cancel, restore action defaults, and clear action");
    }
    {
        // The game's Controls page: main / alt slots per device, "default" resolved, moves between actions.
        const auto both = Parse("default, Mouse4, Pad:LB+A");
        const auto kb = DeviceSlots(both, false, 'N', kY), pad = DeviceSlots(both, true, 'N', kY);
        check(kb.size() == 2 && kb[0].in.vk == 'N' && kb[0].fromDefault && kb[1].in.vk == 0x05, "keyboard slots: default key first, then Mouse4");
        check(pad.size() == 2 && pad[0].in.pad == kY && pad[1].in.pad == kA && pad[1].in.mod == kLB, "controller slots: the game's button, then the chord");
        const auto main = SetSlot(both, false, 0, { 'K', 0, 0 }, 'N', kY);
        const auto kb2 = DeviceSlots(main, false, 'N', kY), pad2 = DeviceSlots(main, true, 'N', kY);
        check(kb2.size() == 2 && kb2[0].in.vk == 'K' && kb2[1].in.vk == 0x05, "setting the main key replaces the default key in place");
        check(pad2.size() == 2 && pad2[0].in.pad == kY, "setting a key keeps the default controller button");
        const auto alt = SetSlot(Parse("T"), false, 1, { 'G', 0, 0 }, 'T', 0);
        check(DeviceSlots(alt, false, 'T', 0).size() == 2 && DeviceSlots(alt, false, 'T', 0)[1].in.vk == 'G', "an empty alt slot is appended");
        const auto cleared = SetSlot(both, true, 1, {}, 'N', kY);
        check(DeviceSlots(cleared, true, 'N', kY).size() == 1 && DeviceSlots(cleared, false, 'N', kY).size() == 2, "clearing a controller slot keeps the keyboard");
        const auto moved = Without(Parse("default, Pad:LB+A"), { 'N', 0, 0 }, 'N', kY);
        check(DeviceSlots(moved, false, 'N', kY).empty() && DeviceSlots(moved, true, 'N', kY).size() == 2, "a key moved away from a default drops only the keyboard default");
        const auto movedPad = Without(Parse("default, Pad:LB+A"), { 0, kA, kLB }, 'N', kY);
        check(DeviceSlots(movedPad, true, 'N', kY).size() == 1 && DeviceSlots(movedPad, false, 'N', kY).size() == 1, "a chord moved away leaves the rest");
        std::string roundBad;
        check(Spec(Parse(Spec(main), &roundBad)) == Spec(main) && roundBad.empty(), "slot edits survive persistence");
    }
    {
        // Prompts follow the device in use; PlayStation names for a DualShock / DualSense.
        const auto summon = Parse("default, Pad:LB+DUp");
        check(Label(summon, 'N', 0, Show::Keyboard, PadStyle::Xbox) == "N", "keyboard in use: only the key");
        check(Label(summon, 'N', 0, Show::Pad, PadStyle::Xbox) == "LB+DUp", "controller in use: only the chord");
        check(Label(summon, 'N', 0, Show::Pad, PadStyle::PlayStation) == "L1+DUp", "PlayStation names");
        check(Label(Parse("default"), 'N', 0, Show::Pad, PadStyle::Xbox) == "unbound", "missing controller input stays visibly unbound");
        check(Label(Parse("default, Pad:A"), 'E', kY, Show::Pad, PadStyle::PlayStation) == "Triangle / Cross", "the game's button and ours, PlayStation names");
        check(Label(summon, 'N', 0, Show::Both, PadStyle::Xbox) == "N / LB+DUp", "both devices for the settings list");
    }
    {
        // The prompt device changes only on a fresh button press from the other device (no flicker).
        DeviceTracker d;
        check(d.device == Device::Keyboard, "prompts start on the keyboard");
        check(d.Update(false, true) && d.device == Device::Pad, "a controller press switches to the controller");
        check(!d.Update(false, false) && d.device == Device::Pad, "no press keeps the controller");
        check(!d.Update(true, true) && d.device == Device::Pad, "both devices pressed in one tick keep the current one");
        check(d.Update(true, false) && d.device == Device::Keyboard, "a key or mouse button press switches back");
        check(!d.Update(true, false), "repeated key presses do not count as a change");
        check(PadPresses(0, kLRight | kRUp) == 0, "moving a stick is not a press");
        check(PadPresses(0, kA | kLDown) == kA, "a button pressed while the stick moves is a press");
        check(PadPresses(kA, kA) == 0, "a held button is not a new press");
        check(PadPresses(0, kRT) == kRT && PadPresses(kRT, kRT | kLT) == kLT, "a trigger press counts as a button");
        DeviceTracker native;
        check(native.Observe(3,true,true)&&native.device==Device::Pad,"native controller source resolves a simultaneous keyboard polling alias");
        check(!native.Observe(3,false,false)&&native.device==Device::Pad,"the last native event cannot replay a device change");
        check(native.Observe(4,true,true)&&native.device==Device::Keyboard,"a later native keyboard press wins even when both poll flags are set");
        check(native.Observe(7,false,false)&&native.device==Device::Pad,"native gamepad presses work without XInput polling");
        check(native.Observe(7,true,false)&&native.device==Device::Keyboard,"keyboard fallback still works when the native hook has no new event");
        check(KeyboardMouseKey('W')&&KeyboardMouseKey(1)&&!KeyboardMouseKey(0xC3)&&!KeyboardMouseKey(0xDA),"gamepad virtual keys never impersonate keyboard or mouse activity");
    }
    {
        // Layout 3: situations and tap / hold gestures (the approved no-chord map).
        std::string bad3;
        const auto scan = Parse("default, Pad:DLeft:tap@vehicle, Pad:Y@scanner", &bad3);
        check(bad3.empty() && scan.size() == 3 && scan[1].ctx == kCtxVehicle && scan[1].gesture == Gesture::Tap && scan[2].ctx == kCtxScanner,
            "situations and gestures parse");
        check(Spec(scan) == "default, Pad:DLeft:tap@vehicle, Pad:Y@scanner", "situations and gestures write back the same");
        check(Spec(Parse("Pad:X@flying|passenger")) == "Pad:X@flying|passenger" && Parse("Pad:X@flying")[0].ctx == (kCtxHover | kCtxVtol),
            "flying is hover and VTOL");
        std::string bad4;
        check(Parse("Pad:Y@nowhere, Pad:Y:spin", &bad4).empty() && bad4 == "Pad:Y@nowhere, Pad:Y:spin", "an unknown situation or gesture is reported");
        const auto summon3 = Parse("default, Pad:DUp@vehicle, Pad:B@scanner");
        auto none3 = [](int) { return false; };
        check(Held(summon3, 'N', kDUp, 0, none3, 0, kCtxGround) && !Held(summon3, 'N', kDUp, 0, none3, 0, kCtxScanner), "DUp counts only in a vehicle");
        check(Held(summon3, 'N', kB, 0, none3, 0, kCtxScanner) && !Held(summon3, 'N', kB, 0, none3, 0, kCtxGround), "B counts only in the scanner, never as the game's exit");
        check(Label(summon3, 'N', 0, Show::Pad, PadStyle::Xbox, kCtxGround) == "DUp" && Label(summon3, 'N', 0, Show::Pad, PadStyle::Xbox, kCtxScanner) == "B",
            "the prompt names the button of the situation now");
        check(Label(Parse("Pad:DLeft:hold@vehicle"), 0, 0, Show::Pad, PadStyle::Xbox, kCtxGround) == "DLeft (hold)", "a hold says so");
        // Tap and hold on one button never both fire.
        GestureState tap, hold;
        bool tapFired = false, holdFired = false;
        for (std::uint64_t ms = 0; ms <= 200; ms += 20) { tapFired |= tap.Update(true, Gesture::Tap, ms); holdFired |= hold.Update(true, Gesture::Hold, ms); }
        tapFired |= tap.Update(false, Gesture::Tap, 220); holdFired |= hold.Update(false, Gesture::Hold, 220);
        check(tapFired && !holdFired, "a short press is the tap only");
        tapFired = holdFired = false;
        for (std::uint64_t ms = 1000; ms <= 1700; ms += 20) { tapFired |= tap.Update(true, Gesture::Tap, ms); holdFired |= hold.Update(true, Gesture::Hold, ms); }
        tapFired |= tap.Update(false, Gesture::Tap, 1720); holdFired |= hold.Update(false, Gesture::Hold, 1720);
        check(holdFired && !tapFired, "a long press is the hold only");
        check(!tap.Update(false, Gesture::Tap, 2000), "a tap fires once");
        const auto own = Parse("default, Pad:DLeft:hold@vehicle");
        std::vector<bool> on = { false, true };
        check(Held(own, 0, kDLeft, 0, none3, 0, kCtxGround, &on) && !Held(own, 0, kDLeft, 0, none3, 0, kCtxGround, nullptr), "a gesture alternative follows its gesture, not the raw button");
        // The game's action is switched off only on the mod's buttons of the situation now.
        check(GameBlockBits(summon3, kCtxGround, 0) == kDUp && GameBlockBits(summon3, kCtxScanner, 0) == kB && GameBlockBits(summon3, kCtxFoot, 0) == 0,
            "blocked buttons follow the situation");
        check(GameBlockBits(Parse("Pad:LB+B"), kCtxGround, 0) == 0 && GameBlockBits(Parse("Pad:LB+B"), kCtxGround, kLB) == kB,
            "a chord's second button is blocked while its modifier is held");
        check(GameBlockBits(Parse("Pad:LStickUp@flying"), kCtxHover, kLUp) == 0, "stick directions are never blocked");
        check(GameCode(kLT) == 9 && GameCode(kRT) == 10 && GameCode(kA) == 0x1000 && GameCode(kLUp) == 0, "controller bits to the game's codes");
        // Rebinding a slot on the Controls page keeps its situation and gesture.
        const auto rebound = SetSlot(own, true, 0, Input{ 0, kDRight, 0 }, 0, 0);
        check(rebound.size() == 2 && rebound[1].pad == kDRight && rebound[1].ctx == kCtxVehicle && rebound[1].gesture == Gesture::Hold, "a rebound slot keeps its situation and gesture");
    }
    {
        // Analog amounts (the physical VTOL roll and throttle).
        auto none = [](int) { return false; };
        auto dKey = [](int vk) { return vk == 'D'; };
        const auto right = Parse("default, Pad:LStickRight");
        Axes ax; ax.lx = 0.62;
        check(std::fabs(Amount(right, 'D', kLRight, 0, none, 0, kCtxHover, nullptr, ax) - 0.5) < 1e-9, "a stick pushed past the deadzone gives its share");
        ax.lx = 0.2;
        check(Amount(right, 'D', 0, 0, none, 0, kCtxHover, nullptr, ax) == 0.0, "inside the deadzone: nothing");
        ax.lx = 1.0;
        check(Amount(right, 'D', kLRight, 0, none, 0, kCtxHover, nullptr, ax) == 1.0, "full deflection: all of it");
        ax.lx = 0.3;
        check(Amount(right, 'D', 0, 0, dKey, 0, kCtxHover, nullptr, ax) == 1.0, "a key is always all of it");
        ax = {}; ax.rt = 0.56;
        check(std::fabs(Amount(Parse("Pad:RT"), 0, kRT, 0, none, 0, kCtxHover, nullptr, ax) - 0.5) < 1e-9, "a trigger is analog");
        check(Amount(Parse("Pad:A@vehicle"), 0, kA, 0, none, 0, kCtxHover, nullptr, ax) == 1.0, "a button is all or nothing");
        ax = {}; ax.ly = 0.9;
        check(Amount(Parse("Pad:LStickUp@flying"), 0, kLUp, 0, none, 0, kCtxGround, nullptr, ax) == 0.0, "an analog binding counts only in its situations");
        check(Past(-1.0, kStickDeadzone) == 0.0 && Past(NAN, kStickDeadzone) == 0.0, "the other direction and bad input give nothing");
    }
    std::printf("Bindings: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
