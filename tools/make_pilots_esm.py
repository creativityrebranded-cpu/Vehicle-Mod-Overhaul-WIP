"""Builds "FVO_NPCPilots.esm" (until 0.72.0 "SeamlessPilots.esm"): a small plugin with one quest ("FVO_NPCPilotsQuest", start-game-enabled) that holds
6 pilot ref aliases and 6 vehicle ref aliases, and 6 packages, one per pilot alias: "activate the vehicle alias".

The plugin (FlyingVehicleOverhaul.dll) fills the aliases at run time with the engine's own ForceRefTo, so the NPC runs a
QUEST package (which outranks the NPC's own schedule) that walks it to the vehicle and activates it (the game's get-in).
Works for any actor, follower or not. Records are copied in shape from Starfield.esm:
  quest   <- SQ_Passengers (001D307E) / DialogueCFTheKey_RandomIdles_LastNova02 (000B2283) for the alias layout
  package <- SQ_ActivateAlarm (002062C8): template "Activate" (0007976C), QNAM as UC08_Hadrian_Stage600_Activate_UtilityPanel
  PTDA type 4 = reference alias (value = alias id)

python scripts/make_pilots_esm.py <Starfield.esm> <out.esm>
Local ids: quest 0x800, packages 0x801..0x80C, activate quest 0x810, its packages 0x811..0x81C.

2026-10-29 (the user: more NPC vehicles at once): 12 pilots. Pilots 0..5 keep their alias ids (pilot 0..5, vehicle 6..11) so a save
made with the 6-pilot plugin keeps its fills; pilots 6..11 use aliases 12..17 (pilot) and 18..23 (vehicle). pilotquest.cpp maps
the same way (PilotAlias / VehicleAlias).
"""
import struct
import sys
import zlib

PILOTS = 256  # must equal fvolimits::kVehicles (native-vehicle-mod/plugin/src/fvolimits.h)
# Slot blocks (first slot, count): each block's pilot aliases, then its vehicle aliases, after the blocks before it. Slots 0..5 are the
# first plugin's (pilot 0..5, vehicle 6..11), 6..11 the 12-slot one's (12..17, 18..23), 12..63 the 64-slot one's (24..75, 76..127),
# 64.. the 256-slot one's (128.., then its vehicle aliases):
# a save made with an older plugin keeps its fills. pilotquest.cpp maps the same way.
BLOCKS = [(0, 6), (6, 6), (12, 52), (64, PILOTS - 64)]


def _block(k):
    base = 0
    for first, count in BLOCKS:
        if first <= k < first + count:
            return base, first, count
        base += 2 * count
    raise ValueError(k)


def pilot_alias(k):
    base, first, count = _block(k)
    return base + (k - first)


def vehicle_alias(k):
    base, first, count = _block(k)
    return base + count + (k - first)


# Package form ids: slots 0..11 keep 0x801.. / 0x811..; slots 12..63 at 0x900.. / 0xA00..; slots 64.. at 0xB00.. / 0xC00.. (2026-10-29).
def pkg_id(k):
    return 0x801 + k if k < 12 else 0x900 + (k - 12) if k < 64 else 0xB00 + (k - 64)


def act_pkg_id(k):
    return 0x811 + k if k < 12 else 0xA00 + (k - 12) if k < 64 else 0xC00 + (k - 64)


ALIASES = sorted([(pilot_alias(k), True, k) for k in range(PILOTS)] + [(vehicle_alias(k), False, k) for k in range(PILOTS)])
assert [a for a, _, _ in ALIASES] == list(range(2 * PILOTS))
QUEST_ID = 0x800
PKG_BASE = 0x801
ACT_QUEST_ID = 0x810
ACT_PKG_BASE = 0x811
MAP_SEEN_ID = 0xD00  # global: the Traffic Manager's first-time message was shown in this playthrough (saved with the game)
F_COMPRESSED, F_ESM = 0x40000, 0x1


def sub(t, v):
    return t + struct.pack("<H", len(v)) + v


def record(rtype, fid, body, hdr_tail):
    return rtype + struct.pack("<II", len(body), 0) + struct.pack("<I", fid) + hdr_tail + body


def read_subs(body):
    i, out = 0, []
    while i + 6 <= len(body):
        n = struct.unpack_from("<H", body, i + 4)[0]
        out.append((body[i:i + 4], body[i + 6:i + 6 + n]))
        i += 6 + n
    return out


def find_record(d, want, fid):
    def walk(a, b):
        i = a
        while i < b:
            if d[i:i + 4] == b"GRUP":
                sz = struct.unpack_from("<I", d, i + 4)[0]
                r = walk(i + 24, i + sz)
                if r:
                    return r
                i += sz
                continue
            sz, fl, rid = struct.unpack_from("<III", d, i + 4)
            if d[i:i + 4] == want and rid == fid:
                body = d[i + 24:i + 24 + sz]
                return zlib.decompress(body[4:]) if fl & F_COMPRESSED else body
            i += 24 + sz
        return None
    return walk(24 + struct.unpack_from("<I", d, 4)[0], len(d))


def main(src, dst):
    d = open(src, "rb").read()
    tail = struct.pack("<IHH", 0, 0x245, 0)  # timestamp, form version 581, vc
    # the package: MQ201_StarbornPkg (Travel template 0x2CB0, location = a reference alias), our quest and the vehicle alias,
    # stopping within 3 m. (The first version cloned SQ_ActivateAlarm, "Activate": the NPC held that package yet never walked to a
    # vehicle target, but did walk to a plain object, 2026-10-17 23:29.) The plugin boards the NPC once it is near (Mount).
    pk = read_subs(find_record(d, b"PACK", 0x00113E28))
    packs = b""
    for k in range(PILOTS):
        out = []
        for t, v in pk:
            if t == b"EDID":
                v = b"FVO_PilotGetIn%d\x00" % k
            elif t == b"QNAM":
                v = struct.pack("<I", 0x01000000 | QUEST_ID)
            elif t == b"PLDT":
                v = struct.pack("<IIfI", 8, vehicle_alias(k), 0.5, 0)  # reference alias (vehicle), radius 0.5 m (3 m left the NPC 5 m from the door)
            elif t == b"PKDT":
                # Ignore Combat (0x00100000): the flag that separates Starfield.esm's *_IgnoreCombat packages from their twins
                # (UC04_Hadrian_RunToSpaceport 00803000 vs _IgnoreCombat 00903000; LC088_Captain_SandboxAtEndMarker 08800000 vs 08900000).
                # 2026-10-21, the user: the get-in should outrank the fight instead of calming the NPC first.
                v = struct.pack("<I", struct.unpack_from("<I", v)[0] | 0x00100000) + v[4:]
            out.append((t, v))
        packs += record(b"PACK", 0x01000000 | pkg_id(k), b"".join(sub(t, v) for t, v in out), tail)

    # the quest: SQ_Passengers' header fields, then the aliases
    body = sub(b"EDID", b"FVO_NPCPilotsQuest\0")
    body += sub(b"DNAM", struct.pack("<III", 0x111, 0, 0))
    body += sub(b"NEXT", b"")
    body += sub(b"ANAM", struct.pack("<I", PILOTS * 2))
    for a, pilot, k in ALIASES:
        body += sub(b"ALST", struct.pack("<I", a))
        body += sub(b"ALID", (b"Pilot%d\0" % k) if pilot else (b"Vehicle%d\0" % k))
        body += sub(b"FNAM", struct.pack("<I", 0x00100002))  # optional (may stay empty), as the vanilla aliases' 0x100000
        body += sub(b"ALFG", struct.pack("<I", 0))
        if pilot:
            body += sub(b"ALPC", struct.pack("<I", 0x01000000 | pkg_id(k)))
        body += sub(b"VTCK", struct.pack("<I", 0))
        body += sub(b"ALED", b"")
    quest = record(b"QUST", 0x01000000 | QUEST_ID, body, tail)

    # Second quest (a NEW form, so existing saves are untouched): the same pilot / vehicle aliases, but its pilot alias carries an ACTIVATE
    # package (SQ_ActivateAlarm's: template Activate 0007976C, target = the vehicle alias of this quest). Decompile 2026-10-18: the Activate
    # procedure (id 0x11) runs FUN_1419E5CF0, which for a vehicle target calls the animated board routine FUN_141993710(actor, vehicle, 0)
    # first and only falls back to the gated walk-in FUN_141951FB0. The plugin fills it once the NPC has walked up (the Travel package alone
    # never boards).
    pa = read_subs(find_record(d, b"PACK", 0x002062C8))
    packs2 = b""
    for k in range(PILOTS):
        out = []
        for t, v in pa:
            if t == b"EDID":
                v = b"FVO_PilotActivate%d\x00" % k
            if t == b"PKCU":
                out.append((b"QNAM", struct.pack("<I", 0x01000000 | ACT_QUEST_ID)))
            if t == b"PTDA":
                v = struct.pack("<III", 4, vehicle_alias(k), 0)
            out.append((t, v))
        packs2 += record(b"PACK", 0x01000000 | act_pkg_id(k), b"".join(sub(t, v) for t, v in out), tail)
    body2 = sub(b"EDID", b"FVO_NPCPilotsActivateQuest\x00")
    body2 += sub(b"DNAM", struct.pack("<III", 0x111, 0, 0))
    body2 += sub(b"NEXT", b"")
    body2 += sub(b"ANAM", struct.pack("<I", PILOTS * 2))
    for a, pilot, k in ALIASES:
        body2 += sub(b"ALST", struct.pack("<I", a))
        body2 += sub(b"ALID", (b"ActPilot%d\x00" % k) if pilot else (b"ActVehicle%d\x00" % k))
        body2 += sub(b"FNAM", struct.pack("<I", 0x00100002))
        body2 += sub(b"ALFG", struct.pack("<I", 0))
        if pilot:
            body2 += sub(b"ALPC", struct.pack("<I", 0x01000000 | act_pkg_id(k)))
        body2 += sub(b"VTCK", struct.pack("<I", 0))
        body2 += sub(b"ALED", b"")
    quest2 = record(b"QUST", 0x01000000 | ACT_QUEST_ID, body2, tail)

    def group(label, recs):
        return b"GRUP" + struct.pack("<I", 24 + len(recs)) + label + struct.pack("<IIHH", 0, 0, 0x245, 0) + recs

    seen = record(b"GLOB", 0x01000000 | MAP_SEEN_ID, sub(b"EDID", b"FVO_TrafficManagerSeen\0") + sub(b"FLTV", struct.pack("<f", 0.0)), tail)
    groups = group(b"GLOB", seen) + group(b"QUST", quest + quest2) + group(b"PACK", packs + packs2)
    nrec = 1 + 2 + PILOTS * 2 + 3  # 1 global + 2 quests + 2 x PILOTS packages + 3 groups
    assert pkg_id(11) < ACT_QUEST_ID and act_pkg_id(11) < 0x820 and pkg_id(63) < 0xA00 and act_pkg_id(63) < 0xB00
    assert PILOTS <= 256 and pkg_id(PILOTS - 1) < 0xC00 and act_pkg_id(PILOTS - 1) < 0xD00
    hedr_ver = struct.unpack_from("<f", d, 24 + 6)[0]
    items = [(b"HEDR", struct.pack("<fII", hedr_ver, nrec, MAP_SEEN_ID + 1)),
             (b"CNAM", b"Flying Vehicle Overhaul (FVO)\0"),
             (b"MAST", b"Starfield.esm\0"), (b"DATA", struct.pack("<Q", 0))]
    tes4 = b"".join(sub(t, v) for t, v in items)
    header = b"TES4" + struct.pack("<II", len(tes4), F_ESM) + d[12:24]
    open(dst, "wb").write(header + tes4 + groups)
    print("quest 1, packages %d, %d bytes" % (PILOTS, len(header) + len(tes4) + len(groups)))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
